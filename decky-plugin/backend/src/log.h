#pragma once

#include <cstdarg>
#include <cstdio>
#include <ctime>

namespace logging {

enum Level { kDebug = 0, kInfo = 1, kWarn = 2, kError = 3 };

inline int &threshold() {
    static int level = kInfo;
    return level;
}

__attribute__((format(printf, 2, 3))) inline void log(int level, const char *fmt, ...) {
    if (level < threshold()) return;
    static const char *names[] = {"DEBUG", "INFO", "WARN", "ERROR"};
    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    tm tm_local{};
    localtime_r(&ts.tv_sec, &tm_local);
    char stamp[32];
    strftime(stamp, sizeof(stamp), "%H:%M:%S", &tm_local);
    std::fprintf(stderr, "%s.%03ld [%s] ", stamp, ts.tv_nsec / 1000000, names[level]);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

} // namespace logging

#define LOGD(...) ::logging::log(::logging::kDebug, __VA_ARGS__)
#define LOGI(...) ::logging::log(::logging::kInfo, __VA_ARGS__)
#define LOGW(...) ::logging::log(::logging::kWarn, __VA_ARGS__)
#define LOGE(...) ::logging::log(::logging::kError, __VA_ARGS__)
