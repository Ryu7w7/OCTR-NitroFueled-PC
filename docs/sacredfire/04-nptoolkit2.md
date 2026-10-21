# libSceNpToolkit2: route decision

Measured: the eboot imports 80 C++ entry points (sce::Toolkit::NP::V2::...: Core::init/term,
Request/Response/value-type constructors, destructors, operator=, Response<T>::get(), Session,
Friend, Commerce, Auth, UserProfile requests). libSceNpToolkit2.prx (914 KB) imports 303 symbols from
28 libraries (NpTus 41, NpMatching2 35, libkernel 33, LibcInternal 32, NpWebApi 20, ...).

First hit at run time: `UserProfile::NpProfiles::NpProfiles()` on a heap object. The value and
Response types are C++ objects with vtables and layouts we do not have; stubbing their constructors
would need those layouts from the PRX's own code anyway.

Decision (2026-10-09): route A, link the dump's own libSceNpToolkit2.prx natively
(`link_modules.py --modules libc.prx libSceFios2.prx libSceNpToolkit2.prx`). Result: the 80 entry
points bind to the real code (290 native bindings), the PRX's LibcInternal imports bind to libc.prx,
its libkernel/Posix imports to the runtime, and its Np/Http/dialog imports resolve lazily: only
functions it actually calls need a contract. Its module_start ran, created three threads
(Toolkit::NP::Main, Notifications, AbortThread) and registered its NetCtl/NpManager/UserService
callbacks. Nothing of the ~235 Np imports has been called yet besides those registrations.
Cheap to reverse: dropping the module from the link and stubbing the 80 entry points remains possible.
