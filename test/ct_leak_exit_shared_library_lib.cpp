// SPDX-License-Identifier: Apache-2.0
// Built into a shared library without --instrument for ct_leak_exit_shared_library.cpp.
// At exit, it calls back into the program, which releases its tracked blocks.
namespace
{
    void (*object_callback)();
    void (*function_callback)();

    struct Runner
    {
        ~Runner()
        {
            if (object_callback)
            {
                object_callback();
            }
        }
    };

    Runner runner;

    __attribute__((destructor)) void run_function_callback()
    {
        if (function_callback)
        {
            function_callback();
        }
    }
} // namespace

extern "C" void ct_exit_library_register(void (*release_object)(), void (*release_function)())
{
    object_callback = release_object;
    function_callback = release_function;
}
