// SPDX-License-Identifier: Apache-2.0
//
// A library that runtime_vtable_reload_test.cpp loads, unloads and replaces with another
// build of it: CT_RELOAD_NAMESPACE names its type and its function, so that each build
// has names of its own.
#if defined(_WIN32)
#define CT_RELOAD_EXPORT extern "C" __declspec(dllexport)
#else
#define CT_RELOAD_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace CT_RELOAD_NAMESPACE
{
    struct Widget
    {
        virtual ~Widget() = default;
        virtual int value() const
        {
            return 1;
        }
    };

    Widget widget;

#if defined(_WIN32)
    __declspec(dllexport)
#else
    __attribute__((visibility("default")))
#endif
    int
    describe(int value)
    {
        return widget.value() + value;
    }
} // namespace CT_RELOAD_NAMESPACE

CT_RELOAD_EXPORT void* ct_reload_object()
{
    return &CT_RELOAD_NAMESPACE::widget;
}

CT_RELOAD_EXPORT void* ct_reload_target()
{
    return reinterpret_cast<void*>(&CT_RELOAD_NAMESPACE::describe);
}
