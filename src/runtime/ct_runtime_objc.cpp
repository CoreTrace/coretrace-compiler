// SPDX-License-Identifier: Apache-2.0
//
// Objective-C object tracking, Apple runtime only. The alloc pass calls __ct_objc_track
// after each allocation it compiles; the object is tracked until the Objective-C runtime
// deallocates it, which every NSObject does through -[NSObject dealloc]: ARC and manual
// -dealloc overrides end by calling super. The Objective-C runtime keeps the object's
// memory and reference count; this file only watches them.
//
// Only programs that call __ct_objc_track pull this file out of the runtime archive, and
// they already link the Objective-C runtime, so C and C++ programs never depend on it.
#include "ct_runtime_alloc_internal.h"

#include <cstring>
#include <malloc/malloc.h>
#include <objc/runtime.h>

namespace
{
    using CtDeallocImp = void (*)(id, SEL);

    Class ct_objc_nsobject = nullptr;
    // The implementation ct_objc_dealloc replaced; null until then, and objects are
    // tracked only once it is set.
    CtDeallocImp ct_objc_nsobject_dealloc = nullptr;

    // Replaces -[NSObject dealloc], so it runs for every object the process deallocates;
    // an untracked one costs a table lookup. The entry goes before the runtime destroys
    // the object: once the memory is freed, a new object may be tracked at its address.
    CT_NOINSTR void ct_objc_dealloc(id self, SEL cmd)
    {
        const size_t size = ct_forget_allocation(self, CT_ALLOC_KIND_OBJC);
        if (size && ct_is_enabled(CT_FEATURE_ALLOC_TRACE))
        {
            ct_log(CTLevel::Info, "{}tracing-objc-dealloc ptr={:p} size={}{}\n",
                   ct_color(CTColor::Cyan), static_cast<void*>(self), size,
                   ct_color(CTColor::Reset));
        }
        ct_objc_nsobject_dealloc(self, cmd);
    }

    // Runs at start-up, before the program can start threads that would deallocate
    // objects while the implementation changes.
    CT_NOINSTR __attribute__((constructor)) void ct_objc_hook_dealloc()
    {
        ct_init_env_once();
        if (!ct_is_enabled(CT_FEATURE_ALLOC))
        {
            return;
        }
        ct_objc_nsobject = objc_getClass("NSObject");
        Method dealloc = ct_objc_nsobject ? class_getInstanceMethod(ct_objc_nsobject,
                                                                    sel_registerName("dealloc"))
                                          : nullptr;
        if (!dealloc)
        {
            return;
        }
        ct_objc_nsobject_dealloc =
            reinterpret_cast<CtDeallocImp>(method_getImplementation(dealloc));
        method_setImplementation(dealloc, reinterpret_cast<IMP>(&ct_objc_dealloc));
    }

    // Classes whose instances the program releases through -[NSObject dealloc]: NSObject
    // subclasses from outside the system frameworks and libraries. System classes may
    // keep an instance for the whole process or release it without -[NSObject dealloc]
    // (NSURL does), and other root classes, such as NSProxy, never reach it.
    CT_NODISCARD CT_NOINSTR bool ct_objc_is_trackable_class(Class cls)
    {
        const char* image = class_getImageName(cls);
        if (image &&
            (std::strncmp(image, "/System/", 8) == 0 || std::strncmp(image, "/usr/lib/", 9) == 0))
        {
            return false;
        }
        Class root = cls;
        while (Class superclass = class_getSuperclass(root))
        {
            root = superclass;
        }
        return root == ct_objc_nsobject;
    }
} // namespace

extern "C"
{
    // Tracks `object` when it is a heap instance of `cls` itself, of a class whose
    // deallocation the runtime sees. A class cluster's +alloc returns a shared placeholder
    // of another class; tracking anything the runtime cannot see go would report it as a
    // leak.
    CT_NOINSTR void __ct_objc_track(void* object, const void* cls, const char* site)
    {
        ct_init_env_once();
        if (!object || !ct_objc_nsobject_dealloc || !ct_is_enabled(CT_FEATURE_ALLOC))
        {
            return;
        }
        Class objectClass = object_getClass(static_cast<id>(object));
        if (static_cast<const void*>(objectClass) != cls ||
            !ct_objc_is_trackable_class(objectClass))
        {
            return;
        }
        const size_t size = malloc_size(object);
        if (!size)
        {
            return;
        }

        ct_track_allocation(object, size, site, CT_ALLOC_KIND_OBJC);
        if (ct_is_enabled(CT_FEATURE_ALLOC_TRACE))
        {
            ct_log(CTLevel::Info,
                   "{}tracing-objc-alloc{} :: tid={} site={} class={} ptr={:p} size={}\n",
                   ct_color(CTColor::Yellow), ct_color(CTColor::Reset), ct_thread_id(),
                   ct_site_name(site), class_getName(objectClass), object, size);
        }
    }
} // extern "C"
