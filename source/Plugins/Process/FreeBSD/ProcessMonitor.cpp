//===-- ProcessMonitor.cpp ------------------------------------ -*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

// C Includes
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <sys/ptrace.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>

// C++ Includes
// Other libraries and framework includes
#include "lldb/Core/Error.h"
#include "lldb/Core/RegisterValue.h"
#include "lldb/Core/Scalar.h"
#include "lldb/Host/Host.h"
#include "lldb/Target/Thread.h"
#include "lldb/Target/RegisterContext.h"
#include "lldb/Utility/PseudoTerminal.h"


#include "POSIXThread.h"
#include "ProcessFreeBSD.h"
#include "ProcessPOSIXLog.h"
#include "ProcessMonitor.h"

extern "C" {
      extern char ** environ;
 }

using namespace lldb;
using namespace lldb_private;

// We disable the tracing of ptrace calls for integration builds to
// avoid the additional indirection and checks.
#ifndef LLDB_CONFIGURATION_BUILDANDINTEGRATION
// Wrapper for ptrace to catch errors and log calls.

const char *
Get_PT_IO_OP(int op)
{
    switch (op) {
        case PIOD_READ_D:  return "READ_D";
        case PIOD_WRITE_D: return "WRITE_D";
        case PIOD_READ_I:  return "READ_I";
        case PIOD_WRITE_I: return "WRITE_I";
        default:           return "Unknown op";
    }
}

// Wrapper for ptrace to catch errors and log calls.
// Note that ptrace sets errno on error because -1 is reserved as a valid result.
extern long
PtraceWrapper(int req, lldb::pid_t pid, void *addr, int data,
              const char* reqName, const char* file, int line)
{
    long int result;

    Log *log (ProcessPOSIXLog::GetLogIfAllCategoriesSet(POSIX_LOG_PTRACE));

    if (log) {
        log->Printf("ptrace(%s, %lu, %p, %x) called from file %s line %d",
                    reqName, pid, addr, data, file, line);
        if (req == PT_IO) {
            struct ptrace_io_desc *pi = (struct ptrace_io_desc *) addr;
            
            log->Printf("PT_IO: op=%s offs=%zx size=%ld",
                     Get_PT_IO_OP(pi->piod_op), (size_t)pi->piod_offs, pi->piod_len);
        }
    }

    //PtraceDisplayBytes(req, data);

    errno = 0;
    result = ptrace(req, pid, (caddr_t) addr, data);

    //PtraceDisplayBytes(req, data);

    if (log && errno != 0)
    {
        const char* str;
        switch (errno)
        {
        case ESRCH:  str = "ESRCH"; break;
        case EINVAL: str = "EINVAL"; break;
        case EBUSY:  str = "EBUSY"; break;
        case EPERM:  str = "EPERM"; break;
        default:     str = "<unknown>";
        }
        log->Printf("ptrace() failed; errno=%d (%s)", errno, str);
    }

#ifdef __amd64__
    if (log) {
        if (req == PT_GETREGS) {
            struct reg *r = (struct reg *) addr;

            log->Printf("PT_GETREGS: ip=0x%lx", r->r_rip);
            log->Printf("PT_GETREGS: sp=0x%lx", r->r_rsp);
            log->Printf("PT_GETREGS: bp=0x%lx", r->r_rbp);
            log->Printf("PT_GETREGS: ax=0x%lx", r->r_rax);
        }
    }
#endif
     
    return result;
}

// Wrapper for ptrace when logging is not required.
// Sets errno to 0 prior to calling ptrace.
extern long
PtraceWrapper(int req, lldb::pid_t pid, void *addr, int data)
{
    long result = 0;
    errno = 0;
    result = ptrace(req, pid, (caddr_t)addr, data);
    return result;
}

#define PTRACE(req, pid, addr, data) \
    PtraceWrapper((req), (pid), (addr), (data), #req, __FILE__, __LINE__)
#else
    PtraceWrapper((req), (pid), (addr), (data))
#endif

//------------------------------------------------------------------------------
// Static implementations of ProcessMonitor::ReadMemory and
// ProcessMonitor::WriteMemory.  This enables mutual recursion between these
// functions without needed to go thru the thread funnel.

static size_t
DoReadMemory(lldb::pid_t pid, lldb::addr_t vm_addr, void *buf, size_t size, 
             Error &error)
{
    struct ptrace_io_desc pi_desc;

    pi_desc.piod_op = PIOD_READ_D;
    pi_desc.piod_offs = (void *)vm_addr;
    pi_desc.piod_addr = buf;
    pi_desc.piod_len = size;

    if (PTRACE(PT_IO, pid, (caddr_t)&pi_desc, 0) < 0)
        error.SetErrorToErrno();
    return pi_desc.piod_len;
}

static size_t
DoWriteMemory(lldb::pid_t pid, lldb::addr_t vm_addr, const void *buf, 
              size_t size, Error &error)
{
    struct ptrace_io_desc pi_desc;

    pi_desc.piod_op = PIOD_WRITE_D;
    pi_desc.piod_offs = (void *)vm_addr;
    pi_desc.piod_addr = (void *)buf;
    pi_desc.piod_len = size;

    if (PTRACE(PT_IO, pid, (caddr_t)&pi_desc, 0) < 0)
        error.SetErrorToErrno();
    return pi_desc.piod_len;
}

// Simple helper function to ensure flags are enabled on the given file
// descriptor.
static bool
EnsureFDFlags(int fd, int flags, Error &error)
{
    int status;

    if ((status = fcntl(fd, F_GETFL)) == -1)
    {
        error.SetErrorToErrno();
        return false;
    }

    if (fcntl(fd, F_SETFL, status | flags) == -1)
    {
        error.SetErrorToErrno();
        return false;
    }

    return true;
}

//------------------------------------------------------------------------------
/// @class Operation
/// @brief Represents a ProcessMonitor operation.
///
/// Under FreeBSD, it is not possible to ptrace() from any other thread but the
/// one that spawned or attached to the process from the start.  Therefore, when
/// a ProcessMonitor is asked to deliver or change the state of an inferior
/// process the operation must be "funneled" to a specific thread to perform the
/// task.  The Operation class provides an abstract base for all services the
/// ProcessMonitor must perform via the single virtual function Execute, thus
/// encapsulating the code that needs to run in the privileged context.
class Operation
{
public:
    virtual ~Operation() {}
    virtual void Execute(ProcessMonitor *monitor) = 0;
};

//------------------------------------------------------------------------------
/// @class ReadOperation
/// @brief Implements ProcessMonitor::ReadMemory.
class ReadOperation : public Operation
{
public:
    ReadOperation(lldb::addr_t addr, void *buff, size_t size,
                  Error &error, size_t &result)
        : m_addr(addr), m_buff(buff), m_size(size),
          m_error(error), m_result(result)
        { }

    void Execute(ProcessMonitor *monitor);

private:
    lldb::addr_t m_addr;
    void *m_buff;
    size_t m_size;
    Error &m_error;
    size_t &m_result;
};

void
ReadOperation::Execute(ProcessMonitor *monitor)
{
    lldb::pid_t pid = monitor->GetPID();

    m_result = DoReadMemory(pid, m_addr, m_buff, m_size, m_error);
}

//------------------------------------------------------------------------------
/// @class WriteOperation
/// @brief Implements ProcessMonitor::WriteMemory.
class WriteOperation : public Operation
{
public:
    WriteOperation(lldb::addr_t addr, const void *buff, size_t size,
                   Error &error, size_t &result)
        : m_addr(addr), m_buff(buff), m_size(size),
          m_error(error), m_result(result)
        { }

    void Execute(ProcessMonitor *monitor);

private:
    lldb::addr_t m_addr;
    const void *m_buff;
    size_t m_size;
    Error &m_error;
    size_t &m_result;
};

void
WriteOperation::Execute(ProcessMonitor *monitor)
{
    lldb::pid_t pid = monitor->GetPID();

    m_result = DoWriteMemory(pid, m_addr, m_buff, m_size, m_error);
}

//------------------------------------------------------------------------------
/// @class ReadRegOperation
/// @brief Implements ProcessMonitor::ReadRegisterValue.
class ReadRegOperation : public Operation
{
public:
    ReadRegOperation(lldb::tid_t tid, unsigned offset, unsigned size,
                     RegisterValue &value, bool &result)
        : m_tid(tid), m_offset(offset), m_size(size),
          m_value(value), m_result(result)
        { }

    void Execute(ProcessMonitor *monitor);

private:
    lldb::tid_t m_tid;
    unsigned m_offset;
    unsigned m_size;
    RegisterValue &m_value;
    bool &m_result;
};

void
ReadRegOperation::Execute(ProcessMonitor *monitor)
{
    struct reg regs;
    int rc;

    if ((rc = PTRACE(PT_GETREGS, m_tid, (caddr_t)&regs, 0)) < 0) {
        m_result = false;
    } else {
        if (m_size == sizeof(uintptr_t))
            m_value = *(uintptr_t *)(((caddr_t)&regs) + m_offset);
        else 
            memcpy(&m_value, (((caddr_t)&regs) + m_offset), m_size);
        m_result = true;
    }
}

//------------------------------------------------------------------------------
/// @class WriteRegOperation
/// @brief Implements ProcessMonitor::WriteRegisterValue.
class WriteRegOperation : public Operation
{
public:
    WriteRegOperation(lldb::tid_t tid, unsigned offset,
                      const RegisterValue &value, bool &result)
        : m_tid(tid), m_offset(offset),
          m_value(value), m_result(result)
        { }

    void Execute(ProcessMonitor *monitor);

private:
    lldb::tid_t m_tid;
    unsigned m_offset;
    const RegisterValue &m_value;
    bool &m_result;
};

void
WriteRegOperation::Execute(ProcessMonitor *monitor)
{
    struct reg regs;

    if (PTRACE(PT_GETREGS, m_tid, (caddr_t)&regs, 0) < 0) {
        m_result = false;
        return;
    }
    *(uintptr_t *)(((caddr_t)&regs) + m_offset) = (uintptr_t)m_value.GetAsUInt64();
    if (PTRACE(PT_SETREGS, m_tid, (caddr_t)&regs, 0) < 0)
        m_result = false;
    else
        m_result = true;
}

//------------------------------------------------------------------------------
/// @class ReadGPROperation
/// @brief Implements ProcessMonitor::ReadGPR.
class ReadGPROperation : public Operation
{
public:
    ReadGPROperation(lldb::tid_t tid, void *buf, bool &result)
        : m_tid(tid), m_buf(buf), m_result(result)
        { }

    void Execute(ProcessMonitor *monitor);

private:
    lldb::tid_t m_tid;
    void *m_buf;
    bool &m_result;
};

void
ReadGPROperation::Execute(ProcessMonitor *monitor)
{
    int rc;

    errno = 0;
    rc = PTRACE(PT_GETREGS, m_tid, (caddr_t)m_buf, 0);
    if (errno != 0)
        m_result = false;
    else
        m_result = true;
}

//------------------------------------------------------------------------------
/// @class ReadFPROperation
/// @brief Implements ProcessMonitor::ReadFPR.
class ReadFPROperation : public Operation
{
public:
    ReadFPROperation(lldb::tid_t tid, void *buf, bool &result)
        : m_tid(tid), m_buf(buf), m_result(result)
        { }

    void Execute(ProcessMonitor *monitor);

private:
    lldb::tid_t m_tid;
    void *m_buf;
    bool &m_result;
};

void
ReadFPROperation::Execute(ProcessMonitor *monitor)
{
    if (PTRACE(PT_GETFPREGS, m_tid, (caddr_t)m_buf, 0) < 0)
        m_result = false;
    else
        m_result = true;
}

//------------------------------------------------------------------------------
/// @class WriteGPROperation
/// @brief Implements ProcessMonitor::WriteGPR.
class WriteGPROperation : public Operation
{
public:
    WriteGPROperation(lldb::tid_t tid, void *buf, bool &result)
        : m_tid(tid), m_buf(buf), m_result(result)
        { }

    void Execute(ProcessMonitor *monitor);

private:
    lldb::tid_t m_tid;
    void *m_buf;
    bool &m_result;
};

void
WriteGPROperation::Execute(ProcessMonitor *monitor)
{
    if (PTRACE(PT_SETREGS, m_tid, (caddr_t)m_buf, 0) < 0)
        m_result = false;
    else
        m_result = true;
}

//------------------------------------------------------------------------------
/// @class WriteFPROperation
/// @brief Implements ProcessMonitor::WriteFPR.
class WriteFPROperation : public Operation
{
public:
    WriteFPROperation(lldb::tid_t tid, void *buf, bool &result)
        : m_tid(tid), m_buf(buf), m_result(result)
        { }

    void Execute(ProcessMonitor *monitor);

private:
    lldb::tid_t m_tid;
    void *m_buf;
    bool &m_result;
};

void
WriteFPROperation::Execute(ProcessMonitor *monitor)
{
    if (PTRACE(PT_SETFPREGS, m_tid, (caddr_t)m_buf, 0) < 0)
        m_result = false;
    else
        m_result = true;
}

//------------------------------------------------------------------------------
/// @class ResumeOperation
/// @brief Implements ProcessMonitor::Resume.
class ResumeOperation : public Operation
{
public:
    ResumeOperation(lldb::tid_t tid, uint32_t signo, bool &result) :
        m_tid(tid), m_signo(signo), m_result(result) { }

    void Execute(ProcessMonitor *monitor);

private:
    lldb::tid_t m_tid;
    uint32_t m_signo;
    bool &m_result;
};

void
ResumeOperation::Execute(ProcessMonitor *monitor)
{
    int data = 0;

    if (m_signo != LLDB_INVALID_SIGNAL_NUMBER)
        data = m_signo;

    if (PTRACE(PT_CONTINUE, m_tid, (caddr_t)1, data))
    {
        Log *log (ProcessPOSIXLog::GetLogIfAllCategoriesSet (POSIX_LOG_PROCESS));

        if (log)
            log->Printf ("ResumeOperation (%"  PRIu64 ") failed: %s", m_tid, strerror(errno));
        m_result = false;
    }
    else
        m_result = true;
}

//------------------------------------------------------------------------------
/// @class SingleStepOperation
/// @brief Implements ProcessMonitor::SingleStep.
class SingleStepOperation : public Operation
{
public:
    SingleStepOperation(lldb::tid_t tid, uint32_t signo, bool &result)
        : m_tid(tid), m_signo(signo), m_result(result) { }

    void Execute(ProcessMonitor *monitor);

private:
    lldb::tid_t m_tid;
    uint32_t m_signo;
    bool &m_result;
};

void
SingleStepOperation::Execute(ProcessMonitor *monitor)
{
    int data = 0;

    if (m_signo != LLDB_INVALID_SIGNAL_NUMBER)
        data = m_signo;

    if (PTRACE(PT_STEP, m_tid, NULL, data))
        m_result = false;
    else
        m_result = true;
}

//------------------------------------------------------------------------------
/// @class LwpInfoOperation
/// @brief Implements ProcessMonitor::GetLwpInfo.
class LwpInfoOperation : public Operation
{
public:
    LwpInfoOperation(lldb::tid_t tid, void *info, bool &result, int &ptrace_err)
        : m_tid(tid), m_info(info), m_result(result), m_err(ptrace_err) { }

    void Execute(ProcessMonitor *monitor);

private:
    lldb::tid_t m_tid;
    void *m_info;
    bool &m_result;
    int &m_err;
};

void
LwpInfoOperation::Execute(ProcessMonitor *monitor)
{
    struct ptrace_lwpinfo plwp;

    if (PTRACE(PT_LWPINFO, m_tid, (caddr_t)&plwp, sizeof(plwp))) {
        m_result = false;
        m_err = errno;
    } else {
        memcpy(m_info, &plwp, sizeof(plwp));
        m_result = true;
    }
}

//------------------------------------------------------------------------------
/// @class EventMessageOperation
/// @brief Implements ProcessMonitor::GetEventMessage.
class EventMessageOperation : public Operation
{
public:
    EventMessageOperation(lldb::tid_t tid, unsigned long *message, bool &result)
        : m_tid(tid), m_message(message), m_result(result) { }

    void Execute(ProcessMonitor *monitor);

private:
    lldb::tid_t m_tid;
    unsigned long *m_message;
    bool &m_result;
};

void
EventMessageOperation::Execute(ProcessMonitor *monitor)
{
    struct ptrace_lwpinfo plwp;

    if (PTRACE(PT_LWPINFO, m_tid, (caddr_t)&plwp, sizeof(plwp)))
        m_result = false;
    else {
        if (plwp.pl_flags & PL_FLAG_FORKED) {
            m_message = (unsigned long *)plwp.pl_child_pid;
            m_result = true;
        } else
            m_result = false;
    }
}

//------------------------------------------------------------------------------
/// @class KillOperation
/// @brief Implements ProcessMonitor::BringProcessIntoLimbo.
class KillOperation : public Operation
{
public:
    KillOperation(bool &result) : m_result(result) { }

    void Execute(ProcessMonitor *monitor);

private:
    bool &m_result;
};

void
KillOperation::Execute(ProcessMonitor *monitor)
{
    lldb::pid_t pid = monitor->GetPID();

    if (PTRACE(PT_KILL, pid, NULL, 0))
        m_result = false;
    else
        m_result = true;
}

//------------------------------------------------------------------------------
/// @class DetachOperation
/// @brief Implements ProcessMonitor::BringProcessIntoLimbo.
class DetachOperation : public Operation
{
public:
    DetachOperation(Error &result) : m_error(result) { }

    void Execute(ProcessMonitor *monitor);

private:
    Error &m_error;
};

void
DetachOperation::Execute(ProcessMonitor *monitor)
{
    lldb::pid_t pid = monitor->GetPID();

    if (PTRACE(PT_DETACH, pid, NULL, 0) < 0)
        m_error.SetErrorToErrno();
  
}

ProcessMonitor::OperationArgs::OperationArgs(ProcessMonitor *monitor)
    : m_monitor(monitor)
{
    sem_init(&m_semaphore, 0, 0);
}

ProcessMonitor::OperationArgs::~OperationArgs()
{
    sem_destroy(&m_semaphore);
}

ProcessMonitor::LaunchArgs::LaunchArgs(ProcessMonitor *monitor,
                                       lldb_private::Module *module,
                                       char const **argv,
                                       char const **envp,
                                       const char *stdin_path,
                                       const char *stdout_path,
                                       const char *stderr_path,
                                       const char *working_dir)
    : OperationArgs(monitor),
      m_module(module),
      m_argv(argv),
      m_envp(envp),
      m_stdin_path(stdin_path),
      m_stdout_path(stdout_path),
      m_stderr_path(stderr_path),
      m_working_dir(working_dir) { }

ProcessMonitor::LaunchArgs::~LaunchArgs()
{ }

ProcessMonitor::AttachArgs::AttachArgs(ProcessMonitor *monitor,
                                       lldb::pid_t pid)
    : OperationArgs(monitor), m_pid(pid) { }

ProcessMonitor::AttachArgs::~AttachArgs()
{ }

//------------------------------------------------------------------------------
/// The basic design of the ProcessMonitor is built around two threads.
///
/// One thread (@see SignalThread) simply blocks on a call to waitpid() looking
/// for changes in the debugee state.  When a change is detected a
/// ProcessMessage is sent to the associated ProcessFreeBSD instance.  This thread
/// "drives" state changes in the debugger.
///
/// The second thread (@see OperationThread) is responsible for two things 1)
/// launching or attaching to the inferior process, and then 2) servicing
/// operations such as register reads/writes, stepping, etc.  See the comments
/// on the Operation class for more info as to why this is needed.
ProcessMonitor::ProcessMonitor(ProcessPOSIX *process,
                               Module *module,
                               const char *argv[],
                               const char *envp[],
                               const char *stdin_path,
                               const char *stdout_path,
                               const char *stderr_path,
                               const char *working_dir,
                               lldb_private::Error &error)
    : m_process(static_cast<ProcessFreeBSD *>(process)),
      m_operation_thread(LLDB_INVALID_HOST_THREAD),
      m_monitor_thread(LLDB_INVALID_HOST_THREAD),
      m_pid(LLDB_INVALID_PROCESS_ID),
      m_terminal_fd(-1),
      m_operation(0)
{
    std::unique_ptr<LaunchArgs> args(new LaunchArgs(this, module, argv, envp,
                                     stdin_path, stdout_path, stderr_path,
                                     working_dir));
    

    sem_init(&m_operation_pending, 0, 0);
    sem_init(&m_operation_done, 0, 0);

    StartLaunchOpThread(args.get(), error);
    if (!error.Success())
        return;

WAIT_AGAIN:
    // Wait for the operation thread to initialize.
    if (sem_wait(&args->m_semaphore))
    {
        if (errno == EINTR)
            goto WAIT_AGAIN;
        else
        {
            error.SetErrorToErrno();
            return;
        }
    }

    // Check that the launch was a success.
    if (!args->m_error.Success())
    {
        StopOpThread();
        error = args->m_error;
        return;
    }

    // Finally, start monitoring the child process for change in state.
    m_monitor_thread = Host::StartMonitoringChildProcess(
        ProcessMonitor::MonitorCallback, this, GetPID(), true);
    if (!IS_VALID_LLDB_HOST_THREAD(m_monitor_thread))
    {
        error.SetErrorToGenericError();
        error.SetErrorString("Process launch failed.");
        return;
    }
}

ProcessMonitor::ProcessMonitor(ProcessPOSIX *process,
                               lldb::pid_t pid,
                               lldb_private::Error &error)
    : m_process(static_cast<ProcessFreeBSD *>(process)),
      m_operation_thread(LLDB_INVALID_HOST_THREAD),
      m_monitor_thread(LLDB_INVALID_HOST_THREAD),
      m_pid(pid),
      m_terminal_fd(-1),
      m_operation(0)
{
    sem_init(&m_operation_pending, 0, 0);
    sem_init(&m_operation_done, 0, 0);


    std::unique_ptr<AttachArgs> args(new AttachArgs(this, pid));

    StartAttachOpThread(args.get(), error);
    if (!error.Success())
        return;

WAIT_AGAIN:
    // Wait for the operation thread to initialize.
    if (sem_wait(&args->m_semaphore))
    {
        if (errno == EINTR)
            goto WAIT_AGAIN;
        else
        {
            error.SetErrorToErrno();
            return;
        }
    }

    // Check that the attach was a success.
    if (!args->m_error.Success())
    {
        StopOpThread();
        error = args->m_error;
        return;
    }

    // Finally, start monitoring the child process for change in state.
    m_monitor_thread = Host::StartMonitoringChildProcess(
        ProcessMonitor::MonitorCallback, this, GetPID(), true);
    if (!IS_VALID_LLDB_HOST_THREAD(m_monitor_thread))
    {
        error.SetErrorToGenericError();
        error.SetErrorString("Process attach failed.");
        return;
    }
}

ProcessMonitor::~ProcessMonitor()
{
    StopMonitor();
}

//------------------------------------------------------------------------------
// Thread setup and tear down.
void
ProcessMonitor::StartLaunchOpThread(LaunchArgs *args, Error &error)
{
    static const char *g_thread_name = "lldb.process.freebsd.operation";

    if (IS_VALID_LLDB_HOST_THREAD(m_operation_thread))
        return;

    m_operation_thread =
        Host::ThreadCreate(g_thread_name, LaunchOpThread, args, &error);
}

void *
ProcessMonitor::LaunchOpThread(void *arg)
{
    LaunchArgs *args = static_cast<LaunchArgs*>(arg);

    if (!Launch(args)) {
        sem_post(&args->m_semaphore);
        return NULL;
    }

    ServeOperation(args);
    return NULL;
}

bool
ProcessMonitor::Launch(LaunchArgs *args)
{
    ProcessMonitor *monitor = args->m_monitor;
    ProcessFreeBSD &process = monitor->GetProcess();
    const char **argv = args->m_argv;
    const char **envp = args->m_envp;
    const char *stdin_path = args->m_stdin_path;
    const char *stdout_path = args->m_stdout_path;
    const char *stderr_path = args->m_stderr_path;
    const char *working_dir = args->m_working_dir;

    lldb_utility::PseudoTerminal terminal;
    const size_t err_len = 1024;
    char err_str[err_len];
    lldb::pid_t pid;

    // Propagate the environment if one is not supplied.
    if (envp == NULL || envp[0] == NULL)
        envp = const_cast<const char **>(environ);

    if ((pid = terminal.Fork(err_str, err_len)) == -1)
    {
        args->m_error.SetErrorToGenericError();
        args->m_error.SetErrorString("Process fork failed.");
        goto FINISH;
    }

    // Recognized child exit status codes.
    enum {
        ePtraceFailed = 1,
        eDupStdinFailed,
        eDupStdoutFailed,
        eDupStderrFailed,
        eChdirFailed,
        eExecFailed
    };

    // Child process.
    if (pid == 0)
    {
        // Trace this process.
        if (PTRACE(PT_TRACE_ME, 0, NULL, 0) < 0)
            exit(ePtraceFailed);

        // Do not inherit setgid powers.
        setgid(getgid());

        // Let us have our own process group.
        setpgid(0, 0);

        // Dup file descriptors if needed.
        //
        // FIXME: If two or more of the paths are the same we needlessly open
        // the same file multiple times.
        if (stdin_path != NULL && stdin_path[0])
            if (!DupDescriptor(stdin_path, STDIN_FILENO, O_RDONLY))
                exit(eDupStdinFailed);

        if (stdout_path != NULL && stdout_path[0])
            if (!DupDescriptor(stdout_path, STDOUT_FILENO, O_WRONLY | O_CREAT))
                exit(eDupStdoutFailed);

        if (stderr_path != NULL && stderr_path[0])
            if (!DupDescriptor(stderr_path, STDERR_FILENO, O_WRONLY | O_CREAT))
                exit(eDupStderrFailed);

        // Change working directory
        if (working_dir != NULL && working_dir[0])
          if (0 != ::chdir(working_dir))
              exit(eChdirFailed);

        // Execute.  We should never return.
        execve(argv[0],
               const_cast<char *const *>(argv),
               const_cast<char *const *>(envp));
        exit(eExecFailed);
    }

    // Wait for the child process to to trap on its call to execve.
    ::pid_t wpid;
    int status;
    if ((wpid = waitpid(pid, &status, 0)) < 0)
    {
        args->m_error.SetErrorToErrno();
        goto FINISH;
    }
    else if (WIFEXITED(status))
    {
        // open, dup or execve likely failed for some reason.
        args->m_error.SetErrorToGenericError();
        switch (WEXITSTATUS(status))
        {
            case ePtraceFailed:
                args->m_error.SetErrorString("Child ptrace failed.");
                break;
            case eDupStdinFailed:
                args->m_error.SetErrorString("Child open stdin failed.");
                break;
            case eDupStdoutFailed:
                args->m_error.SetErrorString("Child open stdout failed.");
                break;
            case eDupStderrFailed:
                args->m_error.SetErrorString("Child open stderr failed.");
                break;
            case eChdirFailed:
                args->m_error.SetErrorString("Child failed to set working directory.");
                break;
            case eExecFailed:
                args->m_error.SetErrorString("Child exec failed.");
                break;
            default:
                args->m_error.SetErrorString("Child returned unknown exit status.");
                break;
        }
        goto FINISH;
    }
    assert(WIFSTOPPED(status) && wpid == pid &&
           "Could not sync with inferior process.");

#ifdef notyet
    // Have the child raise an event on exit.  This is used to keep the child in
    // limbo until it is destroyed.
    if (PTRACE(PTRACE_SETOPTIONS, pid, NULL, PTRACE_O_TRACEEXIT) < 0)
    {
        args->m_error.SetErrorToErrno();
        goto FINISH;
    }
#endif
    // Release the master terminal descriptor and pass it off to the
    // ProcessMonitor instance.  Similarly stash the inferior pid.
    monitor->m_terminal_fd = terminal.ReleaseMasterFileDescriptor();
    monitor->m_pid = pid;

    // Set the terminal fd to be in non blocking mode (it simplifies the
    // implementation of ProcessFreeBSD::GetSTDOUT to have a non-blocking
    // descriptor to read from).
    if (!EnsureFDFlags(monitor->m_terminal_fd, O_NONBLOCK, args->m_error))
        goto FINISH;

    process.SendMessage(ProcessMessage::Attach(pid));

FINISH:
    return args->m_error.Success();
}

void
ProcessMonitor::StartAttachOpThread(AttachArgs *args, lldb_private::Error &error)
{
    static const char *g_thread_name = "lldb.process.freebsd.operation";

    if (IS_VALID_LLDB_HOST_THREAD(m_operation_thread))
        return;

    m_operation_thread =
        Host::ThreadCreate(g_thread_name, AttachOpThread, args, &error);
}

void *
ProcessMonitor::AttachOpThread(void *arg)
{
    AttachArgs *args = static_cast<AttachArgs*>(arg);

    if (!Attach(args))
        return NULL;

    ServeOperation(args);
    return NULL;
}

bool
ProcessMonitor::Attach(AttachArgs *args)
{
    lldb::pid_t pid = args->m_pid;

    ProcessMonitor *monitor = args->m_monitor;
    ProcessFreeBSD &process = monitor->GetProcess();

    if (pid <= 1)
    {
        args->m_error.SetErrorToGenericError();
        args->m_error.SetErrorString("Attaching to process 1 is not allowed.");
        goto FINISH;
    }

    // Attach to the requested process.
    if (PTRACE(PT_ATTACH, pid, NULL, 0) < 0)
    {
        args->m_error.SetErrorToErrno();
        goto FINISH;
    }

    int status;
    if ((status = waitpid(pid, NULL, 0)) < 0)
    {
        args->m_error.SetErrorToErrno();
        goto FINISH;
    }

    process.SendMessage(ProcessMessage::Attach(pid));

FINISH:
    return args->m_error.Success();
}

bool
ProcessMonitor::MonitorCallback(void *callback_baton,
                                lldb::pid_t pid,
                                bool exited,
                                int signal,
                                int status)
{
    ProcessMessage message;
    ProcessMonitor *monitor = static_cast<ProcessMonitor*>(callback_baton);
    ProcessFreeBSD *process = monitor->m_process;
    assert(process);
    bool stop_monitoring;
    struct ptrace_lwpinfo plwp;
    int ptrace_err;

    Log *log (ProcessPOSIXLog::GetLogIfAllCategoriesSet (POSIX_LOG_PROCESS));

    if (exited)
    {
        if (log)
            log->Printf ("ProcessMonitor::%s() got exit signal, tid = %"  PRIu64, __FUNCTION__, pid);
        message = ProcessMessage::Exit(pid, status);
        process->SendMessage(message);
        return pid == process->GetID();
    }

    if (!monitor->GetLwpInfo(pid, &plwp, ptrace_err))
        stop_monitoring = true; // pid is gone.  Bail.
    else {
        switch (plwp.pl_siginfo.si_signo)
        {
        case SIGTRAP:
            message = MonitorSIGTRAP(monitor, &plwp.pl_siginfo, pid);
            break;
            
        default:
            message = MonitorSignal(monitor, &plwp.pl_siginfo, pid);
            break;
        }

        process->SendMessage(message);
        stop_monitoring = message.GetKind() == ProcessMessage::eExitMessage;
    }

    return stop_monitoring;
}

ProcessMessage
ProcessMonitor::MonitorSIGTRAP(ProcessMonitor *monitor,
                               const siginfo_t *info, lldb::pid_t pid)
{
    ProcessMessage message;

    Log *log (ProcessPOSIXLog::GetLogIfAllCategoriesSet (POSIX_LOG_PROCESS));

    assert(monitor);
    assert(info && info->si_signo == SIGTRAP && "Unexpected child signal!");

    switch (info->si_code)
    {
    default:
        assert(false && "Unexpected SIGTRAP code!");
        break;

    case (SIGTRAP /* | (PTRACE_EVENT_EXIT << 8) */):
    {
        // The inferior process is about to exit.  Maintain the process in a
        // state of "limbo" until we are explicitly commanded to detach,
        // destroy, resume, etc.
        unsigned long data = 0;
        if (!monitor->GetEventMessage(pid, &data))
            data = -1;
        if (log)
            log->Printf ("ProcessMonitor::%s() received exit? event, data = %lx, pid = %" PRIu64, __FUNCTION__, data, pid);
        message = ProcessMessage::Limbo(pid, (data >> 8));
        break;
    }

    case 0:
    case TRAP_TRACE:
        if (log)
            log->Printf ("ProcessMonitor::%s() received trace event, pid = %" PRIu64, __FUNCTION__, pid);
        message = ProcessMessage::Trace(pid);
        break;

    case SI_KERNEL:
    case TRAP_BRKPT:
        if (log)
            log->Printf ("ProcessMonitor::%s() received breakpoint event, pid = %" PRIu64, __FUNCTION__, pid);
        message = ProcessMessage::Break(pid);
        break;
    }

    return message;
}

ProcessMessage
ProcessMonitor::MonitorSignal(ProcessMonitor *monitor,
                              const siginfo_t *info, lldb::pid_t pid)
{
    ProcessMessage message;
    int signo = info->si_signo;

    Log *log (ProcessPOSIXLog::GetLogIfAllCategoriesSet (POSIX_LOG_PROCESS));

    // POSIX says that process behaviour is undefined after it ignores a SIGFPE,
    // SIGILL, SIGSEGV, or SIGBUS *unless* that signal was generated by a
    // kill(2) or raise(3).  Similarly for tgkill(2) on FreeBSD.
    //
    // IOW, user generated signals never generate what we consider to be a
    // "crash".
    //
    // Similarly, ACK signals generated by this monitor.
    if (info->si_code == SI_USER)
    {
        if (log)
            log->Printf ("ProcessMonitor::%s() received signal %s with code %s, pid = %d",
                            __FUNCTION__,
                            monitor->m_process->GetUnixSignals().GetSignalAsCString (signo),
                            "SI_USER",
                            info->si_pid);
        if (info->si_pid == getpid())
            return ProcessMessage::SignalDelivered(pid, signo);
        else
            return ProcessMessage::Signal(pid, signo);
    }

    if (log)
        log->Printf ("ProcessMonitor::%s() received signal %s", __FUNCTION__, monitor->m_process->GetUnixSignals().GetSignalAsCString (signo));

    if (signo == SIGSEGV) {
        lldb::addr_t fault_addr = reinterpret_cast<lldb::addr_t>(info->si_addr);
        ProcessMessage::CrashReason reason = GetCrashReasonForSIGSEGV(info);
        return ProcessMessage::Crash(pid, reason, signo, fault_addr);
    }

    if (signo == SIGILL) {
        lldb::addr_t fault_addr = reinterpret_cast<lldb::addr_t>(info->si_addr);
        ProcessMessage::CrashReason reason = GetCrashReasonForSIGILL(info);
        return ProcessMessage::Crash(pid, reason, signo, fault_addr);
    }

    if (signo == SIGFPE) {
        lldb::addr_t fault_addr = reinterpret_cast<lldb::addr_t>(info->si_addr);
        ProcessMessage::CrashReason reason = GetCrashReasonForSIGFPE(info);
        return ProcessMessage::Crash(pid, reason, signo, fault_addr);
    }

    if (signo == SIGBUS) {
        lldb::addr_t fault_addr = reinterpret_cast<lldb::addr_t>(info->si_addr);
        ProcessMessage::CrashReason reason = GetCrashReasonForSIGBUS(info);
        return ProcessMessage::Crash(pid, reason, signo, fault_addr);
    }

    // Everything else is "normal" and does not require any special action on
    // our part.
    return ProcessMessage::Signal(pid, signo);
}

ProcessMessage::CrashReason
ProcessMonitor::GetCrashReasonForSIGSEGV(const siginfo_t *info)
{
    ProcessMessage::CrashReason reason;
    assert(info->si_signo == SIGSEGV);

    reason = ProcessMessage::eInvalidCrashReason;

    switch (info->si_code) 
    {
    default:
        assert(false && "unexpected si_code for SIGSEGV");
        break;
    case SEGV_MAPERR:
        reason = ProcessMessage::eInvalidAddress;
        break;
    case SEGV_ACCERR:
        reason = ProcessMessage::ePrivilegedAddress;
        break;
    }
        
    return reason;
}

ProcessMessage::CrashReason
ProcessMonitor::GetCrashReasonForSIGILL(const siginfo_t *info)
{
    ProcessMessage::CrashReason reason;
    assert(info->si_signo == SIGILL);

    reason = ProcessMessage::eInvalidCrashReason;

    switch (info->si_code)
    {
    default:
        assert(false && "unexpected si_code for SIGILL");
        break;
    case ILL_ILLOPC:
        reason = ProcessMessage::eIllegalOpcode;
        break;
    case ILL_ILLOPN:
        reason = ProcessMessage::eIllegalOperand;
        break;
    case ILL_ILLADR:
        reason = ProcessMessage::eIllegalAddressingMode;
        break;
    case ILL_ILLTRP:
        reason = ProcessMessage::eIllegalTrap;
        break;
    case ILL_PRVOPC:
        reason = ProcessMessage::ePrivilegedOpcode;
        break;
    case ILL_PRVREG:
        reason = ProcessMessage::ePrivilegedRegister;
        break;
    case ILL_COPROC:
        reason = ProcessMessage::eCoprocessorError;
        break;
    case ILL_BADSTK:
        reason = ProcessMessage::eInternalStackError;
        break;
    }

    return reason;
}

ProcessMessage::CrashReason
ProcessMonitor::GetCrashReasonForSIGFPE(const siginfo_t *info)
{
    ProcessMessage::CrashReason reason;
    assert(info->si_signo == SIGFPE);

    reason = ProcessMessage::eInvalidCrashReason;

    switch (info->si_code)
    {
    default:
        assert(false && "unexpected si_code for SIGFPE");
        break;
    case FPE_INTDIV:
        reason = ProcessMessage::eIntegerDivideByZero;
        break;
    case FPE_INTOVF:
        reason = ProcessMessage::eIntegerOverflow;
        break;
    case FPE_FLTDIV:
        reason = ProcessMessage::eFloatDivideByZero;
        break;
    case FPE_FLTOVF:
        reason = ProcessMessage::eFloatOverflow;
        break;
    case FPE_FLTUND:
        reason = ProcessMessage::eFloatUnderflow;
        break;
    case FPE_FLTRES:
        reason = ProcessMessage::eFloatInexactResult;
        break;
    case FPE_FLTINV:
        reason = ProcessMessage::eFloatInvalidOperation;
        break;
    case FPE_FLTSUB:
        reason = ProcessMessage::eFloatSubscriptRange;
        break;
    }

    return reason;
}

ProcessMessage::CrashReason
ProcessMonitor::GetCrashReasonForSIGBUS(const siginfo_t *info)
{
    ProcessMessage::CrashReason reason;
    assert(info->si_signo == SIGBUS);

    reason = ProcessMessage::eInvalidCrashReason;

    switch (info->si_code)
    {
    default:
        assert(false && "unexpected si_code for SIGBUS");
        break;
    case BUS_ADRALN:
        reason = ProcessMessage::eIllegalAlignment;
        break;
    case BUS_ADRERR:
        reason = ProcessMessage::eIllegalAddress;
        break;
    case BUS_OBJERR:
        reason = ProcessMessage::eHardwareError;
        break;
    }

    return reason;
}

void
ProcessMonitor::ServeOperation(OperationArgs *args)
{
    int status;

    ProcessMonitor *monitor = args->m_monitor;

    // We are finised with the arguments and are ready to go.  Sync with the
    // parent thread and start serving operations on the inferior.
    sem_post(&args->m_semaphore);

    for (;;)
    {
        // wait for next pending operation
        sem_wait(&monitor->m_operation_pending);

        monitor->m_operation->Execute(monitor);

        // notify calling thread that operation is complete
        sem_post(&monitor->m_operation_done);
    }
}

void
ProcessMonitor::DoOperation(Operation *op)
{
    Mutex::Locker lock(m_operation_mutex);

    m_operation = op;

    // notify operation thread that an operation is ready to be processed
    sem_post(&m_operation_pending);

    // wait for operation to complete
    sem_wait(&m_operation_done);
}

size_t
ProcessMonitor::ReadMemory(lldb::addr_t vm_addr, void *buf, size_t size,
                           Error &error)
{
    size_t result;
    ReadOperation op(vm_addr, buf, size, error, result);
    DoOperation(&op);
    return result;
}

size_t
ProcessMonitor::WriteMemory(lldb::addr_t vm_addr, const void *buf, size_t size,
                            lldb_private::Error &error)
{
    size_t result;
    WriteOperation op(vm_addr, buf, size, error, result);
    DoOperation(&op);
    return result;
}

bool
ProcessMonitor::ReadRegisterValue(lldb::tid_t tid, unsigned offset, const char* reg_name,
                                  unsigned size, RegisterValue &value)
{
    bool result;
    ReadRegOperation op(tid, offset, size, value, result);
    DoOperation(&op);
    return result;
}

bool
ProcessMonitor::WriteRegisterValue(lldb::tid_t tid, unsigned offset,
                                   const char* reg_name, const RegisterValue &value)
{
    bool result;
    WriteRegOperation op(tid, offset, value, result);
    DoOperation(&op);
    return result;
}

bool
ProcessMonitor::ReadGPR(lldb::tid_t tid, void *buf, size_t buf_size)
{
    bool result;
    ReadGPROperation op(tid, buf, result);
    DoOperation(&op);
    return result;
}

bool
ProcessMonitor::ReadFPR(lldb::tid_t tid, void *buf, size_t buf_size)
{
    bool result;
    ReadFPROperation op(tid, buf, result);
    DoOperation(&op);
    return result;
}

bool
ProcessMonitor::ReadRegisterSet(lldb::tid_t tid, void *buf, size_t buf_size, unsigned int regset)
{
    return false;
}

bool
ProcessMonitor::WriteGPR(lldb::tid_t tid, void *buf, size_t buf_size)
{
    bool result;
    WriteGPROperation op(tid, buf, result);
    DoOperation(&op);
    return result;
}

bool
ProcessMonitor::WriteFPR(lldb::tid_t tid, void *buf, size_t buf_size)
{
    bool result;
    WriteFPROperation op(tid, buf, result);
    DoOperation(&op);
    return result;
}

bool
ProcessMonitor::WriteRegisterSet(lldb::tid_t tid, void *buf, size_t buf_size, unsigned int regset)
{
    return false;
}

bool
ProcessMonitor::ReadThreadPointer(lldb::tid_t tid, lldb::addr_t &value)
{
    return false;
}

bool
ProcessMonitor::Resume(lldb::tid_t tid, uint32_t signo)
{
    bool result;
    Log *log (ProcessPOSIXLog::GetLogIfAllCategoriesSet (POSIX_LOG_PROCESS));

    if (log)
        log->Printf ("ProcessMonitor::%s() resuming thread = %"  PRIu64 " with signal %s", __FUNCTION__, tid,
                                 m_process->GetUnixSignals().GetSignalAsCString (signo));
    ResumeOperation op(tid, signo, result);
    DoOperation(&op);
    if (log)
        log->Printf ("ProcessMonitor::%s() resuming result = %s", __FUNCTION__, result ? "true" : "false");
    return result;
}

bool
ProcessMonitor::SingleStep(lldb::tid_t tid, uint32_t signo)
{
    bool result;
    SingleStepOperation op(tid, signo, result);
    DoOperation(&op);
    return result;
}

bool
ProcessMonitor::BringProcessIntoLimbo()
{
    bool result;
    KillOperation op(result);
    DoOperation(&op);
    return result;
}

bool
ProcessMonitor::GetLwpInfo(lldb::tid_t tid, void *lwpinfo, int &ptrace_err)
{
    bool result;
    LwpInfoOperation op(tid, lwpinfo, result, ptrace_err);
    DoOperation(&op);
    return result;
}

bool
ProcessMonitor::GetEventMessage(lldb::tid_t tid, unsigned long *message)
{
    bool result;
    EventMessageOperation op(tid, message, result);
    DoOperation(&op);
    return result;
}

lldb_private::Error
ProcessMonitor::Detach(lldb::tid_t tid)
{
    lldb_private::Error error;
    if (tid != LLDB_INVALID_THREAD_ID)
    {
        DetachOperation op(error);
        DoOperation(&op);
    }
    return error;
}    

bool
ProcessMonitor::DupDescriptor(const char *path, int fd, int flags)
{
    int target_fd = open(path, flags, 0666);

    if (target_fd == -1)
        return false;

    return (dup2(target_fd, fd) == -1) ? false : true;
}

void
ProcessMonitor::StopMonitoringChildProcess()
{
    lldb::thread_result_t thread_result;

    if (IS_VALID_LLDB_HOST_THREAD(m_monitor_thread))
    {
        Host::ThreadCancel(m_monitor_thread, NULL);
        Host::ThreadJoin(m_monitor_thread, &thread_result, NULL);
        m_monitor_thread = LLDB_INVALID_HOST_THREAD;
    }
}

void
ProcessMonitor::StopMonitor()
{
    StopMonitoringChildProcess();
    StopOpThread();
    sem_destroy(&m_operation_pending);
    sem_destroy(&m_operation_done);

    // Note: ProcessPOSIX passes the m_terminal_fd file descriptor to
    // Process::SetSTDIOFileDescriptor, which in turn transfers ownership of
    // the descriptor to a ConnectionFileDescriptor object.  Consequently
    // even though still has the file descriptor, we shouldn't close it here.
}

// FIXME: On Linux, when a new thread is created, we receive to notifications,
// (1) a SIGTRAP|PTRACE_EVENT_CLONE from the main process thread with the
// child thread id as additional information, and (2) a SIGSTOP|SI_USER from
// the new child thread indicating that it has is stopped because we attached.
// We have no guarantee of the order in which these arrive, but we need both
// before we are ready to proceed.  We currently keep a list of threads which
// have sent the initial SIGSTOP|SI_USER event.  Then when we receive the
// SIGTRAP|PTRACE_EVENT_CLONE notification, if the initial stop has not occurred
// we call ProcessMonitor::WaitForInitialTIDStop() to wait for it.
//
// Right now, the above logic is in ProcessPOSIX, so we need a definition of
// this function in the FreeBSD ProcessMonitor implementation even if it isn't
// logically needed.
//
// We really should figure out what actually happens on FreeBSD and move the
// Linux-specific logic out of ProcessPOSIX as needed.

bool
ProcessMonitor::WaitForInitialTIDStop(lldb::tid_t tid)
{
    return true;
}

void
ProcessMonitor::StopOpThread()
{
    lldb::thread_result_t result;

    if (!IS_VALID_LLDB_HOST_THREAD(m_operation_thread))
        return;

    Host::ThreadCancel(m_operation_thread, NULL);
    Host::ThreadJoin(m_operation_thread, &result, NULL);
    m_operation_thread = LLDB_INVALID_HOST_THREAD;
}
