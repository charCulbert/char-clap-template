// AccessKit's macOS library declares Objective-C classes with fixed names
// (AccessKitNode, the first time a screen reader asks for the view's children).
// Classes are global to the host process and every plugin links its own copy
// of the library, so the second plugin to declare one would find the name
// taken, and AccessKit aborts the host when that happens.
//
// This definition takes the library's calls to objc_allocateClassPair: the
// linker binds them to it rather than to libobjc's, and only inside this
// plugin (it is hidden). It adds this plugin's load address to AccessKit's
// class names, so each loaded plugin, even a second copy of one, has its own.
// Not an Objective-C file: <objc/runtime.h> declares the function otherwise.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <string>

extern "C" __attribute__ ((visibility ("hidden"))) void* objc_allocateClassPair (
    void* superclass, const char* name, size_t extraBytes)
{
    using Allocate = void* (*) (void*, const char*, size_t);
    static const auto allocate = reinterpret_cast<Allocate> (
        dlsym (dlopen ("/usr/lib/libobjc.A.dylib", RTLD_LAZY | RTLD_NOLOAD), "objc_allocateClassPair"));
    if (std::strncmp (name, "AccessKit", 9) != 0)
        return allocate (superclass, name, extraBytes);
    Dl_info image {};
    dladdr (reinterpret_cast<void*> (&objc_allocateClassPair), &image);
    char base[32];
    std::snprintf (
        base, sizeof (base), "_%llx", static_cast<unsigned long long> (reinterpret_cast<uintptr_t> (image.dli_fbase)));
    return allocate (superclass, (std::string (name) + base).c_str(), extraBytes); // the runtime copies the name
}
