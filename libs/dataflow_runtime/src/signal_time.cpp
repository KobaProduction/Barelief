#include "signal_time.h"

namespace dataflow_runtime::detail {
using namespace signal_contracts;
bool spectral_vector(const NumericContract& c) noexcept {return c.shape.axes[0]==Axis::Bin || c.shape.axes[0]==Axis::Band || c.coordinates[0].unit==NumericUnit::Hertz;}
struct WideUnsigned {std::uint64_t high=0,low=0;};
WideUnsigned multiply_wide(std::uint64_t a,std::uint64_t b) noexcept {
    const std::uint64_t al=a&0xffffffffULL,ah=a>>32,bl=b&0xffffffffULL,bh=b>>32;
    const std::uint64_t first=al*bl,second=ah*bl+(first>>32),third=al*bh+(second&0xffffffffULL);
    return {ah*bh+(second>>32)+(third>>32),(third<<32)|(first&0xffffffffULL)};
}
// Exact comparison of nonnegative diagnostic durations, including ratios
// whose difference is below double precision.
bool latency_greater(Ratio a,Ratio b) noexcept {
    const auto left=multiply_wide(std::uint64_t(a.numerator),b.denominator);
    const auto right=multiply_wide(std::uint64_t(b.numerator),a.denominator);
    return left.high>right.high || (left.high==right.high && left.low>right.low);
}
bool divide_bit(WideUnsigned& remainder,WideUnsigned numerator,WideUnsigned denominator,
    int bit,std::uint64_t& quotient) noexcept {
    const bool carry=(remainder.high>>63)!=0;
    remainder.high=(remainder.high<<1)|(remainder.low>>63);
    remainder.low=(remainder.low<<1)|((bit>=64?numerator.high:numerator.low)>>(bit%64)&1);
    if(!carry && (remainder.high<denominator.high ||
        (remainder.high==denominator.high && remainder.low<denominator.low)))return true;
    const bool borrow=remainder.low<denominator.low;
    remainder.low-=denominator.low;
    remainder.high-=denominator.high+std::uint64_t(borrow);
    if(bit>=64)return false;
    quotient|=std::uint64_t{1}<<bit;
    return true;
}
// Exact floor of two products, without compiler-specific 128-bit integers.
// The division is bounded to 128 steps and performs no allocation.
bool divide_products(std::uint64_t a,std::uint64_t b,std::uint64_t c,std::uint64_t d,std::uint64_t& out,std::uint64_t* fraction) noexcept {
    const auto numerator=multiply_wide(a,b),denominator=multiply_wide(c,d);
    if(!denominator.high && !denominator.low)return false;
    WideUnsigned remainder;out=0;
    for(int bit=127;bit>=0;--bit)if(!divide_bit(remainder,numerator,denominator,bit,out))return false;
    if(fraction){if(remainder.high)return false;*fraction=remainder.low;}return true;
}
bool convert_rate_ticks(std::int64_t ticks,std::uint32_t input,std::uint32_t output,std::int64_t& converted) noexcept {
    const auto magnitude=ticks<0 ? std::uint64_t(-(ticks+1))+1 : std::uint64_t(ticks);
    std::uint64_t value=0,fraction=0;
    if(!divide_products(magnitude,output,input,1,value,&fraction))return false;
    if(ticks<0 && fraction){if(value==UINT64_MAX)return false;++value;}
    const auto negative_limit=std::uint64_t(INT64_MAX)+1;
    if(value>std::uint64_t(INT64_MAX)){
        if(ticks>=0 || value!=negative_limit)return false;
        converted=INT64_MIN;
    }else converted=ticks<0 ? -std::int64_t(value) : std::int64_t(value);
    return true;
}
bool same_fraction(Ratio a,Ratio b) noexcept {
    const auto x=multiply_wide(std::uint64_t(a.numerator),b.denominator),y=multiply_wide(std::uint64_t(b.numerator),a.denominator);
    return x.high==y.high&&x.low==y.low;
}
}
