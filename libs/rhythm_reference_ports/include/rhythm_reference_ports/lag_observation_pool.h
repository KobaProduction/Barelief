// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <dsp_kernels/numeric_rows.h>
#include <span>
#include <vector>

namespace rhythm_reference_ports {
// Source Pool flattens every matchingPeriods output row, including repeated
// cumulative prefixes when TempoTap evaluates every frame. Store only appended
// suffixes plus bounded row metadata; never clone cumulative history per push.
// An empty row starts a new prefix run. A nonempty row must extend the preceding
// row in the run. Flattening is a borrowed scalar reader, preserving row order
// and repetitions for the existing BPM vote. All bounds are prepared explicitly.
class PreparedLagObservationPool {
public:
    // Dynamic owned/cold-preparation upper bound; excludes this object.
    static bool storage_bytes(std::size_t maximum_rows,std::size_t maximum_unique_values,std::size_t maximum_flat_values,std::size_t& bytes) noexcept;
    std::size_t workspace_bytes() const noexcept;
    bool prepare(std::size_t maximum_rows,std::size_t maximum_unique_values,std::size_t maximum_flat_values);
    void reset() noexcept;
    bool push(std::span<const float> cumulative_row) noexcept;
    std::size_t flattened_size() const noexcept {return flat_count_;}
    std::size_t rows() const noexcept {return row_count_;}
    std::span<const float> unique_values() const noexcept {return std::span(values_).first(value_count_);}
    dsp_kernels::FloatSequenceSource source() const noexcept {return {this,read};}
private:
    struct Row {std::size_t start=0,end=0;};
    static bool read(const void*,std::size_t,float&) noexcept;
    std::size_t maximum_flat_=0,value_count_=0,row_count_=0,flat_count_=0,active_start_=0,active_count_=0;
    std::vector<float> values_;
    std::vector<Row> rows_;
};
}
