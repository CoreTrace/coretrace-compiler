// SPDX-License-Identifier: Apache-2.0
//
// A library whose constructor, which runs while dlopen holds the loader's lock, waits for a
// thread that makes traced virtual calls, as a thread pool's start does (#223). The program
// that loads it, runtime_loader_lock_test.cpp, exports the runtime's entry points to it.
#include <thread>

extern "C" void __ct_vcall_trace(void* this_ptr, void* target, const char* site,
                                 const char* static_type);

namespace
{
    struct Widget
    {
        virtual ~Widget() = default;
        virtual int value() const
        {
            return 5;
        }
    };

    Widget widget;
} // namespace

extern "C" __attribute__((visibility("default"))) int ct_trace_constructor_target(int value)
{
    return widget.value() + value;
}

namespace
{
    __attribute__((constructor)) void ct_trace_constructor()
    {
        std::thread(
            []
            {
                __ct_vcall_trace(&widget, reinterpret_cast<void*>(&ct_trace_constructor_target),
                                 "constructor:1", "<unknown>");
                // A target that no module holds, as a corrupted vtable gives.
                int* block = new int(0);
                __ct_vcall_trace(&widget, block, "constructor:2", "<unknown>");
                delete block;
            })
            .join();
    }
} // namespace
