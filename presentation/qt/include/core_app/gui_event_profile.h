#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <algorithm>

// GUI-thread-only, opt-in diagnostics. Nested notify calls contribute to the
// parent inclusive duration, but are subtracted from its exclusive duration.
namespace gui_event_profile {
struct Group {
    std::array<char, 64> receiver{};
    int type = 0;
    uint64_t count = 0, inclusive = 0, exclusive = 0, maximum = 0;
    uint64_t inclusive_cycles = 0, exclusive_cycles = 0;
};
inline bool enabled = false;
inline std::array<Group, 256> groups{};
inline size_t group_count = 0;
inline uint64_t overflow = 0;
struct Frame {
    Frame *parent = nullptr;
    uint64_t children = 0;
    uint64_t children_cycles = 0;
};
inline Frame *current = nullptr;

inline void record(const char *receiver, int type, uint64_t duration, uint64_t children,
                   uint64_t cycles = 0, uint64_t children_cycles = 0) noexcept {
    size_t index = 0;
    while (index < group_count &&
           (groups[index].type != type || std::strncmp(groups[index].receiver.data(), receiver, 63) != 0))
        ++index;
    if (index == group_count) {
        if (group_count == groups.size()) { ++overflow; return; }
        auto &group = groups[group_count++];
        std::strncpy(group.receiver.data(), receiver, group.receiver.size() - 1);
        group.type = type;
    }
    auto &group = groups[index];
    ++group.count;
    group.inclusive += duration;
    group.exclusive += duration - std::min(duration, children);
    group.maximum = std::max(group.maximum, duration);
    group.inclusive_cycles += cycles;
    group.exclusive_cycles += cycles - std::min(cycles, children_cycles);
}
}
