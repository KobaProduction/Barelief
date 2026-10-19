#pragma once
#include "prepared_node.h"
#include "music_execution.h"
#include <dsp_kernels/pcm_resampler.h>
#include <dsp_kernels/oscillator_bank.h>
#include <algorithm>
namespace dataflow_runtime::detail {
struct NumericBuffer {
    std::vector<std::byte> bytes;
    std::vector<float> floats; // Native Float32 objects; no byte-to-float type punning.
    std::vector<PlaneLayout> planes;
    std::shared_ptr<const std::vector<std::byte>> silence;
    std::size_t silence_bytes=0;
    std::array<std::vector<std::byte>,2> coordinates;
    std::array<std::size_t,2> actual{1,1};
    std::span<std::byte> storage() noexcept {return floats.empty()?std::span(bytes):std::as_writable_bytes(std::span(floats));}
    std::span<const std::byte> storage() const noexcept {return silence?std::span<const std::byte>(*silence).first(silence_bytes):floats.empty()?std::span<const std::byte>(bytes):std::as_bytes(std::span(floats));}
};
struct HistoryRouteState {
    NodeId producer=-1;
    std::uint64_t ordinal=0,epoch=0,generation=0;
    bool valid=false;
};
// Cold-owned optional operation: adding a resampler must not inflate every
// existing node's inline state on memory-constrained targets. Context snapshots
// deep-copy mutable FIR state; refresh_state copies into prepared storage instead.
struct ResamplerKernel {
    std::unique_ptr<dsp_kernels::PreparedResampler> value=std::make_unique<dsp_kernels::PreparedResampler>();
    ResamplerKernel()=default;
    ResamplerKernel(const ResamplerKernel& other):value(std::make_unique<dsp_kernels::PreparedResampler>(*other.value)){}
    ResamplerKernel& operator=(const ResamplerKernel& other){if(this!=&other)value=std::make_unique<dsp_kernels::PreparedResampler>(*other.value);return *this;}
    ResamplerKernel(ResamplerKernel&&) noexcept=default;
    ResamplerKernel& operator=(ResamplerKernel&&) noexcept=default;
    dsp_kernels::PreparedResampler& operation() noexcept {return *value;}
    const dsp_kernels::PreparedResampler& operation() const noexcept {return *value;}
};
struct OscillatorKernel {
    std::unique_ptr<dsp_kernels::OscillatorBank> value=std::make_unique<dsp_kernels::OscillatorBank>();
    OscillatorKernel()=default;
    OscillatorKernel(const OscillatorKernel& x):value(std::make_unique<dsp_kernels::OscillatorBank>(*x.value)){}
    OscillatorKernel& operator=(const OscillatorKernel& x){if(this!=&x)value=std::make_unique<dsp_kernels::OscillatorBank>(*x.value);return *this;}
    OscillatorKernel(OscillatorKernel&&) noexcept=default;OscillatorKernel& operator=(OscillatorKernel&&) noexcept=default;
};
struct MixerSampleProvenance {Ratio latency_seconds{0,1};bool frequency_dependent_latency=false;};
// Only source-aligned operations allocate this extra per-sample ledger. Integer
// signal positions are never converted to double, including beyond 2^53.
struct AlignedSample {std::int64_t position=0;Ratio fractional_delay{0,1};};
struct MixerQueue {
    std::vector<float> samples;std::vector<MixerSampleProvenance> provenance;std::size_t head=0,count=0;std::uint64_t dropped=0,underrun=0;
    bool discontinuity=false;
    std::array<std::vector<std::byte>,2> coordinates;
    std::array<FrameStamp,2> stamps{};std::size_t stamp_head=0,stamp_count=0;
    std::vector<AlignedSample> aligned;
    FrameStamp last_input{};bool input_marker=false;
    FrameStamp boundary{};bool boundary_marker=false;
    // Group progress uses interval starts, not ends: sequential delivery of
    // two matching oversized capture packets must not expire the first lane.
    std::int64_t aligned_start_watermark=0;bool aligned_start_valid=false;
};
struct WledInputState {
    std::array<FrameStamp,3> stamps{};
    std::array<bool,3> pending{};
    std::array<float,2> scalars{};
    std::vector<char> stream;
    std::size_t stream_size=0,count=0;
    FrameStamp emitted{};
    bool published=false;
    static bool same_frame(const FrameStamp& a,const FrameStamp& b) noexcept {
        return a.producer_epoch==b.producer_epoch && a.generation==b.generation && a.epoch==b.epoch &&
            a.graph_revision==b.graph_revision && a.timestamp_ticks==b.timestamp_ticks &&
            a.source_sequence==b.source_sequence && a.sequence==b.sequence &&
            a.has_sample_interval==b.has_sample_interval &&
            (!a.has_sample_interval || (a.sample_begin==b.sample_begin && a.sample_count==b.sample_count));
    }
    void reset() noexcept {pending={};published=false;stream_size=count=0;}
    void retain(std::size_t lane,const FrameStamp& stamp) noexcept {
        stamps[lane]=stamp;stamps[lane].stream_id={};pending[lane]=true;
    }
    bool already_published(const FrameStamp& stamp) const noexcept {
        if(!published || stamp.producer_epoch!=emitted.producer_epoch || stamp.generation!=emitted.generation ||
           stamp.epoch!=emitted.epoch || stamp.graph_revision!=emitted.graph_revision)return false;
        return stamp.timestamp_ticks<emitted.timestamp_ticks ||
            (stamp.timestamp_ticks==emitted.timestamp_ticks && stamp.sequence<=emitted.sequence);
    }

    bool ready(const PlanNode& node) const noexcept {
        if(!pending[0] || already_published(stamps[0]))return false;
        for(std::size_t lane=1;lane<3;++lane)
            if(node.endpoint_plan->inputs[lane].parent!=absent &&
               (!pending[lane] || !same_frame(stamps[0],stamps[lane])))return false;
        return true;
    }
};

struct State {
    // A node owns only its prepared numerical operation; contexts copy independently.
    std::variant<std::monostate,dsp_kernels::MagnitudeSpectrum,dsp_kernels::MelFilterBank,ResamplerKernel,OscillatorKernel,PreparedOperationKernel> kernel;
    std::vector<float> left, values, coefficients, pcm;
    std::size_t window_extent=0;
    std::vector<MixerQueue> mixer_queues;
    NodeStats stats;
    std::uint64_t pending_self_nanoseconds=0;
    std::vector<double> frequencies;
    std::size_t frames = 0, oldest = 0, progress = 0, hop = 0;
    std::uint64_t remainder=0;
    std::uint64_t output_sequence = 0;
    std::array<std::uint64_t, 16> retired{};
    std::size_t retired_count = 0, retired_next = 0;
    bool pending_discontinuity = false;
    std::vector<NumericBuffer> numeric_outputs;
    std::vector<std::vector<std::byte>> route_masks;
    std::vector<HistoryRouteState> route_state;
    NumericBuffer numeric_zero;
    std::vector<std::byte> numeric_scratch,gather_payload,gather_metadata,gather_scratch;
    std::vector<std::byte> input_gather_payload,input_gather_metadata,input_gather_scratch;
    std::vector<PlaneLayout> row_planes;
    std::vector<DataFrameView> numeric_frames;
    std::vector<char> stream_storage;
    std::shared_ptr<const void> ingress_owner; // Declared before handles: released after them.
    OwnedDataFrame owned_ingress;
    FrameStamp numeric_stamp;
    std::vector<FrameStamp> source_stamps;
    std::vector<bool> source_markers;
    std::uint64_t last_sequence=0;
    std::uint64_t row_ordinal=0;
    NodeId history_origin=-1;
    bool history_origin_valid=false;
    bool numeric_marker=false;
    // Allocated only by WLED preparation; context snapshots deep-copy the latch.
    std::vector<WledInputState> wled_inputs;
    void clear_operation_inputs() noexcept {
        for(auto& input:wled_inputs)input.reset();
        for(auto& queue:mixer_queues){queue.head=queue.count=queue.stamp_head=queue.stamp_count=0;queue.discontinuity=true;queue.input_marker=queue.boundary_marker=queue.aligned_start_valid=false;}
    }
    void clear_kernel() noexcept {
        if(auto* resampler=std::get_if<ResamplerKernel>(&kernel))resampler->operation().reset();
        if(auto* music=std::get_if<PreparedOperationKernel>(&kernel))music->reset();
    }
    void clear_history_state() noexcept {
        window_extent=0;frames=oldest=progress=remainder=hop=0;
        last_sequence=0;numeric_stamp.stream_id={};row_ordinal=0;history_origin_valid=false;
        for(auto& route:route_state)route={};
        for(auto& mask:route_masks)std::fill(mask.begin(),mask.end(),std::byte{0});
    }

    void clear() noexcept {
        clear_history_state();
        clear_kernel();
        std::fill(pcm.begin(),pcm.end(),0);
        pending_discontinuity=true;numeric_marker=false;owned_ingress={};ingress_owner.reset();
        clear_operation_inputs();
    }
};
void clear_node_state(const PlanNode&,State&) noexcept;
void stamp_state(State&,const FrameStamp&) noexcept;
}
