#pragma once
#include <cstdint>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <fstream>
#include <unistd.h>
#endif

inline int64_t resident_memory() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS memory{};
    memory.cb = sizeof(memory);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory)))
        throw std::runtime_error("Cannot measure memory");
    return memory.WorkingSetSize;
#else
    std::ifstream statm("/proc/self/statm");
    int64_t pages = 0, resident = 0;
    if (!(statm >> pages >> resident)) throw std::runtime_error("Cannot measure memory");
    return resident * sysconf(_SC_PAGESIZE);
#endif
}
