#pragma once
#include <signal_contracts/data_frame.h>

namespace signal_contracts {
struct NumericGatherLimits {
    FrameLimits frame;
    std::size_t max_output_payload_bytes, max_output_metadata_bytes, max_scratch_bytes;
};
struct NumericGatherRequest {
    NumericContract input;
    std::span<const std::uint32_t> selected_channels; // empty = all, otherwise exact declared order
    NumericGatherLimits limits;
    bool matrix_row=false; // Explicit operation: select one Matrix row; no semantic tag dispatch.
};
struct NumericGatherRequirements {
    std::size_t plan_metadata_bytes=0, plan_metadata_alignment=1;
    std::size_t payload_bytes=0, payload_alignment=1, output_metadata_bytes=0;
    std::size_t scratch_bytes=0, scratch_alignment=1;
};
struct NumericGatherRow { bool latest=true; std::size_t logical=0; };
struct NumericGatherDestination { MutableNumericView value; std::span<std::byte> metadata; };
struct NumericGatherRowInfo {
    bool history=false, silenced=false, mask_present=false;
    std::size_t logical_row=0, physical_row=0, filled_rows=0, capacity=0;
    // History coordinates/mask are in logical oldest-to-newest order, not physical ring order.
    AxisCoordinates time_axis;
    std::span<const std::byte> time_coordinate, selected_mask;
};
struct NumericGatherOutput {
    DataFrameView frame;
    NumericGatherRowInfo row;
    std::size_t payload_bytes=0, metadata_bytes=0;
};
class PreparedNumericGather {
public:
    PreparedNumericGather()=default;
    PreparedNumericGather(const PreparedNumericGather&)=default;
    PreparedNumericGather& operator=(const PreparedNumericGather&)=default;
    PreparedNumericGather(PreparedNumericGather&&) noexcept=default;
    PreparedNumericGather& operator=(PreparedNumericGather&&) noexcept=default;
    explicit operator bool() const noexcept {return bool(metadata_);}
    const NumericContract& input_contract() const noexcept {return input_;}
    const NumericContract& output_contract() const noexcept {return output_;}
    // For subset row events/reduction only: source shape/History metadata with projected channel IDs.
    // This is distinct from the original full-stream input contract.
    NumericContract projected_input_contract() const noexcept {auto c=input_;c.channel_order=output_.channel_order;return c;}
    std::span<const PlaneLayout> output_planes() const noexcept {return planes_;}
    const NumericGatherRequirements& requirements() const noexcept {return requirements_;}
private:
    BufferHandle metadata_;
    NumericContract input_,output_;
    std::span<const PlaneLayout> planes_;
    std::span<const std::size_t> selection_;
    NumericGatherLimits limits_{};
    NumericGatherRequirements requirements_;
    std::array<std::size_t,2> coordinate_offsets_{};
    std::size_t time_offset_=0, mask_offset_=0, stream_offset_=0;
    friend Status prepare_numeric_gather(const NumericGatherRequest&,BufferPool&,PreparedNumericGather&) noexcept;
    friend Status gather_numeric_frame(const PreparedNumericGather&,const DataFrameView&,NumericGatherRow,
        const NumericGatherDestination&,std::span<std::byte>,NumericGatherOutput&) noexcept;
};
Status numeric_gather_requirements(const NumericGatherRequest&,NumericGatherRequirements&) noexcept;
// Pool/slots outlive all plan copies. Plans immutable after publication; failure leaves out intact.
// Admission accounts physical pool blocks and retained old plans in addition to this logical query.
Status prepare_numeric_gather(const NumericGatherRequest&,BufferPool& metadata,PreparedNumericGather& out) noexcept;
// Immutable donor and exclusively writable destination/scratch, all disjoint from descriptors/donors.
// Full producer frame validation occurs at admission; only relevant row/population values are read here.
// History empty/unavailable -> unavailable, no invented row. Ordinary projection ignores row selection.
// Destination must use prepared contract/layout and source-projected actual extents, capacity >= requirements.
// Scratch may change on error; destination bytes, metadata and out remain unchanged.
// Output contract/planes remain plan-owned, data/coords/stamp/row info caller-owned until reuse.
// retain_frame(out.frame) makes a compact independent snapshot; row info needs its own async ownership.
Status gather_numeric_frame(const PreparedNumericGather&,const DataFrameView&,NumericGatherRow,
    const NumericGatherDestination&,std::span<std::byte> scratch,NumericGatherOutput& out) noexcept;
}
