#pragma once
#include "python_export_next.h"
#include <dsp_kernels/feature_reduction.h>
#include <algorithm>

namespace originals {
// Test-owned external reduction ports reuse the numerical reduction kernel.
// All callback storage is prepared before the operation under comparison.
class PreparedSumReduction {
    struct Sink {
        std::span<float> output;
        static bool write(void* context,std::size_t row,std::span<const float> values) noexcept {
            auto& sink=*static_cast<Sink*>(context);
            if(row || values.size()!=sink.output.size())return false;
            std::copy(values.begin(),values.end(),sink.output.begin());return true;
        }
    };
    dsp_kernels::PreparedFeatureReduction kernel_;
    static bool reduce(void* context,dsp_kernels::FloatMatrixSource source,std::size_t rows,
                       std::size_t columns,std::span<float> output) noexcept {
        if(output.size()!=columns)return false;
        Sink sink{output};return static_cast<PreparedSumReduction*>(context)->kernel_.process(source,rows,{&sink,Sink::write});
    }
public:
    PreparedSumReduction(std::size_t rows,std::size_t columns) {
        require(kernel_.prepare(rows,columns,{dsp_kernels::FeatureReduction::Sum,false}),"prepare explicit custom sum port");
    }
    dsp_kernels::FloatMatrixReductionPort port() noexcept {return {this,reduce};}
    std::size_t workspace_bytes() const noexcept {return kernel_.workspace_bytes();}
};
}
