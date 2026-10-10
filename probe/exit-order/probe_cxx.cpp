// SPDX-License-Identifier: Apache-2.0
// A C++ DLL as ct_leak_exit_shared_library_lib.cpp: a global object whose destructor runs
// at exit, and a destructor function. It depends on probe_dep.dll.
#include "probe_report.h"

extern "C" __declspec(dllimport) int probe_dep_touch(void);

namespace
{
    struct Global
    {
        ~Global()
        {
            probe_report("cxx-dll-global-object-destructor");
        }
    };

    Global global;

    __attribute__((destructor)) void cxx_destructor()
    {
        probe_report("cxx-dll-destructor");
    }
} // namespace

extern "C" __declspec(dllexport) int probe_cxx_touch(void)
{
    return probe_dep_touch();
}
