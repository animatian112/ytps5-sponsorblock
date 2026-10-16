// ps5-homebrew-ui - PS5 system services: logging, clock, splash screen.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "platform/ps5/system.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>

extern "C"
{
    int sceKernelDebugOutText(int channel, const char *text);
    int sceKernelUsleep(unsigned int microseconds);
    int sceSystemServiceHideSplashScreen(void);
    int sceSystemServiceLoadExec(const char *path, const char **arguments);
    int sceSystemServiceParamGetInt(int param_id, int *value);
}

extern "C" void hui_release_splash(void) __attribute__((weak));

namespace hui::sys
{

std::int64_t monotonic_us()
{
    timespec now{};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    return static_cast<std::int64_t>(now.tv_sec) * 1000000 + now.tv_nsec / 1000;
}

bool local_clock(int *hour, int *minute, bool *twelve_hour)
{
    // System parameters: 3 time format (0 = 12-hour), 4 time zone (minutes
    // east of UTC), 5 summer time (1 = on).
    constexpr int kTimeFormat = 3;
    constexpr int kTimeZone = 4;
    constexpr int kSummerTime = 5;
    const long long now = static_cast<long long>(std::time(nullptr));
    if (now < 1600000000LL)
        return false;
    int zone = 0;
    int summer = 0;
    int format = 1;
    if (sceSystemServiceParamGetInt(kTimeZone, &zone) != 0)
        zone = 0;
    if (sceSystemServiceParamGetInt(kSummerTime, &summer) != 0)
        summer = 0;
    if (sceSystemServiceParamGetInt(kTimeFormat, &format) != 0)
        format = 1;
    long long local = now + static_cast<long long>(zone) * 60 + (summer != 0 ? 3600 : 0);
    local %= 86400;
    if (local < 0)
        local += 86400;
    *hour = static_cast<int>(local / 3600);
    *minute = static_cast<int>((local % 3600) / 60);
    *twelve_hour = format == 0;
    return true;
}

bool local_date(int *weekday, int *month, int *day)
{
    constexpr int kTimeZone = 4;
    constexpr int kSummerTime = 5;
    const long long now = static_cast<long long>(std::time(nullptr));
    if (now < 1600000000LL)
        return false;
    int zone = 0;
    int summer = 0;
    if (sceSystemServiceParamGetInt(kTimeZone, &zone) != 0)
        zone = 0;
    if (sceSystemServiceParamGetInt(kSummerTime, &summer) != 0)
        summer = 0;
    const long long local = now + static_cast<long long>(zone) * 60 + (summer != 0 ? 3600 : 0);
    const long long days = local / 86400;        // after 2020: never negative
    *weekday = static_cast<int>((days + 4) % 7); // 1970-01-01 was a Thursday
    // Howard Hinnant's civil_from_days.
    const long long z = days + 719468;
    const long long era = z / 146097;
    const long long doe = z - era * 146097;
    const long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const long long mp = (5 * doy + 2) / 153;
    *day = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    *month = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    return true;
}

void log(const char *format, ...)
{
    // klog lines are short; longer messages are truncated rather than split.
    char line[384];
    va_list arguments;
    va_start(arguments, format);
    int length = std::vsnprintf(line, sizeof(line) - 1, format, arguments);
    va_end(arguments);
    if (length < 0)
        return;
    std::size_t used = std::strlen(line);
    line[used] = '\n';
    line[used + 1] = '\0';
    std::fputs(line, stdout);
    sceKernelDebugOutText(0, line);
}

bool hide_splash_screen()
{
    // The runtime holds earlier requests back (runtime_shims.c): this is the
    // one that counts. An app with a runtime of its own has no such hold.
    if (hui_release_splash)
        hui_release_splash();
    return sceSystemServiceHideSplashScreen() == 0;
}

void sleep_us(std::uint32_t microseconds)
{
    sceKernelUsleep(microseconds);
}

void park()
{
    std::fflush(nullptr);
    for (;;)
        sceKernelUsleep(100000);
}

void quit()
{
    log("[HUI] quit requested");
    std::fflush(nullptr);
    const int refused = sceSystemServiceLoadExec("exit", nullptr);
    log("[HUI] quit refused rc=0x%x", static_cast<unsigned>(refused));
    park();
}

} // namespace hui::sys
