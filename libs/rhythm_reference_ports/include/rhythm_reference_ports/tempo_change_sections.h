// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace rhythm_reference_ports {
struct TempoChangeSectionConfig {
    float tolerance=.08f,long_region_seconds=20,short_region_seconds=4;
};
// Source BpmRubato's experimental detector, including its five-period delay
// and alternating section open/close rules. Input is a complete seconds-valued
// beat sequence, not onsets. The original short-sequence rule (<=6 -> empty)
// is retained. No accuracy/confidence claim is attached to these sections.
class PreparedTempoChangeSections {
public:
    bool prepare(std::size_t maximum_beats,TempoChangeSectionConfig={});
    static bool required_workspace_bytes(std::size_t maximum,TempoChangeSectionConfig,std::size_t& bytes) noexcept;
    bool process(std::span<const float> beat_seconds) noexcept;
    std::span<const float> starts() const noexcept {return std::span(starts_).first(start_count_);}
    std::span<const float> stops() const noexcept {return std::span(stops_).first(stop_count_);}
    std::size_t section_count() const noexcept {return stop_count_;}
    std::size_t workspace_bytes() const noexcept {return (starts_.capacity()+stops_.capacity())*sizeof(float);}
private:
    void update_section(const std::array<float,5>&,float) noexcept;
    bool validate(std::span<const float>) const noexcept;
    bool step(const std::array<float,5>&) const noexcept;
    void open(float time) noexcept;
    void close(float time) noexcept;
    void finish(float time) noexcept;
    void advance(std::span<const float>) noexcept;
    TempoChangeSectionConfig config_;
    bool open_=false;
    std::size_t start_count_=0,stop_count_=0;
    std::vector<float> starts_,stops_;
};
}
