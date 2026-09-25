// SPDX-License-Identifier: Apache-2.0
//
// __ct_objc_track with the Apple Objective-C runtime. The tracked class is created at
// run time, so it belongs to no system image. Every test releases its objects, leaving
// the allocation table as it found it.
#include "ct_runtime_alloc_internal.h"

#include <gtest/gtest.h>

#include <objc/message.h>
#include <objc/runtime.h>

namespace
{
    Class trackedClass()
    {
        static Class cls = []
        {
            Class created =
                objc_allocateClassPair(objc_getClass("NSObject"), "CtObjcTrackingTestNode", 0);
            objc_registerClassPair(created);
            return created;
        }();
        return cls;
    }

    id send(id receiver, const char* selector)
    {
        return reinterpret_cast<id (*)(id, SEL)>(objc_msgSend)(receiver,
                                                               sel_registerName(selector));
    }

    id makeInstance(Class cls)
    {
        return send(send(reinterpret_cast<id>(cls), "alloc"), "init");
    }

    bool isTracked(const void* object)
    {
        size_t size = 0;
        size_t reqSize = 0;
        const char* site = nullptr;
        unsigned char state = 0;
        ct_lock_acquire();
        const int found = ct_table_lookup(object, &size, &reqSize, &site, &state);
        ct_lock_release();
        return found == 1 && state == CT_ENTRY_USED;
    }

    TEST(ObjcTracking, TracksAnObjectUntilItsDeallocation)
    {
        Class cls = trackedClass();
        id object = makeInstance(cls);
        __ct_objc_track(object, cls, "test:objc");
        EXPECT_TRUE(isTracked(object));

        send(object, "release");
        EXPECT_FALSE(isTracked(object));
    }

    // A class cluster's +alloc returns a placeholder of another class.
    TEST(ObjcTracking, IgnoresAnObjectOfAnotherClassThanTheReceiver)
    {
        id object = makeInstance(trackedClass());
        __ct_objc_track(object, objc_getClass("NSObject"), "test:objc");
        EXPECT_FALSE(isTracked(object));
        send(object, "release");
    }

    TEST(ObjcTracking, IgnoresInstancesOfSystemClasses)
    {
        Class cls = objc_getClass("NSObject");
        id object = makeInstance(cls);
        __ct_objc_track(object, cls, "test:objc");
        EXPECT_FALSE(isTracked(object));
        send(object, "release");
    }

    // NSProxy's subclasses and other root classes never reach -[NSObject dealloc], where
    // the runtime sees an object go: tracking them would report every one as a leak.
    TEST(ObjcTracking, IgnoresClassesOutsideTheNSObjectHierarchy)
    {
        static Class root = []
        {
            Class created = objc_allocateClassPair(nullptr, "CtObjcTrackingTestRoot", 0);
            objc_registerClassPair(created);
            return created;
        }();
        id object = class_createInstance(root, 0);
        __ct_objc_track(object, root, "test:objc");
        EXPECT_FALSE(isTracked(object));
        object_dispose(object);
    }

    TEST(ObjcTracking, TracksNothingWhileAllocationTrackingIsOff)
    {
        const bool allocEnabled = (ct_get_features() & CT_FEATURE_ALLOC) != 0;
        ct_set_enabled(CT_FEATURE_ALLOC, 0);
        Class cls = trackedClass();
        id object = makeInstance(cls);
        __ct_objc_track(object, cls, "test:objc");
        ct_set_enabled(CT_FEATURE_ALLOC, allocEnabled);

        EXPECT_FALSE(isTracked(object));
        send(object, "release");
    }
} // namespace
