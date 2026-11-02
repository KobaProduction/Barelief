#include <dsp_kernels/sample_rotation.h>
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <functional>
namespace dsp_kernels {
namespace {
template<typename Scalar> bool partial_overlap(std::span<const Scalar> input,std::span<Scalar> output) noexcept {
    const auto left=reinterpret_cast<std::uintptr_t>(input.data()),right=reinterpret_cast<std::uintptr_t>(output.data());
    if(left==right)return false;
    const auto distance=left<right?right-left:left-right;
    return distance<input.size()*sizeof(Scalar);
}

template<typename Scalar> void write_rotation(std::span<const Scalar> input,std::span<Scalar> output,
    std::ptrdiff_t shift) noexcept {
    if(input.data()==output.data()){
        std::rotate(output.begin(),output.begin()+shift,output.end());
        return;
    }
    const auto tail=input.size()-std::size_t(shift);
    std::copy_n(input.begin()+shift,tail,output.begin());
    std::copy_n(input.begin(),std::size_t(shift),output.begin()+std::ptrdiff_t(tail));
}
template<typename Scalar> void write_embedded_frame(std::span<const Scalar> frame,std::span<Scalar> output,
    std::size_t left,bool shifted) noexcept {
    std::fill(output.begin(),output.end(),Scalar{});
    if(!shifted){
        std::copy_n(frame.begin(),std::min(frame.size(),output.size()),output.begin());
        return;
    }
    std::copy(frame.begin()+std::ptrdiff_t(left),frame.end(),output.begin());
    std::copy_n(frame.begin(),left,output.end()-std::ptrdiff_t(left));
}
template<typename Scalar> bool rotate_values(std::span<const Scalar> input,std::span<Scalar> output,std::ptrdiff_t shift) noexcept {
    if(input.size()!=output.size() || input.size()>std::size_t(PTRDIFF_MAX) || input.size()>SIZE_MAX/sizeof(Scalar))return false;
    if(input.empty())return true;
    if(partial_overlap(input,output))return false;
    shift%=std::ptrdiff_t(input.size());
    if(shift<0)shift+=std::ptrdiff_t(input.size());
    write_rotation(input,output,shift);
    return true;
}
template<typename Scalar> bool embed_values(std::span<const Scalar> frame,std::span<Scalar> output,bool shifted) noexcept {
    if(frame.empty() || output.empty() || frame.size()>std::size_t(PTRDIFF_MAX) ||
        !std::all_of(frame.begin(),frame.end(),[](Scalar x){return std::isfinite(x);}))return false;
    const auto left=frame.size()/2,right=frame.size()-left;
    if(shifted && output.size()<std::max(left,right))return false;
    if(frame.data()==output.data() && frame.size()==output.size())
        return !shifted || rotate_values(frame,output,std::ptrdiff_t(left));
    const std::less<const Scalar*> before;
    if(before(frame.data(),output.data()+output.size()) && before(output.data(),frame.data()+frame.size()))return false;
    write_embedded_frame(frame,output,left,shifted);
    return true;
}
}
bool embed_fourier_frame(std::span<const float> frame,std::span<float> output,bool shifted) noexcept {
    return embed_values(frame,output,shifted);
}
bool embed_fourier_frame(std::span<const double> frame,std::span<double> output,bool shifted) noexcept {
    return embed_values(frame,output,shifted);
}
bool rotate_samples(std::span<const float> input,std::span<float> output,std::ptrdiff_t shift) noexcept {
    return rotate_values(input,output,shift);
}
bool rotate_samples(std::span<const double> input,std::span<double> output,std::ptrdiff_t shift) noexcept {
    return rotate_values(input,output,shift);
}
}
