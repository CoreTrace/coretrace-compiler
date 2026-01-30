#include "ct_runtime_internal.h"

#include <cstdlib>
#include <pthread.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif

namespace
{
    int ct_log_enabled = 0;
    int ct_log_atexit_registered = 0;

    CT_NODISCARD CT_NOINSTR int ct_use_color(void)
    {
        static int cached = -1;

        if (cached != -1)
            return cached;

        if (getenv("NO_COLOR") != nullptr)
        {
            cached = 0;
            return cached;
        }

        cached = isatty(2) ? 1 : 0;
        return cached;
    }

    CT_NOINSTR void ct_register_atexit(void)
    {
        int expected = 0;
        if (__atomic_compare_exchange_n(&ct_log_atexit_registered, &expected, 1, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        {
            std::atexit(ct_disable_logging);
        }
    }
} // namespace

CT_NODISCARD CT_NOINSTR size_t ct_strlen(const char* str)
{
    size_t len = 0;

    if (!str)
        return 0;

    while (str[len] != '\0')
        ++len;

    return len;
}

CT_NODISCARD CT_NOINSTR int ct_streq(const char* lhs, const char* rhs)
{
    if (!lhs || !rhs)
        return 0;

    while (*lhs != '\0' && *rhs != '\0')
    {
        if (*lhs != *rhs)
            return 0;
        ++lhs;
        ++rhs;
    }
    return *lhs == *rhs;
}

CT_NODISCARD CT_NOINSTR std::string_view ct_color(CTColor color)
{
    if (!ct_use_color())
        return {};

    switch (color)
    {
    case CTColor::Reset:
        return "\x1b[0m";

    case CTColor::Dim:
        return "\x1b[2m";
    case CTColor::Bold:
        return "\x1b[1m";
    case CTColor::Underline:
        return "\x1b[4m";
    case CTColor::Italic:
        return "\x1b[3m";
    case CTColor::Blink:
        return "\x1b[5m";
    case CTColor::Reverse:
        return "\x1b[7m";
    case CTColor::Hidden:
        return "\x1b[8m";
    case CTColor::Strike:
        return "\x1b[9m";

    case CTColor::Black:
        return "\x1b[30m";
    case CTColor::Red:
        return "\x1b[31m";
    case CTColor::Green:
        return "\x1b[32m";
    case CTColor::Yellow:
        return "\x1b[33m";
    case CTColor::Blue:
        return "\x1b[34m";
    case CTColor::Magenta:
        return "\x1b[35m";
    case CTColor::Cyan:
        return "\x1b[36m";
    case CTColor::White:
        return "\x1b[37m";

    case CTColor::Gray:
        return "\x1b[90m";
    case CTColor::BrightRed:
        return "\x1b[91m";
    case CTColor::BrightGreen:
        return "\x1b[92m";
    case CTColor::BrightYellow:
        return "\x1b[93m";
    case CTColor::BrightBlue:
        return "\x1b[94m";
    case CTColor::BrightMagenta:
        return "\x1b[95m";
    case CTColor::BrightCyan:
        return "\x1b[96m";
    case CTColor::BrightWhite:
        return "\x1b[97m";

    case CTColor::BgBlack:
        return "\x1b[40m";
    case CTColor::BgRed:
        return "\x1b[41m";
    case CTColor::BgGreen:
        return "\x1b[42m";
    case CTColor::BgYellow:
        return "\x1b[43m";
    case CTColor::BgBlue:
        return "\x1b[44m";
    case CTColor::BgMagenta:
        return "\x1b[45m";
    case CTColor::BgCyan:
        return "\x1b[46m";
    case CTColor::BgWhite:
        return "\x1b[47m";

    case CTColor::BgGray:
        return "\x1b[100m";
    case CTColor::BgBrightRed:
        return "\x1b[101m";
    case CTColor::BgBrightGreen:
        return "\x1b[102m";
    case CTColor::BgBrightYellow:
        return "\x1b[103m";
    case CTColor::BgBrightBlue:
        return "\x1b[104m";
    case CTColor::BgBrightMagenta:
        return "\x1b[105m";
    case CTColor::BgBrightCyan:
        return "\x1b[106m";
    case CTColor::BgBrightWhite:
        return "\x1b[107m";
    }

    return {};
}

CT_NODISCARD CT_NOINSTR std::string_view ct_level_label(CTLevel level)
{
    switch (level)
    {
    case CTLevel::Info:
        return "INFO";
    case CTLevel::Warn:
        return "WARN";
    case CTLevel::Error:
        return "ERROR";
    }

    return "INFO";
}

CT_NODISCARD CT_NOINSTR std::string_view ct_level_color(CTLevel level)
{
    switch (level)
    {
    case CTLevel::Info:
        return ct_color(CTColor::Green);
    case CTLevel::Warn:
        return ct_color(CTColor::Yellow);
    case CTLevel::Error:
        return ct_color(CTColor::Red);
    }

    return ct_color(CTColor::Cyan);
}

CT_NODISCARD CT_NOINSTR int ct_pid(void)
{
    static int cached = 0;

    if (cached == 0)
        cached = static_cast<int>(getpid());

    return cached;
}

CT_NODISCARD CT_NOINSTR unsigned long long ct_thread_id(void)
{
#if defined(__APPLE__)
    uint64_t tid = 0;
    (void)pthread_threadid_np(nullptr, &tid);
    return static_cast<unsigned long long>(tid);
#elif defined(__linux__)
    return static_cast<unsigned long long>(syscall(SYS_gettid));
#else
    return reinterpret_cast<unsigned long long>(pthread_self());
#endif
}

CT_NODISCARD CT_NOINSTR const char* ct_site_name(const char* site)
{
    if (site && site[0] != '\0')
        return site;

    if (ct_current_site && ct_current_site[0] != '\0')
    {
        return ct_current_site;
    }
    return "<unknown>";
}

CT_NODISCARD CT_NOINSTR int ct_log_is_enabled(void)
{
    return __atomic_load_n(&ct_log_enabled, __ATOMIC_ACQUIRE) != 0;
}

CT_NOINSTR void ct_disable_logging(void)
{
    __atomic_store_n(&ct_log_enabled, 0, __ATOMIC_RELEASE);
}

CT_NOINSTR void ct_enable_logging(void)
{
    __atomic_store_n(&ct_log_enabled, 1, __ATOMIC_RELEASE);
    ct_register_atexit();
}

CT_NOINSTR void ct_write_raw(const char* data, size_t size)
{
    if (!data || size == 0)
        return;

    while (size > 0)
    {
        ssize_t written = write(2, data, size);
        if (written > 0)
        {
            data += static_cast<size_t>(written);
            size -= static_cast<size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR)
            continue;
        break;
    }
}

CT_NOINSTR void ct_write_str(std::string_view value)
{
    if (value.empty())
        return;
    ct_write_raw(value.data(), value.size());
}

CT_NOINSTR void ct_write_cstr(const char* value)
{
    if (!value)
        return;
    ct_write_raw(value, ct_strlen(value));
}

CT_NOINSTR void ct_write_dec(size_t value)
{
    char buf[32];
    size_t idx = 0;

    if (value == 0)
    {
        buf[idx++] = '0';
    }
    else
    {
        while (value != 0 && idx < sizeof(buf))
        {
            buf[idx++] = static_cast<char>('0' + (value % 10));
            value /= 10;
        }
    }

    for (size_t i = 0; i < idx / 2; ++i)
    {
        char tmp = buf[i];
        buf[i] = buf[idx - 1 - i];
        buf[idx - 1 - i] = tmp;
    }

    ct_write_raw(buf, idx);
}

CT_NOINSTR void ct_write_hex(uintptr_t value)
{
    char buf[2 + sizeof(uintptr_t) * 2];
    size_t idx = 0;

    buf[idx++] = '0';
    buf[idx++] = 'x';

    bool started = false;
    for (int shift = static_cast<int>(sizeof(uintptr_t) * 8 - 4); shift >= 0; shift -= 4)
    {
        unsigned int nibble = static_cast<unsigned int>((value >> shift) & 0xF);
        if (!started && nibble == 0 && shift != 0)
            continue;

        started = true;
        if (nibble < 10)
        {
            buf[idx++] = static_cast<char>('0' + nibble);
        }
        else
        {
            buf[idx++] = static_cast<char>('a' + (nibble - 10));
        }
    }

    if (!started)
        buf[idx++] = '0';

    ct_write_raw(buf, idx);
}

CT_NOINSTR void ct_write_prefix(CTLevel level)
{
    ct_write_str(ct_color(CTColor::Dim));
    ct_write_cstr("|");
    ct_write_dec(static_cast<size_t>(ct_pid()));
    ct_write_cstr("|");
    ct_write_str(ct_color(CTColor::Reset));
    ct_write_cstr(" ");

    ct_write_str(ct_color(CTColor::Gray));
    ct_write_str(ct_color(CTColor::Italic));
    ct_write_cstr("==ct== ");
    ct_write_str(ct_color(CTColor::Reset));

    ct_write_str(ct_level_color(level));
    ct_write_cstr("[");
    ct_write_str(ct_level_label(level));
    ct_write_cstr("]");
    ct_write_str(ct_color(CTColor::Reset));
    ct_write_cstr(" ");
}
