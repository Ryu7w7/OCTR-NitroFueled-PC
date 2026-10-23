// bbport (macOS): one frame of Metal commands captured to a .gputrace document for Xcode.
// BB_METAL_CAPTURE=<file.gputrace> (also needs METAL_CAPTURE_ENABLED=1): creating <file>.go starts
// the capture at the next presentation and the one after it ends it (the .go file is removed).
#ifdef __APPLE__
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <objc/message.h>
#include <objc/runtime.h>

extern "C" void* MTLCreateSystemDefaultDevice(void);

namespace BbMetalCapture {
namespace {

template <typename R = void*, typename... Args>
R Send(void* object, const char* selector, Args... args) {
    return reinterpret_cast<R (*)(void*, SEL, Args...)>(objc_msgSend)(object, sel_registerName(selector),
                                                                      args...);
}

void* Class(const char* name) {
    return reinterpret_cast<void*>(objc_getClass(name));
}

bool capturing = false;

void Start(const std::string& path) {
    void* device = MTLCreateSystemDefaultDevice();
    void* manager = Send(Class("MTLCaptureManager"), "sharedCaptureManager");
    // MTLCaptureDestinationGPUTraceDocument
    if (!Send<bool, long>(manager, "supportsDestination:", 2)) {
        std::printf("Metal capture: GPU trace documents unavailable (METAL_CAPTURE_ENABLED=1?)\n");
        return;
    }
    void* descriptor = Send(Send(Class("MTLCaptureDescriptor"), "alloc"), "init");
    Send<void, void*>(descriptor, "setCaptureObject:", device);
    Send<void, long>(descriptor, "setDestination:", 2);
    void* ns_path = Send<void*, const char*>(Class("NSString"), "stringWithUTF8String:", path.c_str());
    void* url = Send<void*, void*>(Class("NSURL"), "fileURLWithPath:", ns_path);
    Send<void, void*>(descriptor, "setOutputURL:", url);
    void* error = nullptr;
    if (!Send<bool, void*, void**>(manager, "startCaptureWithDescriptor:error:", descriptor, &error)) {
        void* text = error ? Send(error, "localizedDescription") : nullptr;
        std::printf("Metal capture: not started (%s)\n",
                    text ? Send<const char*>(text, "UTF8String") : "unknown error");
        return;
    }
    capturing = true;
    std::printf("Metal capture: started, writing %s\n", path.c_str());
}

void Stop() {
    Send<void>(Send(Class("MTLCaptureManager"), "sharedCaptureManager"), "stopCapture");
    capturing = false;
    std::printf("Metal capture: done\n");
}

} // namespace
} // namespace BbMetalCapture

extern "C" void bb_metal_capture_present() {
    using namespace BbMetalCapture;
    static const char* path = std::getenv("BB_METAL_CAPTURE");
    if (!path || !*path) {
        return;
    }
    if (capturing) {
        Stop();
        return;
    }
    const std::string trigger = std::string(path) + ".go";
    struct stat st;
    if (stat(trigger.c_str(), &st) == 0) {
        unlink(trigger.c_str());
        Start(path);
    }
}
#endif
