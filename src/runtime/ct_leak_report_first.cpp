// SPDX-License-Identifier: Apache-2.0
// Linked first into instrumented executables on Apple targets, by cc with -force_load (#161).
// ld64 runs an executable's initializers in link order, not by priority across objects: this
// one runs before those of every object that follows, compiled with --instrument or not,
// which may register exit-time code that releases tracked blocks. The leak report it
// schedules is then the first exit handler registered, so it runs last.
extern "C" void __ct_schedule_leak_report(void);

// Global, so that the archive has a symbol table.
extern "C" __attribute__((constructor)) void __ct_schedule_leak_report_first(void)
{
    __ct_schedule_leak_report();
}
