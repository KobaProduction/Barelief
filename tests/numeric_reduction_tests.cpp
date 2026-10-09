#include <dsp_kernels/numeric_reduce.h>
#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <cfenv>
#include <new>
namespace {bool watch_allocations=false;std::size_t allocations=0;}
void* operator new(std::size_t n){if(watch_allocations)++allocations;if(auto* p=std::malloc(n?n:1))return p;std::abort();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete[](void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
void operator delete[](void* p,std::size_t) noexcept {std::free(p);}
namespace {void check(bool x,const char* text){if(!x){std::cerr<<text<<'\n';std::exit(3);}}}
int main() {
    using namespace signal_contracts;using namespace dsp_kernels;
    const std::array<std::uint32_t,1> channels{0};
    NumericContract input;input.dtype=DType::UInt64;input.semantic=Semantic::Numeric;input.channel_order=channels;
    input.shape.rank=Rank::Vector;input.shape.axes={Axis::Declared,Axis::None};input.shape.extents[0]={3,3};
    auto output=input;output.dtype=DType::Float64;output.semantic=Semantic::Scalar;output.shape={};
    const KernelLimits limits{8,1,64,64,64};const FrameLimits frames{1,8,64,1024,256,64};
    ReductionRequest request{input,output,ScalarReduction::SignedMean,PrecisionLoss::AllowDeclared,Rounding::NearestTiesEven,limits,frames};
    PreparedReduction plan;check(prepare_reduction(request,{true,true,true},plan)==NumericStatus::Ok,"exact64 mean reduction prepare");
    std::array<std::uint64_t,3> values{9007199254740993ULL,9007199254740993ULL,9007199254740993ULL};
    const std::array<PlaneLayout,1> in_planes{{{0,{8,0}}}},out_planes{{{0,{0,0}}}};
    std::array<std::byte,8> destination{},scratch{};destination.fill(std::byte{0xa5});
    NumericView in{&input,std::as_bytes(std::span(values)),in_planes,{3,1},{},{}};
    MutableNumericView out{&output,destination,out_planes,{1,1},{},{}};
    check(reduce(plan,in,out,scratch).status==NumericStatus::Ok,"exact64 mean aggregate");
    std::uint64_t bits;std::memcpy(&bits,destination.data(),8);check(bits==0x4340000000000000ULL,"exact64 mean final rounding independent oracle");
    request.operation=ScalarReduction::Rms;check(prepare_reduction(request,{true,true,true},plan)==NumericStatus::Ok,"exact64 RMS prepare");
    check(reduce(plan,in,out,scratch).status==NumericStatus::Ok,"exact64 sum squares aggregate");
    std::memcpy(&bits,destination.data(),8);check(bits==0x4340000000000000ULL,"exact64 RMS final rounding independent oracle");
    request.precision_loss=PrecisionLoss::RejectInexact;
    check(prepare_reduction(request,{true,true,true},plan)==NumericStatus::Ok,"exact64 RMS reject mode prepares after exact rational proof");
    destination.fill(std::byte{0xa5});const auto before=destination;
    check(reduce(plan,in,out,scratch).status==NumericStatus::PrecisionLoss && destination==before,"exact64 RMS inexact output atomically rejected");
    values={UINT64_MAX,UINT64_MAX,UINT64_MAX};request.precision_loss=PrecisionLoss::AllowDeclared;
    check(prepare_reduction(request,{true,true,true},plan)==NumericStatus::Ok && reduce(plan,in,out,scratch).status==NumericStatus::Ok,"UInt64max squares never overflow64 accumulator");
    std::memcpy(&bits,destination.data(),8);check(bits==0x43f0000000000000ULL,"UInt64max RMS rounds only final aggregate to2^64");
    input.dtype=DType::Int64;request.input=input;values={UINT64_C(0x8000000000000000),UINT64_C(0x8000000000000000),UINT64_C(0x8000000000000000)};
    request.precision_loss=PrecisionLoss::RejectInexact;check(prepare_reduction(request,{true,true,true},plan)==NumericStatus::Ok && reduce(plan,in,out,scratch).status==NumericStatus::Ok,"Int64min square exact limbs");
    std::memcpy(&bits,destination.data(),8);check(bits==0x43e0000000000000ULL,"Int64min RMS exact positive2^63");
    request.operation=ScalarReduction::SignedMean;values={UINT64_C(0x8000000000000000),UINT64_C(0x7fffffffffffffff),1};
    check(prepare_reduction(request,{true,true,true},plan)==NumericStatus::Ok && reduce(plan,in,out,scratch).status==NumericStatus::Ok,"signed exact cancellation before final rounding");
    std::memcpy(&bits,destination.data(),8);check(bits==0,"Int64min+Int64max+1 exact meanzero");
    values={1,0,0};destination=before;
    check(reduce(plan,in,out,scratch).status==NumericStatus::PrecisionLoss && destination==before,"mean exact1/3 rejects binary inexact");
    request.precision_loss=PrecisionLoss::AllowDeclared;check(prepare_reduction(request,{true,true,true},plan)==NumericStatus::Ok,"mean rational rounding prepare");
    for(const auto mode:{FE_TONEAREST,FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO}) {
        check(std::fesetround(mode)==0,"fenv selectable");check(reduce(plan,in,out,scratch).status==NumericStatus::Ok,"rational mean software IEEE pack");
        std::memcpy(&bits,destination.data(),8);check(bits==0x3fd5555555555555ULL && std::fegetround()==mode,"1/3 nearest independent fenv and preserves roundingmode");
    }
    std::fesetround(FE_TONEAREST);
    request.operation=ScalarReduction::MaximumAbsolute;output.dtype=DType::UInt64;request.output=output;values={UINT64_C(0x8000000000000000),0,1};
    request.precision_loss=PrecisionLoss::RejectInexact;check(prepare_reduction(request,{true,true,true},plan)==NumericStatus::Ok && reduce(plan,in,out,scratch).status==NumericStatus::Ok,"Int64min absolute fitsUInt64");
    std::memcpy(&bits,destination.data(),8);check(bits==0x8000000000000000ULL,"exact unsigned absolute Int64min");
    output.dtype=DType::Int64;request.output=output;destination=before;
    check(prepare_reduction(request,{true,true,true},plan)==NumericStatus::Ok && reduce(plan,in,out,scratch).status==NumericStatus::Overflow && destination==before,"Int64min absolute refuses signed narrowing atomically");
    input.dtype=DType::Float64;request.input=input;output.dtype=DType::Float64;request.output=output;
    request.operation=ScalarReduction::Rms;request.precision_loss=PrecisionLoss::AllowDeclared;
    values={UINT64_C(0x7fefffffffffffff),UINT64_C(0x7fefffffffffffff),UINT64_C(0x7fefffffffffffff)};
    check(prepare_reduction(request,{true,true,true},plan)==NumericStatus::Ok,"Float64 scaled RMS prepare");
    for(const auto mode:{FE_DOWNWARD,FE_UPWARD}) {
        std::fesetround(mode);std::feraiseexcept(FE_INVALID);
        check(reduce(plan,in,out,scratch).status==NumericStatus::Ok,"DBLmax scaled RMS avoids square overflow");
        std::memcpy(&bits,destination.data(),8);check(bits==0x7fefffffffffffffULL && std::fegetround()==mode && std::fetestexcept(FE_INVALID),"scaled RMS restores ambient fenv flags");
    }
    std::fesetround(FE_TONEAREST);std::feclearexcept(FE_ALL_EXCEPT);
    values[2]=0x7ff0000000000001ULL;destination=before;
    const auto nonfinite=reduce(plan,in,out,scratch);check(nonfinite.status==NumericStatus::NonFiniteInput && nonfinite.error_element==2 && destination==before,"late signaling NaN rejected without partial scalar write");
    values={0x3ff0000000000000ULL,0x4000000000000000ULL,0x4008000000000000ULL};
    watch_allocations=true;
    for(unsigned i=0;i<32;++i)check(reduce(plan,in,out,scratch).status==NumericStatus::Ok,"prepared scaled RMS steady-state");
    watch_allocations=false;check(allocations==0,"reduction no C++ heap after prepare");
    const auto stable=destination;
    check(reduce(plan,in,out,destination).status==NumericStatus::InvalidLayout && destination==stable,"reduction scratch/output alias rejected atomically");
    check(reduce(plan,in,out,std::span(scratch).first(7)).status==NumericStatus::BufferTooSmall && destination==stable,"reduction insufficient scratch atomic");
    request.operation=ScalarReduction::SignedMean;values={1,0,0};
    check(prepare_reduction(request,{true,true,true},plan)==NumericStatus::Ok,"native Float64 subnormal mean prepare");
    for(const auto mode:{FE_TONEAREST,FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO})for(const bool negative:{false,true}) {
        values={negative?UINT64_C(0x8000000000000001):UINT64_C(1),0,0};
        std::fesetround(mode);std::feclearexcept(FE_ALL_EXCEPT);std::feraiseexcept(FE_INVALID|FE_DIVBYZERO);
        const int ambient_flags=std::fetestexcept(FE_ALL_EXCEPT);
        const auto native_underflow=reduce(plan,in,out,scratch);std::memcpy(&bits,destination.data(),8);
        check(native_underflow.status==NumericStatus::Ok && bits==(negative?UINT64_C(0x8000000000000000):0) && native_underflow.loss_flags==9,
              "native Float64 signed minsub divided3 reports rounded and underflow");
        check(std::fegetround()==mode && std::fetestexcept(FE_ALL_EXCEPT)==ambient_flags,"native Float64 underflow restores exact caller flags");
    }
    std::fesetround(FE_TONEAREST);std::feclearexcept(FE_ALL_EXCEPT);
    input.dtype=DType::Float32;request.input=input;request.operation=ScalarReduction::SignedMean;
    values={0x7f7fffff,1,0xff7fffff};
    check(prepare_reduction(request,{true,true,true},plan)==NumericStatus::Ok,"Float32 exact dyadic mean prepares");
    check(reduce(plan,in,out,scratch).status==NumericStatus::Ok,"Float32 cancellation keeps minimum subnormal aggregate");
    std::memcpy(&bits,destination.data(),8);check(bits==0x3685555555555555ULL,"FLTmax+minsub-FLTmax divided3 finalFloat64 independent oracle");
    output.dtype=DType::Float32;request.output=output;request.precision_loss=PrecisionLoss::RejectInexact;
    request.operation=ScalarReduction::Rms;values={0x3f800000,0x3f800000,0x3f800000};
    check(prepare_reduction(request,{true,true,false},plan)==NumericStatus::Ok,"Float32 RMS requires no nativeFloat64 capability");
    check(reduce(plan,in,out,scratch).status==NumericStatus::Ok,"Float32 exact RMS one portable path");
    std::uint32_t bits32;std::memcpy(&bits32,destination.data(),4);check(bits32==0x3f800000,"Float32 exact RMS one oracle");
    values={0x7f7fffff,0x7f7fffff,0x7f7fffff};
    check(reduce(plan,in,out,scratch).status==NumericStatus::Ok,"Float32 max squares fit bounded wide limbs");
    std::memcpy(&bits32,destination.data(),4);check(bits32==0x7f7fffff,"Float32 max RMS finalexact oracle");
    values={1,0,0};destination.fill(std::byte{0xa5});const auto tiny_before=destination;
    request.operation=ScalarReduction::SignedMean;
    check(prepare_reduction(request,{true,true,false},plan)==NumericStatus::Ok,"Float32 default inexact check prepare");
    check(reduce(plan,in,out,scratch).status==NumericStatus::PrecisionLoss && destination==tiny_before,"Float32 minimumsubnormal divided3 rejects final loss");
    request.precision_loss=PrecisionLoss::AllowDeclared;check(prepare_reduction(request,{true,true,false},plan)==NumericStatus::Ok,"Float32 software rounded mean prepare");
    for(const auto mode:{FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO}) {
        std::fesetround(mode);std::feraiseexcept(FE_INVALID);
        const auto tiny=reduce(plan,in,out,scratch);std::memcpy(&bits32,destination.data(),4);
        check(tiny.status==NumericStatus::Ok && bits32==0 && (tiny.loss_flags&std::uint32_t(LossFlag::Rounded)) &&
              (tiny.loss_flags&std::uint32_t(LossFlag::Underflow)),"Float32 portable underflow loss flags and ties policy");
        check(std::fegetround()==mode && std::fetestexcept(FE_INVALID),"Float32 portable path leaves ambient fenv untouched");
    }
    std::fesetround(FE_TONEAREST);std::feclearexcept(FE_ALL_EXCEPT);
    request.operation=ScalarReduction::Rms;values={1,1,0};
    check(prepare_reduction(request,{true,true,false},plan)==NumericStatus::Ok && reduce(plan,in,out,scratch).status==NumericStatus::Ok,"Float32 tiny RMS software square root rounding");
    std::memcpy(&bits32,destination.data(),4);check(bits32==1,"Float32 sqrt2/3 minimumsubnormal rounds nearest1");
    watch_allocations=true;
    for(unsigned i=0;i<16;++i)check(reduce(plan,in,out,scratch).status==NumericStatus::Ok,"Float32 software RMS bounded steady-state");
    watch_allocations=false;check(allocations==0,"Float32 exact software reduction allocates no C++ heap after prepare");
    std::cout<<"numeric reduction exact oracles passed\n";
}
