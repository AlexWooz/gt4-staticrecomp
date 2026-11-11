// TASK-090 crash-race diagnostic: when GT4_CRASH_DUMP=1, an access violation in the game process prints the machine
// context and the guest EE state to stderr before the process dies, then lets the default handler terminate it.
//
// It exists to tell an emulator bug from a guest-program bug. The host-clock round's first handler walked the guest
// stack and read the EE thread's live registers into its report, which made a worker-thread fault look like recompiled
// guest code; this version reports the faulting thread, the faulting host address and the host backtrace first, and the
// guest state only as context for the thread that actually runs the guest.
#pragma once

namespace gt4::crash_dump {

// Called once from the game entry point; nothing happens unless GT4_CRASH_DUMP is set to a non-empty value other than
// "0".
void install_from_environment();

}  // namespace gt4::crash_dump
