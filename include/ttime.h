#pragma once
#ifndef HG_TTIME_H
#define HG_TTIME_H
#if defined(_WIN32) || defined(_WIN64)
#define TTIME_OS_WINDOWS
#elif defined(__linux__)
#define TTIME_OS_LINUX
#elif defined(__APPLE__)
#define TTIME_OS_MACOS
#else
#error "ttime.h: Unsupported operating system"
#endif
#if defined(TTIME_OS_WINDOWS)
#include <windows.h>
static double ttime(void) {
    static double frequency = 0.0;
    LARGE_INTEGER freq;
    LARGE_INTEGER counter;
    if (frequency == 0.0) {
        QueryPerformanceFrequency(&freq);
        frequency = (double) freq.QuadPart;
    }
    QueryPerformanceCounter(&counter);
    return (double) counter.QuadPart * 1000.0 / frequency;
}
static void tsleep(int ms) {
    if (ms <= 0)
        return;
    Sleep((DWORD) ms);
}
#else
#include <time.h>
#include <errno.h>

static double ttime(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec * 1000.0 +
           (double) ts.tv_nsec / 1000000.0;
}

static void tsleep(int ms) {
    struct timespec req;
    struct timespec rem;

    if (ms <= 0)
        return;

    req.tv_sec = (time_t) (ms / 1000);
    req.tv_nsec = (long) (ms % 1000) * 1000000L;

    while (nanosleep(&req, &rem) == -1 && errno == EINTR) {
        req = rem;
    }
}
#endif
#endif
