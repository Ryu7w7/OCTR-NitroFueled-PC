// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Lab: CPU time per named thread (Common::SetCurrentThreadName registers each one), to see what
// the native renderer, shadPS4's command processor and the game's own threads each cost. The
// native report prints it every 5 seconds; "game+rest" is the process minus the named threads.

#include <string>

namespace BbCpu {

/// Counts the calling thread under `name` (threads of one name are added together).
void Register(const char* name);

/// "bb:NativeCapture 23%, shadPS4:GpuCommandProcessor 41%, ..., game+rest 120% (process 210%)"
/// since the last call, in percent of one core.
std::string Report();

} // namespace BbCpu
