#pragma once
#include <array>
#include <cstdint>
#include <bit>
#include <algorithm>
#include <cfenv>

// Detect the standard-library arithmetic facility, not an OS/SDK/CPU name.
// A libc without these facilities cannot honor declared native IEEE64 rounding
// and exception observation. Exact software bit arithmetic below needs none.
#if defined(FE_TONEAREST) && defined(FE_ALL_EXCEPT) && defined(FE_INEXACT) && defined(FE_UNDERFLOW)
#define DSP_KERNELS_HAS_DECLARED_IEEE64_ENVIRONMENT 1
#else
#define DSP_KERNELS_HAS_DECLARED_IEEE64_ENVIRONMENT 0
#endif

// Private exact binary arithmetic; no public numeric registry or graph DTO.
namespace dsp_kernels::numeric_detail {
inline constexpr bool declared_ieee64_environment_available=DSP_KERNELS_HAS_DECLARED_IEEE64_ENVIRONMENT;
// IEEE64 declared arithmetic uses a scoped thread floating environment. Restores
// caller rounding and exception flags, including every rejection path.
#if DSP_KERNELS_HAS_DECLARED_IEEE64_ENVIRONMENT
class NearestEnvironment {
    std::fenv_t saved_{};
    bool saved=false;
public:
    bool ready=false;
    explicit NearestEnvironment(bool enabled=true) noexcept {
        if(!enabled){ready=true;return;}
        saved=std::fegetenv(&saved_)==0;
        if(saved)ready=std::fesetround(FE_TONEAREST)==0 && std::feclearexcept(FE_ALL_EXCEPT)==0;
    }
    ~NearestEnvironment(){
        if(saved){
            // Some standard runtimes merge current SSE status into fesetenv.
            // Clear this scope's flags first; saved_ then restores exactly the
            // caller's status and control state, including preexisting flags.
            std::feclearexcept(FE_ALL_EXCEPT);
            std::fesetenv(&saved_);
        }
    }
    NearestEnvironment(const NearestEnvironment&)=delete;
    NearestEnvironment& operator=(const NearestEnvironment&)=delete;
};
#endif
struct Magnitude {
    static constexpr unsigned word_count=20,bit_capacity=word_count*32;
    std::array<std::uint32_t,word_count> words{};
    static Magnitude from(std::uint64_t value) noexcept {
        Magnitude out; out.words[0]=std::uint32_t(value);out.words[1]=std::uint32_t(value>>32);return out;
    }
    bool zero() const noexcept {for(auto w:words)if(w)return false;return true;}
    unsigned bits() const noexcept {
        for(unsigned i=word_count;i;--i)if(words[i-1])return (i-1)*32+32-std::countl_zero(words[i-1]);
        return 0;
    }
    bool bit(unsigned i) const noexcept {return i<bit_capacity && ((words[i/32]>>(i%32))&1);}
    bool any_below(unsigned n) const noexcept {
        if(n>=bit_capacity)return !zero();
        for(unsigned i=0;i<n/32;++i)if(words[i])return true;
        return n%32 && (words[n/32]&((std::uint32_t{1}<<(n%32))-1));
    }
    std::uint64_t low() const noexcept {return words[0]|(std::uint64_t(words[1])<<32);}
    int compare(const Magnitude& b) const noexcept {
        for(unsigned i=word_count;i;--i)if(words[i-1]!=b.words[i-1])return words[i-1]<b.words[i-1]?-1:1;
        return 0;
    }
    bool left(unsigned n) noexcept {
        if(zero())return true;
        if(n>=bit_capacity || bits()>bit_capacity-n)return false;
        Magnitude out;
        for(unsigned i=0;i<word_count;++i)if(words[i]) {
            const auto j=i+n/32;out.words[j]|=words[i]<<(n%32);
            if(n%32 && j+1<word_count)out.words[j+1]|=words[i]>>(32-n%32);
        }
        *this=out;return true;
    }
    void right(unsigned n) noexcept {
        Magnitude out;
        if(n<bit_capacity)for(unsigned i=n/32;i<word_count;++i) {
            const auto j=i-n/32;out.words[j]|=words[i]>>(n%32);
            if(n%32 && j)out.words[j-1]|=words[i]<<(32-n%32);
        }
        *this=out;
    }
    bool add(const Magnitude& b) noexcept {
        std::uint64_t carry=0;
        for(unsigned i=0;i<word_count;++i){carry+=std::uint64_t(words[i])+b.words[i];words[i]=std::uint32_t(carry);carry>>=32;}
        return !carry;
    }
    void subtract(const Magnitude& b) noexcept {
        std::uint64_t borrow=0;
        for(unsigned i=0;i<word_count;++i){const auto a=std::uint64_t(words[i]);const auto sub=std::uint64_t(b.words[i])+borrow;
            words[i]=std::uint32_t(a-sub);borrow=a<sub;}
    }
    bool multiply_row(const Magnitude& b,Magnitude& out,unsigned row) const noexcept {
        std::uint64_t carry=0;
        for(unsigned column=0;column<word_count;++column) {
            if(row+column>=word_count) {
                if(carry || (words[row] && b.words[column]))return false;
                continue;
            }
            const auto sum=std::uint64_t(words[row])*b.words[column]+out.words[row+column]+carry;
            out.words[row+column]=std::uint32_t(sum);carry=sum>>32;
        }
        return !carry;
    }
    bool multiply(const Magnitude& b) noexcept {
        Magnitude out;
        for(unsigned row=0;row<word_count;++row)if(!multiply_row(b,out,row))return false;
        *this=out;return true;
    }
};
struct Binary { Magnitude magnitude; int exponent2=0; bool negative=false; };
enum class Special { Finite, NaN, NegativeInfinity, PositiveInfinity };
struct Decoded {Binary value;Special special=Special::Finite;};
inline Decoded decode_integer(std::uint64_t bits,unsigned width,bool is_signed) noexcept {
    const auto mask=width==64?UINT64_MAX:(std::uint64_t{1}<<width)-1;
    bits&=mask;const bool negative=is_signed && ((bits>>(width-1))&1);
    return {{Magnitude::from(negative?((~bits+1)&mask):bits),0,negative}};
}
inline Decoded decode_ieee(std::uint64_t bits,unsigned eb,unsigned mb) noexcept {
    const auto mantissa=bits&((std::uint64_t{1}<<mb)-1);
    const auto exponent=(bits>>mb)&((std::uint64_t{1}<<eb)-1);
    const bool negative=(bits>>(eb+mb))&1;
    if(exponent==((std::uint64_t{1}<<eb)-1))return {{},mantissa?Special::NaN:negative?Special::NegativeInfinity:Special::PositiveInfinity};
    const int bias=(1<<(eb-1))-1;
    return {{Magnitude::from(mantissa|(exponent?(std::uint64_t{1}<<mb):0)),int(exponent?exponent:1)-bias-int(mb),negative}};
}
inline bool combine_binary_magnitudes(Binary& a,const Binary& b,Magnitude& left,Magnitude& right) noexcept {
    if(a.negative==b.negative)return left.add(right);
    if(left.compare(right)>=0)left.subtract(right);
    else {right.subtract(left);left=right;a.negative=b.negative;}
    return true;
}
inline bool align_binary_magnitudes(const Binary& a,const Binary& b,int exponent,
                                    Magnitude& av,Magnitude& bv) noexcept {
    return av.left(unsigned(a.exponent2-exponent)) && bv.left(unsigned(b.exponent2-exponent));
}

inline bool add_nonzero_binary(Binary& a,const Binary& b) noexcept {
    const int exponent=std::min(a.exponent2,b.exponent2);
    auto av=a.magnitude,bv=b.magnitude;
    if(!align_binary_magnitudes(a,b,exponent,av,bv))return false;
    if(!combine_binary_magnitudes(a,b,av,bv))return false;
    a.magnitude=av;a.exponent2=exponent;
    if(av.zero())a.negative=false;
    return true;
}
inline bool add_binary(Binary& a,const Binary& b) noexcept {
    if(b.magnitude.zero())return true;
    if(a.magnitude.zero()){a=b;return true;}
    return add_nonzero_binary(a,b);
}
inline int absolute_compare(const Binary& a,const Binary& b) noexcept {
    if(a.magnitude.zero())return b.magnitude.zero()?0:-1;
    if(b.magnitude.zero())return 1;
    const int ah=int(a.magnitude.bits())+a.exponent2,bh=int(b.magnitude.bits())+b.exponent2;
    if(ah!=bh)return ah<bh?-1:1;
    auto av=a.magnitude,bv=b.magnitude;const int common=std::min(a.exponent2,b.exponent2);
    av.left(unsigned(a.exponent2-common));bv.left(unsigned(b.exponent2-common));return av.compare(bv);
}
inline int compare_binary(const Binary& a,const Binary& b) noexcept {
    const bool an=a.negative && !a.magnitude.zero(),bn=b.negative && !b.magnitude.zero();
    if(an!=bn)return an?-1:1;
    const int magnitude=absolute_compare(a,b);return an?-magnitude:magnitude;
}
enum class ArithmeticError { Ok, Overflow, Inexact };
struct Packed { ArithmeticError error; std::uint64_t bits; bool rounded=false, underflow=false; };
inline bool rounded_shift(Magnitude& value,int shift,bool nearest,bool& rounded) noexcept {
    if(shift>=0)return value.left(unsigned(shift));
    const auto n=unsigned(-shift);rounded=value.any_below(n);
    const bool increment=nearest && n && value.bit(n-1) && (value.any_below(n-1)||value.bit(n));
    value.right(n);
    return !increment || value.add(Magnitude::from(1));
}
inline Packed packed_ieee_significand(std::uint64_t sign,std::uint64_t significand,int leading,
                                    int minimum,int maximum,int bias,unsigned mantissa_bits,bool rounded) noexcept {
    if(significand>=(std::uint64_t{1}<<(mantissa_bits+1))){significand>>=1;++leading;}
    if(leading>maximum)return {ArithmeticError::Overflow,0};
    const bool subnormal=leading<minimum && significand<(std::uint64_t{1}<<mantissa_bits);
    const auto exponent=subnormal?0:std::uint64_t(std::max(leading,minimum)+bias);
    const auto bits=sign|(exponent<<mantissa_bits)|(significand&((std::uint64_t{1}<<mantissa_bits)-1));
    return {ArithmeticError::Ok,bits,rounded,subnormal && rounded};
}
inline Packed pack_ieee(const Binary& input,unsigned exponent_bits,unsigned mantissa_bits,bool nearest,bool allow) noexcept {
    const auto sign=std::uint64_t(input.negative)<<(exponent_bits+mantissa_bits);
    if(input.magnitude.zero())return {ArithmeticError::Ok,sign};
    const int bias=(1<<(exponent_bits-1))-1, minimum=1-bias,maximum=bias;
    int leading=int(input.magnitude.bits())-1+input.exponent2;
    if(leading>maximum)return {ArithmeticError::Overflow,0};
    const int quantum=std::max(leading,minimum)-int(mantissa_bits);
    auto rounded_value=input.magnitude;bool rounded=false;
    if(!rounded_shift(rounded_value,input.exponent2-quantum,nearest,rounded))return {ArithmeticError::Overflow,0};
    if(rounded && !allow)return {ArithmeticError::Inexact,0};
    return packed_ieee_significand(sign,rounded_value.low(),leading,minimum,maximum,bias,mantissa_bits,rounded);
}
inline Packed pack_integer(Binary value,unsigned bits,bool sign,bool nearest,bool allow) noexcept {
    bool rounded=false;
    const bool shifted=rounded_shift(value.magnitude,value.exponent2,nearest,rounded);
    const auto bound=sign?(value.negative?(std::uint64_t{1}<<(bits-1)):(std::uint64_t{1}<<(bits-1))-1):
        bits==64?UINT64_MAX:(std::uint64_t{1}<<bits)-1;
    if(!shifted || value.magnitude.compare(Magnitude::from(bound))>0 || (!sign && value.negative && !value.magnitude.zero()))
        return {ArithmeticError::Overflow,0};
    if(rounded && !allow)return {ArithmeticError::Inexact,0};
    const auto magnitude=value.magnitude.low();
    return {ArithmeticError::Ok,value.negative?~magnitude+1:magnitude,rounded};
}
// Compare a nonnegative finite dyadic (or its square) to an exact scaled ratio.
// Leading bit/exponent comparison avoids materializing huge exponent gaps;
// admitted equal-leading operands fit640bits. No floating intermediate is involved.
inline int compare_ratio(Binary value,const Magnitude& numerator,std::uint64_t denominator,bool square,int numerator_exponent2=0) noexcept {
    if(square){value.magnitude.multiply(value.magnitude);value.exponent2*=2;}
    value.magnitude.multiply(Magnitude::from(denominator));
    return compare_binary(value,Binary{numerator,numerator_exponent2,false});
}
inline std::uint64_t ratio_floor(const Magnitude& numerator,std::uint64_t denominator,bool square,
                                 int numerator_exponent2,unsigned exponent_bits,unsigned mantissa_bits,
                                 std::uint64_t high) noexcept {
    std::uint64_t low=0;
    while(low<high) {
        const auto mid=low+(high-low)/2+(high-low)%2;
        if(compare_ratio(decode_ieee(mid,exponent_bits,mantissa_bits).value,numerator,denominator,square,numerator_exponent2)<=0)
            low=mid;
        else high=mid-1;
    }
    return low;
}
inline std::uint64_t round_ratio(std::uint64_t low,const Magnitude& numerator,std::uint64_t denominator,
                                 bool square,int numerator_exponent2,unsigned exponent_bits,unsigned mantissa_bits) noexcept {
    auto midpoint=decode_ieee(low,exponent_bits,mantissa_bits).value;
    add_binary(midpoint,decode_ieee(low+1,exponent_bits,mantissa_bits).value);
    --midpoint.exponent2;
    const auto comparison=compare_ratio(midpoint,numerator,denominator,square,numerator_exponent2);
    return comparison<0 || (!comparison && (low&1))?low+1:low;
}
inline Packed pack_ratio(const Magnitude& numerator,std::uint64_t denominator,bool negative,bool square,
                         bool float32,bool nearest,bool allow,int numerator_exponent2=0,bool float32_population=false) noexcept {
    const unsigned eb=float32?8:11,mb=float32?23:52;
    const auto sign=std::uint64_t(negative)<<(eb+mb);
    if(numerator.zero())return {ArithmeticError::Ok,sign};
    // UInt32 population bound: integer mean/RMS <=2^64, Float32 mean/RMS <=FLT_MAX.
    const std::uint64_t high=float32_population?(float32?UINT64_C(0x7f7fffff):UINT64_C(0x47efffffe0000000)):
        float32?UINT64_C(0x5f800000):UINT64_C(0x43f0000000000000);
    auto low=ratio_floor(numerator,denominator,square,numerator_exponent2,eb,mb,high);
    const auto exact=compare_ratio(decode_ieee(low,eb,mb).value,numerator,denominator,square,numerator_exponent2)==0;
    if(!exact && !allow)return {ArithmeticError::Inexact,0};
    if(!exact && nearest)low=round_ratio(low,numerator,denominator,square,numerator_exponent2,eb,mb);
    const bool subnormal=((low>>mb)&((std::uint64_t{1}<<eb)-1))==0;
    return {ArithmeticError::Ok,sign|low,!exact,!exact && subnormal};
}
}
