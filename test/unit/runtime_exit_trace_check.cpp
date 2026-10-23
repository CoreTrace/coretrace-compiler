// SPDX-License-Identifier: Apache-2.0
//
// A program whose first traced virtual call comes from the destructor of a global object, which
// exit runs after the destructors of the objects constructed after it: those of the runtime
// that the boxes read must not be among them (#206). Built with the runtime that
// AddressSanitizer instruments, which reports a read of a destroyed object; exits with 0
// otherwise.
#include "ct_runtime_internal.h"

extern "C" void __ct_vcall_trace(void* this_ptr, void* target, const char* site,
                                 const char* static_type);

namespace
{
    struct Widget
    {
        virtual ~Widget() = default;
        virtual int value() const
        {
            return 7;
        }
    };

    Widget widget;

    int describe(int value)
    {
        return widget.value() + value;
    }

    // Declared after `widget`, so destroyed before it; constructed before the runtime's
    // objects, which its object file comes after.
    struct ExitTrace
    {
        ~ExitTrace()
        {
            __ct_vcall_trace(&widget, reinterpret_cast<void*>(&describe), "exit:1", "<unknown>");
        }
    };

    ExitTrace exit_trace;
} // namespace

int main()
{
    ct_init_env_once();
    ct_set_enabled(CT_FEATURE_VTABLE_DIAG, 1);
    return 0;
}
