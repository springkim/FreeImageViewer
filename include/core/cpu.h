#pragma once
#ifndef CPU_INFO_HPP
#define CPU_INFO_HPP
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#if defined(_WIN32) || defined(_WIN64)
#define CPUINFO_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#define CPUINFO_MACOS
#include <sys/sysctl.h>
#include <sys/types.h>
#elif defined(__linux__)
#define CPUINFO_LINUX
#include <filesystem>
#endif
namespace core {
    struct CPUInfo {
        std::string model;
        std::optional<unsigned int> cores;
        std::optional<unsigned int> threads;
        std::optional<unsigned int> p_cores;
        std::optional<unsigned int> e_cores;
    };

    inline std::string trim(std::string str) {
        const auto first = str.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
            return {};
        const auto last = str.find_last_not_of(" \t\r\n");
        return str.substr(first, last - first + 1);
    }

    inline std::optional<std::string>
    read_text(const std::string &path) {
        std::ifstream file(path);
        if (!file)
            return std::nullopt;
        std::string text{
            std::istreambuf_iterator<char>(file),
            std::istreambuf_iterator<char>()
        };
        return trim(text);
    }

    inline std::optional<unsigned long long>
    read_uint64(const std::string &path) {
        const auto text = read_text(path);
        if (!text)
            return std::nullopt;
        try {
            return std::stoull(*text);
        } catch (...) {
            return std::nullopt;
        }
    }
#if defined(CPUINFO_WINDOWS)
    inline std::string get_windows_cpu_name() {
        HKEY key = nullptr;
        if (RegOpenKeyExA(
                HKEY_LOCAL_MACHINE,
                "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                0,
                KEY_READ,
                &key
            ) != ERROR_SUCCESS) {
            return {};
        }
        char buffer[512]{};
        DWORD size = sizeof(buffer);
        DWORD type = 0;
        const LONG result = RegQueryValueExA(
            key,
            "ProcessorNameString",
            nullptr,
            &type,
            (LPBYTE) buffer,
            &size
        );
        RegCloseKey(key);
        if (result != ERROR_SUCCESS)
            return {};
        return trim(buffer);
    }
    inline std::optional<unsigned int>
    get_windows_thread_count() {
        const DWORD count =
                GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
        if (count == 0)
            return std::nullopt;
        return (unsigned int) count;
    }
    struct WindowsCoreTopology {
        unsigned int cores = 0;
        std::vector<unsigned int> efficiency_classes;
    };
    inline WindowsCoreTopology
    get_windows_core_topology() {
        WindowsCoreTopology result;
        DWORD size = 0;
        GetLogicalProcessorInformationEx(
            RelationProcessorCore,
            nullptr,
            &size
        );
        if (!size)
            return result;
        std::vector<unsigned char> buffer(size);
        if (!GetLogicalProcessorInformationEx(
            RelationProcessorCore,
            (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)
            buffer.data(),
            &size
        )) {
            return result;
        }
        DWORD offset = 0;
        while (offset < size) {
            auto *info =
                    (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)
                    (buffer.data() + offset);
            if (!info->Size)
                break;
            if (info->Relationship == RelationProcessorCore) {
                ++result.cores;
                result.efficiency_classes.push_back(
                    (unsigned int)
                    info->Processor.EfficiencyClass
                );
            }
            offset += info->Size;
        }
        return result;
    }
    inline std::pair<
        std::optional<unsigned int>,
        std::optional<unsigned int>
    >
    get_windows_hybrid_counts(
        const std::vector<unsigned int> &classes
    ) {
        if (classes.empty()) {
            return {
                std::nullopt,
                std::nullopt
            };
        }
        const auto [min_it, max_it] =
                std::minmax_element(
                    classes.begin(),
                    classes.end()
                );
        if (*min_it == *max_it) {
            return {
                std::nullopt,
                std::nullopt
            };
        }
        const unsigned int max_class = *max_it;
        unsigned int p = 0;
        unsigned int e = 0;
        for (const auto value: classes) {
            if (value == max_class)
                ++p;
            else
                ++e;
        }
        return {p, e};
    }
    inline CPUInfo get_windows_cpu_info() {
        CPUInfo result;
        result.model =
                get_windows_cpu_name();
        result.threads =
                get_windows_thread_count();
        const auto topology =
                get_windows_core_topology();
        if (topology.cores)
            result.cores = topology.cores;
        const auto [p, e] =
                get_windows_hybrid_counts(
                    topology.efficiency_classes
                );
        result.p_cores = p;
        result.e_cores = e;
        return result;
    }
#endif
#if defined(CPUINFO_MACOS)
    inline std::optional<std::string>
    sysctl_string(const char *name) {
        size_t size = 0;
        if (sysctlbyname(
                name,
                nullptr,
                &size,
                nullptr,
                0
            ) != 0) {
            return std::nullopt;
        }
        if (!size)
            return std::nullopt;
        std::string value(size, '\0');
        if (sysctlbyname(
                name,
                value.data(),
                &size,
                nullptr,
                0
            ) != 0) {
            return std::nullopt;
        }
        while (!value.empty() &&
               value.back() == '\0') {
            value.pop_back();
        }
        return trim(value);
    }

    inline std::optional<unsigned int>
    sysctl_uint(const char *name) {
        std::uint32_t value = 0;
        size_t size = sizeof(value);
        if (sysctlbyname(
                name,
                &value,
                &size,
                nullptr,
                0
            ) != 0) {
            return std::nullopt;
        }
        return (unsigned int) value;
    }

    inline bool is_apple_silicon() {
#if defined(__aarch64__) || defined(__arm64__)
        return true;
#else
        int translated = 0;
        size_t size = sizeof(translated);
        if (sysctlbyname(
                "sysctl.proc_translated",
                &translated,
                &size,
                nullptr,
                0
            ) == 0) {
            return translated == 1;
        }
        return false;
#endif
    }

    inline std::string get_macos_cpu_name() {
        if (const auto value =
                sysctl_string("machdep.cpu.brand_string")) {
            if (!value->empty())
                return *value;
        }
        if (const auto value =
                sysctl_string("hw.model")) {
            if (is_apple_silicon())
                return "Apple Silicon (" + *value + ")";
            return *value;
        }
        return {};
    }

    inline std::pair<
        std::optional<unsigned int>,
        std::optional<unsigned int>
    >
    get_macos_hybrid_counts() {
        if (!is_apple_silicon()) {
            return {
                std::nullopt,
                std::nullopt
            };
        }
        const auto p =
                sysctl_uint(
                    "hw.perflevel0.physicalcpu"
                );
        const auto e =
                sysctl_uint(
                    "hw.perflevel1.physicalcpu"
                );
        if (!p || !e) {
            return {
                std::nullopt,
                std::nullopt
            };
        }
        return {p, e};
    }

    inline CPUInfo get_macos_cpu_info() {
        CPUInfo result;
        result.model =
                get_macos_cpu_name();
        result.cores =
                sysctl_uint("hw.physicalcpu");
        result.threads =
                sysctl_uint("hw.logicalcpu");
        const auto [p, e] =
                get_macos_hybrid_counts();
        result.p_cores = p;
        result.e_cores = e;
        return result;
    }
#endif
#if defined(CPUINFO_LINUX)
    namespace fs = std::filesystem;
    inline std::string get_linux_cpu_name() {
        std::ifstream file("/proc/cpuinfo");
        if (!file)
            return {};
        std::string line;
        static constexpr const char *keys[] = {
            "model name",
            "Hardware",
            "Processor",
            "Model"
        };
        while (std::getline(file, line)) {
            const auto colon =
                    line.find(':');
            if (colon == std::string::npos)
                continue;
            const auto key =
                    trim(
                        line.substr(0, colon)
                    );
            for (const char *expected: keys) {
                if (key == expected) {
                    return trim(
                        line.substr(
                            colon + 1
                        )
                    );
                }
            }
        }
        return {};
    }
    inline unsigned int get_linux_thread_count() {
        unsigned int count = 0;
        std::error_code ec;
        for (const auto &entry:
             fs::directory_iterator(
                 "/sys/devices/system/cpu",
                 ec
             )) {
            if (ec)
                break;
            const auto name =
                    entry.path()
                    .filename()
                    .string();
            if (name.size() <= 3)
                continue;
            if (name.compare(0, 3, "cpu") != 0)
                continue;
            bool numeric = true;
            for (std::size_t i = 3;
                 i < name.size();
                 ++i) {
                if (!std::isdigit(
                    (unsigned char) name[i]
                )) {
                    numeric = false;
                    break;
                }
            }
            if (numeric)
                ++count;
        }
        if (!count)
            count =
                    std::thread::hardware_concurrency();
        return count;
    }
    struct LinuxCore {
        std::string key;
        std::optional<unsigned int> core_type;
        std::optional<unsigned long long> capacity;
        std::optional<unsigned long long> max_frequency;
    };
    inline std::vector<LinuxCore>
    get_linux_cores() {
        std::map<std::string, LinuxCore> cores;
        const fs::path root =
                "/sys/devices/system/cpu";
        std::error_code ec;
        fs::directory_iterator iterator(
            root,
            ec
        );
        if (ec)
            return {};
        for (const auto &entry: iterator) {
            const std::string name =
                    entry.path()
                    .filename()
                    .string();
            if (name.size() <= 3)
                continue;
            if (name.compare(0, 3, "cpu") != 0)
                continue;
            bool numeric = true;
            for (std::size_t i = 3;
                 i < name.size();
                 ++i) {
                if (!std::isdigit(
                    (unsigned char) name[i]
                )) {
                    numeric = false;
                    break;
                }
            }
            if (!numeric)
                continue;
            const fs::path cpu_path =
                    entry.path();
            const fs::path topology =
                    cpu_path / "topology";
            const auto core_id =
                    read_text(
                        (topology /
                         "core_id").string()
                    );
            if (!core_id)
                continue;
            const auto package_id =
                    read_text(
                        (topology /
                         "physical_package_id").string()
                    );
            const std::string key =
                    (package_id
                         ? *package_id
                         : "0")
                    + ":" +
                    *core_id;
            LinuxCore core{};
            core.key = key;
            if (const auto value =
                    read_uint64(
                        (topology /
                         "core_type").string()
                    )) {
                core.core_type =
                        (unsigned int) *value;
            }
            core.capacity =
                    read_uint64(
                        (cpu_path /
                         "cpu_capacity").string()
                    );
            core.max_frequency =
                    read_uint64(
                        (cpu_path /
                         "cpufreq" /
                         "cpuinfo_max_freq").string()
                    );
            auto it = cores.find(key);
            if (it == cores.end()) {
                cores.emplace(
                    key,
                    std::move(core)
                );
            } else {
                if (!it->second.core_type &&
                    core.core_type) {
                    it->second.core_type =
                            core.core_type;
                }
                if (!it->second.capacity &&
                    core.capacity) {
                    it->second.capacity =
                            core.capacity;
                }
                if (!it->second.max_frequency &&
                    core.max_frequency) {
                    it->second.max_frequency =
                            core.max_frequency;
                }
            }
        }
        std::vector<LinuxCore> result;
        result.reserve(cores.size());
        for (auto &[key, core]: cores) {
            result.emplace_back(
                std::move(core)
            );
        }
        return result;
    }
    inline std::pair<
        std::optional<unsigned int>,
        std::optional<unsigned int>
    >
    hybrid_from_core_type(
        const std::vector<LinuxCore> &cores
    ) {
        unsigned int p = 0;
        unsigned int e = 0;
        for (const auto &core: cores) {
            if (!core.core_type)
                continue;
            if (*core.core_type == 2)
                ++p;
            else if (*core.core_type == 1)
                ++e;
        }
        if (!p || !e) {
            return {
                std::nullopt,
                std::nullopt
            };
        }
        return {p, e};
    }
    inline std::pair<
        std::optional<unsigned int>,
        std::optional<unsigned int>
    >
    hybrid_from_capacity(
        const std::vector<LinuxCore> &cores
    ) {
        std::vector<unsigned long long> values;
        for (const auto &core: cores) {
            if (core.capacity)
                values.push_back(*core.capacity);
        }
        if (values.size() != cores.size())
            return {
                std::nullopt,
                std::nullopt
            };
        const auto [min_it, max_it] =
                std::minmax_element(
                    values.begin(),
                    values.end()
                );
        if (*min_it == *max_it)
            return {
                std::nullopt,
                std::nullopt
            };
        unsigned int p = 0;
        unsigned int e = 0;
        const auto max_capacity =
                *max_it;
        for (const auto value: values) {
            if (value == max_capacity)
                ++p;
            else
                ++e;
        }
        return {p, e};
    }
    inline std::pair<
        std::optional<unsigned int>,
        std::optional<unsigned int>
    >
    hybrid_from_frequency(
        const std::vector<LinuxCore> &cores
    ) {
        std::vector<unsigned long long> values;
        for (const auto &core: cores) {
            if (core.max_frequency)
                values.push_back(
                    *core.max_frequency
                );
        }
        if (values.size() != cores.size())
            return {
                std::nullopt,
                std::nullopt
            };
        const auto [min_it, max_it] =
                std::minmax_element(
                    values.begin(),
                    values.end()
                );
        if (*min_it == *max_it)
            return {
                std::nullopt,
                std::nullopt
            };
        const double ratio =
                (double) *max_it /
                (double) *min_it;
        if (ratio < 1.10)
            return {
                std::nullopt,
                std::nullopt
            };
        unsigned int p = 0;
        unsigned int e = 0;
        const auto max_freq =
                *max_it;
        for (const auto value: values) {
            if (value == max_freq)
                ++p;
            else
                ++e;
        }
        return {p, e};
    }
    inline std::pair<
        std::optional<unsigned int>,
        std::optional<unsigned int>
    >
    get_linux_hybrid_counts(
        const std::vector<LinuxCore> &cores
    ) {
        {
            const auto result =
                    hybrid_from_core_type(cores);
            if (result.first &&
                result.second) {
                return result;
            }
        }
        {
            const auto result =
                    hybrid_from_capacity(cores);
            if (result.first &&
                result.second) {
                return result;
            }
        }
        return hybrid_from_frequency(cores);
    }
    inline CPUInfo get_linux_cpu_info() {
        CPUInfo result;
        result.model =
                get_linux_cpu_name();
        const auto cores =
                get_linux_cores();
        if (!cores.empty()) {
            result.cores =
                    (unsigned int) cores.size();
        }
        const unsigned int threads =
                get_linux_thread_count();
        if (threads)
            result.threads = threads;
        const auto [p, e] =
                get_linux_hybrid_counts(cores);
        result.p_cores = p;
        result.e_cores = e;
        return result;
    }
#endif
    inline CPUInfo get_cpu_info() {
#if defined(CPUINFO_WINDOWS)
        return get_windows_cpu_info();
#elif defined(CPUINFO_MACOS)
        return get_macos_cpu_info();
#elif defined(CPUINFO_LINUX)
        return get_linux_cpu_info();
#else
        CPUInfo result;
        const unsigned int threads =
                std::thread::hardware_concurrency();
        if (threads)
            result.threads = threads;
        return result;
#endif
    }
}
#endif
