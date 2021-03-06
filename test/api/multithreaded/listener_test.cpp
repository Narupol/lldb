// LLDB test snippet that registers a listener with a process that hits
// a breakpoint.

#include <atomic>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "lldb-headers.h"
#include "common.h"

using namespace lldb;
using namespace std;

void listener_func();
void check_listener(SBDebugger &dbg);

// Listener thread and related variables
atomic<bool> g_done; 
SBListener g_listener("test-listener");
thread g_listener_thread;

void shutdown_listener() {
  g_done.store(true);
  if (g_listener_thread.joinable())
    g_listener_thread.join();
}

void test(SBDebugger &dbg, std::vector<string> args) {
  try {
    g_done.store(false);
    SBTarget target = dbg.CreateTarget(args.at(0).c_str());
    if (!target.IsValid()) throw Exception("invalid target");

    SBBreakpoint breakpoint = target.BreakpointCreateByName("next");
    if (!breakpoint.IsValid()) throw Exception("invalid breakpoint");

    std::unique_ptr<char> working_dir(get_working_dir());

    SBError error;
    SBProcess process = target.Launch(g_listener,
                                      0, 0, 0, 0, 0,
                                      working_dir.get(),
                                      0,
                                      false,
                                      error);
    if (!error.Success())
      throw Exception("Error launching process.");

    /* FIXME: the approach below deadlocks
    SBProcess process = target.LaunchSimple(0, 0, working_dir.get());

    // get debugger listener (which is attached to process by default)
    g_listener = dbg.GetListener();
    */

    // FIXME: because a listener is attached to the process at launch-time,
    // registering the listener below results in two listeners being attached,
    // which is not supported by LLDB.
    // register listener
    // process.GetBroadcaster().AddListener(g_listener,
    //                                   SBProcess::eBroadcastBitStateChanged);

    // start listener thread
    g_listener_thread = thread(listener_func);
    check_listener(dbg);

  } catch (Exception &e) {
    shutdown_listener();
    throw e;
  }
  shutdown_listener();
}
