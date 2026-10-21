# Imports implemented for CTR (chronological)

Format: symbol (library) | what the game does with it | contract | confirmed by.

- _sceKernelSetThreadDtors, _sceKernelSetThreadAtexitCount, _sceKernelSetThreadAtexitReport,
  _sceKernelRtldThreadAtexitIncrement/Decrement, __pthread_cxa_finalize (libkernel) | libc.prx
  module init registers its thread destructor and atexit bookkeeping | callbacks stored; dtors run
  at guest thread exit before key destructors; counters are atomic | STOP seen at libc init, init
  completes afterwards.
- sceKernelGetModuleInfoFromAddr, sceKernelGetModuleInfoForUnwind, sceSysmoduleGetModuleInfoForUnwind,
  __elf_phdr_match_addr, _is_signal_return | libc unwinder/dladdr | ESRCH / no match | imported by
  libc.prx; not yet hit at run time.
- sceKernelIsAddressSanitizerEnabled, sceKernelGetSanitizer*ReplaceExternal | libc heap | 0/NULL |
  imported by libc.prx.
- sceKernelInternalMemoryGetModuleSegmentInfo | libc heap arena sizing | EINVAL, logged | not hit yet.
- sceKernelMlock | 0 | not hit yet. sceLibcHeapGetTraceInfo (libSceLibcInternalExt) | heap tracing off
  | ENOENT | STOP seen, libc init continued.
- __progname (libkernel, data) | "eboot.bin".
- sceKernelConvertUtcToLocaltime/LocaltimeToUtc | host zone via localtime_r | not hit yet.
- sceKernelIsNeoMode (0, BB_NEO=1 for Neo), sceKernelGetCpumode (0: 6 CPUs), sceKernelGetCurrentCpu
  (sched_getcpu % 6) | first eboot calls | STOP seen; game continues.
- Event flags (sceKernelCreate/Delete/Set/Clear/Wait/Poll/CancelEventFlag) | one flag, thousands
  of waits/sets per second (streaming) | AND/OR, CLEAR_ALL/CLEAR_PAT, single/multi waiter, µs
  timeouts updated | counters in the runtime report.
- sceKernelGetdents | directory listing | getdirentries without base | not hit yet.
- sceKernelDirectMemoryQuery | block containing/after offset | not hit yet.
- sceRandomGetRandomNumber | getrandom, ≤64 bytes | not hit yet.
- scePthreadCondattrInit/Destroy, scePthreadGetname, scePthreadGetthreadid, scePthreadCancel (noted,
  deferred cancellation never fires), pthread_get/setschedparam.
- scePadSetLightBar/ResetLightBar | accepted on the open handle.
- sceSysmoduleLoadModuleInternalWithArg | 0x8000xxxx ids through the module counter.
- sceNetCtlInit/Term, sceNetResolverAbort, sceMsgDialogGetResult (OK/button 1).
- PlayGo: GetInstallSpeed, GetProgress (progress == total), GetEta (0), Get/SetToDoList (empty),
  Prefetch, Close, Terminate | STOP seen at GetInstallSpeed.
- libSceJson2: MemAllocator base ctor installs a vtable (dtor, dtor, allocate, deallocate,
  notifyError), Initializer initialize/terminate record the allocator | STOP seen; "initialized with
  the game's allocator (buffer 1024 bytes)".
- SharePlay (init/term/prohibition/connection info dormant), VideoRecording (mem size 64 KiB, open
  fails), ContentExport (init OK, exports fail), GameLiveStreaming, ScreenShot, Np*A variants (signed
  out), trophy destroy, HTTP extras, commerce dialog result/icons, WebBrowserDialog and
  InvitationDialog (close immediately) | STOP seen at sceSharePlayInitialize.
- sceNetCtl*ForNpToolkit, sceNp*StateCallbackForToolkit, sceNpCheckCallbackForLib,
  sceUserService(Un)RegisterEventCallback | the linked libSceNpToolkit2.prx's start-up | same
  contracts as the public functions | STOP seen; the toolkit's threads start afterwards.
