#include "ct_runtime_internal.h"

#include <cstdlib>
#include <execinfo.h>
#include <signal.h>

namespace {
int ct_backtrace_installed = 0;

CT_NOINSTR void ct_signal_handler(int signo)
{
    ct_disable_logging();
    ct_write_prefix(CTLevel::Error);
    ct_write_str(ct_color(CTColor::Red));
    ct_write_cstr("ct: fatal signal ");
    ct_write_dec(static_cast<size_t>(signo));
    ct_write_str(ct_color(CTColor::Reset));
    ct_write_cstr("\n");

    void *frames[64];
    int count = backtrace(frames, static_cast<int>(sizeof(frames) / sizeof(frames[0])));
    if (count > 0) {
        backtrace_symbols_fd(frames, count, 2);
    }

    _exit(128 + signo);
}
} // namespace

CT_NOINSTR void ct_maybe_install_backtrace(void)
{
    if (getenv("CT_BACKTRACE") == nullptr) {
        return;
    }

    int expected = 0;
    if (!__atomic_compare_exchange_n(&ct_backtrace_installed,
                                     &expected,
                                     1,
                                     false,
                                     __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE)) {
        return;
    }

    struct sigaction sa {};
    sa.sa_handler = ct_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESETHAND;

    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    sigaction(SIGILL, &sa, nullptr);

    ct_write_prefix(CTLevel::Info);
    ct_write_str(ct_color(CTColor::Green));
    ct_write_cstr("ct: backtrace handler installed");
    ct_write_str(ct_color(CTColor::Reset));
    ct_write_cstr("\n");
}
