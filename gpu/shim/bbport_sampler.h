// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Lab: a sampling profiler for named threads (Windows). BB_VK_PROFILE=<comma-separated parts of
// thread names> (e.g. "NativeCapture,NativeRecorder"): about every millisecond each such thread
// is suspended, its instruction pointer and a few return addresses (unwound) are noted, and every
// 10 seconds the log gets the hottest code addresses as "exe+0x..." (exclusive: where the thread
// was; inclusive: on its stack), for llvm-symbolizer --obj=sacredfire.exe 0x140000000+offset.

namespace BbSampler {

/// The calling thread, under `name` (Common::SetCurrentThreadName via BbCpu::Register).
void Register(const char* name);

} // namespace BbSampler
