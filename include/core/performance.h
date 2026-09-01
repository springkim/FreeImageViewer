#ifndef FIV_PERFORMANCE_H
#define FIV_PERFORMANCE_H

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#include <intrin.h>
#elif defined(__linux__)
#include <cpuid.h>
#include <sched.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <pthread.h>
#include <sys/qos.h>
#else
#error "ttime.h: Unsupported operating system"
#endif


namespace core {
#if defined(_WIN32) || defined(_WIN64)
    bool use_pcore() {
        int regs[4];
        __cpuid(regs, 0);
        if (regs[0] < 0x1A) return false;
        DWORD ncpu = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
        if (ncpu > 64) ncpu = 64;
        HANDLE self = GetCurrentThread();
        DWORD_PTR original = SetThreadAffinityMask(self, 1);
        DWORD_PTR pcores = 0;
        for (DWORD cpu = 0; cpu < ncpu; ++cpu) {
            if (SetThreadAffinityMask(self, (DWORD_PTR) 1 << cpu) == 0) continue;
            SwitchToThread();
            __cpuidex(regs, 0x1A, 0);
            if (((unsigned) regs[0] >> 24) == 0x20) pcores |= (DWORD_PTR) 1 << cpu;
        }
        if (pcores == 0) {
            SetThreadAffinityMask(self, original);
            return false;
        }
        return SetThreadAffinityMask(self, pcores) != 0;
    }
#elif defined(__linux__)

    bool use_pcore() {
        if (__get_cpuid_max(0, nullptr) < 0x1A) return false;
        cpu_set_t original, pcores;
        CPU_ZERO(&pcores);
        sched_getaffinity(0, sizeof(original), &original);
        int ncpu = (int) sysconf(_SC_NPROCESSORS_ONLN);
        for (int cpu = 0; cpu < ncpu; ++cpu) {
            cpu_set_t one;
            CPU_ZERO(&one);
            CPU_SET(cpu, &one);
            if (sched_setaffinity(0, sizeof(one), &one) != 0) continue;
            sched_yield();
            unsigned eax, ebx, ecx, edx;
            __cpuid_count(0x1A, 0, eax, ebx, ecx, edx);
            if ((eax >> 24) == 0x20) CPU_SET(cpu, &pcores);
        }
        if (CPU_COUNT(&pcores) == 0) {
            sched_setaffinity(0, sizeof(original), &original);
            return false;
        }
        return sched_setaffinity(0, sizeof(pcores), &pcores) == 0;
    }
#elif defined(__APPLE__)

    bool use_pcore() {
        return pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0) == 0;
    }
#else
#error "ttime.h: Unsupported operating system"
#endif
}
#endif
