#pragma once
#include <dsp_kernels/pcm_resampler.h>
#include <vector>
// Benchmark-only independent indexed FIR. Same prepared coefficients and
// interpolated phases as production, double accumulation, no platform shortcuts.
class CorrectedDoubleReference {
public:
    CorrectedDoubleReference(const dsp_kernels::PcmResampler& op,unsigned in,unsigned out):
        geometry_(op.filter_geometry()),weights_(op.coefficients()),history_(2*geometry_.taps),in_(in),out_(out){}
    void reset(){std::fill(history_.begin(),history_.end(),0.f);position_=until_=fraction_=0;}
    bool process(std::span<const float> input,std::span<float> output,std::size_t& written){
        written=0;const auto width=geometry_.taps;
        for(const auto value:input){history_[position_]=history_[position_+width]=value;
            while(!until_){const auto back=fraction_?out_-fraction_:0;
                const auto scaled=std::uint64_t(back)*geometry_.phases;const auto phase=std::size_t(scaled/out_);
                const double blend=double(scaled%out_)/out_;double a=0,b=0;
                for(std::size_t k=0;k<width;++k){const double x=history_[position_+k];
                    a+=x*weights_[phase*width+k];b+=x*weights_[(phase+1)*width+k];}
                if(written>=output.size())return false;
                output[written++]=float(a+blend*(b-a));
                const auto old=fraction_;const auto total=std::uint64_t(fraction_)+in_;
                until_=std::size_t(total/out_)+(total%out_!=0)-(old!=0);fraction_=std::uint32_t(total%out_);
            }
            --until_;position_=position_?position_-1:width-1;
        }
        return true;
    }
private:
    dsp_kernels::PcmResampler::Geometry geometry_;
    std::span<const float> weights_;
    std::vector<float> history_;
    unsigned in_,out_;std::size_t position_=0,until_=0;std::uint32_t fraction_=0;
};
