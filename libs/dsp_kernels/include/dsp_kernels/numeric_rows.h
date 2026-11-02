#pragma once
#include <cstddef>
#include <span>

namespace dsp_kernels {
// Synchronous views of caller-owned numeric rows. A processor never retains
// these callbacks or their context beyond process(). Layout/ownership adapters
// can expose planar, strided or circular storage without cloning a history.
// Scalar reader for generated/ragged flattened sequences; synchronous only.
struct FloatSequenceSource {
    const void* context=nullptr;
    bool (*read)(const void*,std::size_t,float&) noexcept=nullptr;
};
struct FloatMatrixSource {
    const void* context=nullptr;
    bool (*read)(const void*,std::size_t,std::span<float>) noexcept=nullptr;
};
// Explicit batch axis; writer consumes each borrowed row synchronously.
struct FloatMatrixBatchDestination {
    void* context=nullptr;
    bool (*write)(void*,std::size_t channel,std::size_t row,std::span<const float>) noexcept=nullptr;
};
// Optional prepared reduction over matrix rows, one output per column.
struct FloatMatrixReductionPort {
    void* context=nullptr;
    bool (*reduce)(void*,FloatMatrixSource,std::size_t rows,std::size_t columns,std::span<float>) noexcept=nullptr;
};
struct FloatMatrixDestination {
    void* context=nullptr;
    bool (*write)(void*,std::size_t,std::span<const float>) noexcept=nullptr;
};
// Stack adapter for one synchronous batch-channel invocation.
struct FloatMatrixBatchChannel {
    FloatMatrixBatchDestination output;
    std::size_t channel=0;
    static bool write(void* context,std::size_t row,std::span<const float> values) noexcept {
        auto& owner=*static_cast<FloatMatrixBatchChannel*>(context);
        return owner.output.write && owner.output.write(owner.output.context,owner.channel,row,values);
    }
    FloatMatrixDestination destination() noexcept {return {this,write};}
};
}
