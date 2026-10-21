#include <dsp_kernels/numeric_convert.h>
#include <array>
#include <cstring>
#include <cstdlib>
#include <iostream>
namespace {
void check(bool x,const char* text){if(!x){std::cerr<<text<<'\n';std::exit(3);}}
}
namespace {
using namespace signal_contracts;using namespace dsp_kernels;
struct PcmConversionScenario {
 const std::array<std::uint32_t,1> channels{7};NumericContract pcm,normalized;ConversionPolicy policy;PreparedConversion plan;
 const KernelLimits limits{8,1,64,64,64};const FrameLimits frames{1,8,64,1024,256,64};
 std::array<std::byte,8> input{},output{},scratch{};const std::array<PlaneLayout,1> planes{{{0,{0,0}}}};
 NumericView in,reverse;MutableNumericView out,pcm_out;std::uint64_t result=0;
 const std::uint64_t negative_subnormal=0x8000000000000001ULL;
 void packed24(){


    pcm.channel_order=channels;pcm.dtype=DType::Int32;pcm.semantic=Semantic::PCM;
    pcm.representation={StorageEncoding::PackedSigned24LE,24,BitPlacement::LeastSignificant,ChannelLayout::Planar,Normalization::SignedUnit,1};
    normalized=pcm;normalized.dtype=DType::Float64;normalized.representation={};normalized.representation.normalization=Normalization::FloatUnit;
    policy.scaling.mode=ScaleMode::PcmNormalize;


    check(prepare_conversion({pcm,normalized,policy,limits,frames},{true,true,true},plan)==NumericStatus::Ok,"packed24 normalize prepare");


    in={&pcm,input,planes,{1,1},{},{}};out={&normalized,output,planes,{1,1},{},{}};
    input[2]=std::byte{0x80};
    check(convert(plan,in,out,scratch).status==NumericStatus::Ok,"packed24 minimum converts");
    std::memcpy(&result,output.data(),8);check(result==0xbff0000000000000ULL,"packed24 min maps exact-1");
    pcm.representation.encoding=StorageEncoding::FixedLittleEndian; //valid24 in32 canonical LSB
    input[3]=std::byte{0};output.fill(std::byte{0xa5});const auto sentinel=output;
    check(prepare_conversion({pcm,normalized,policy,limits,frames},{true,true,true},plan)==NumericStatus::Ok,"24in32 LSB prepare");
    check(convert(plan,in,out,scratch).status==NumericStatus::InvalidLayout && output==sentinel,"noncanonical LSB sign extension rejected atomically");
    input[3]=std::byte{0xff};check(convert(plan,in,out,scratch).status==NumericStatus::Ok,"canonical LSB sign extension accepted");
    pcm.representation.placement=BitPlacement::MostSignificant;input={std::byte{1},std::byte{0},std::byte{0},std::byte{0x80}};
    check(prepare_conversion({pcm,normalized,policy,limits,frames},{true,true,true},plan)==NumericStatus::Ok,"24in32 MSB prepare");
    check(convert(plan,in,out,scratch).status==NumericStatus::InvalidLayout,"MSB low nonzero bits refused");
    input[0]=std::byte{0};check(convert(plan,in,out,scratch).status==NumericStatus::Ok,"MSB lowzero accepted");
 }
 void unsigned_bias(){
    pcm.dtype=DType::UInt8;pcm.representation={};pcm.representation.normalization=Normalization::UnsignedUnit;
    input.fill(std::byte{0});input[0]=std::byte{128};
    check(prepare_conversion({pcm,normalized,policy,limits,frames},{true,true,true},plan)==NumericStatus::Ok,"biased unsigned PCM prepare");
    check(convert(plan,in,out,scratch).status==NumericStatus::Ok,"unsigned PCM midpoint normalize");
    std::memcpy(&result,output.data(),8);check(result==0,"unsigned silence midpoint maps zero");
    policy.scaling.mode=ScaleMode::PcmDenormalize;policy.precision_loss=PrecisionLoss::AllowDeclared;
    check(prepare_conversion({normalized,pcm,policy,limits,frames},{true,true,true},plan)==NumericStatus::Ok,"unsigned PCM denormalize prepare");
    reverse={&normalized,input,planes,{1,1},{},{}};pcm_out={&pcm,output,planes,{1,1},{},{}};
    const std::uint64_t subnormal=1;std::memcpy(input.data(),&subnormal,8);
    check(convert(plan,reverse,pcm_out,scratch).status==NumericStatus::Ok && output[0]==std::byte{128},"positive minimum subnormal denormalizes to unsigned silence");
    std::memcpy(input.data(),&negative_subnormal,8);
    policy.rounding=Rounding::TowardZero;
    check(prepare_conversion({normalized,pcm,policy,limits,frames},{true,true,true},plan)==NumericStatus::Ok,"unsigned PCM directed denormalize prepare");
    check(convert(plan,reverse,pcm_out,scratch).status==NumericStatus::Ok && output[0]==std::byte{127},"tiny negative plusbias towardzero is127 not128");
 }
 void signed16(){
    pcm.dtype=DType::Int16;pcm.representation={StorageEncoding::FixedBigEndian,0,BitPlacement::LeastSignificant,ChannelLayout::Planar,Normalization::SignedUnit,1};
    policy.scaling.mode=ScaleMode::PcmNormalize;policy.precision_loss=PrecisionLoss::RejectInexact;policy.rounding=Rounding::NearestTiesEven;
    check(prepare_conversion({pcm,normalized,policy,limits,frames},{true,true,true},plan)==NumericStatus::Ok,"bigendian signed16 PCM prepare");
    input.fill(std::byte{0});input[0]=std::byte{0x80};
    check(convert(plan,in,out,scratch).status==NumericStatus::Ok,"bigendian signed16 minimum");std::memcpy(&result,output.data(),8);
    check(result==0xbff0000000000000ULL,"bigendian signed16 exact-1 oracle");
 }
 void unsigned_saturation(){
    policy.scaling.mode=ScaleMode::PcmDenormalize;policy.precision_loss=PrecisionLoss::AllowDeclared;policy.overflow=Overflow::Saturate;
    for(const auto type:{DType::UInt8,DType::UInt16,DType::UInt32,DType::UInt64}) {
        pcm.dtype=type;pcm.representation={};pcm.representation.normalization=Normalization::UnsignedUnit;
        const auto bytes=dtype_descriptor(type)->bytes;
        check(prepare_conversion({normalized,pcm,policy,limits,frames},{true,true,true},plan)==NumericStatus::Ok,"all unsigned PCM widths saturating prepare");
        for(const auto raw:{UINT64_C(0x7fefffffffffffff),UINT64_C(0xffefffffffffffff)}) {
            std::memcpy(input.data(),&raw,8);output.fill(std::byte{0xa5});
            const auto saturated=convert(plan,reverse,pcm_out,scratch);
            check(saturated.status==NumericStatus::Ok && saturated.loss_flags==2,"huge finite PCM obeys saturation policy both signs");
            for(std::size_t i=0;i<bytes;++i)check(output[i]==(raw>>63?std::byte{0}:std::byte{0xff}),"unsigned PCM exact saturation endpoint bytes");
            for(std::size_t i=bytes;i<output.size();++i)check(output[i]==std::byte{0xa5},"saturation preserves output padding");
        }
        policy.overflow=Overflow::Reject;
        check(prepare_conversion({normalized,pcm,policy,limits,frames},{true,true,true},plan)==NumericStatus::Ok,"unsigned PCM overflow reject prepare");
        for(const auto raw:{UINT64_C(0x7fefffffffffffff),UINT64_C(0xffefffffffffffff)}) {
            std::memcpy(input.data(),&raw,8);output.fill(std::byte{0xa5});const auto before=output;
            const auto rejected=convert(plan,reverse,pcm_out,scratch);
            check(rejected.status==NumericStatus::Overflow && rejected.loss_flags==0 && rejected.used_scratch==0 && output==before,"large unsigned PCM Reject unchanged atomic failure");
        }
        policy.overflow=Overflow::Saturate;
    }
 }
 void tiny_precision(){
    pcm.dtype=DType::UInt8;policy.overflow=Overflow::Reject;policy.precision_loss=PrecisionLoss::RejectInexact;
    std::memcpy(input.data(),&negative_subnormal,8);output.fill(std::byte{0xa5});const auto tiny_before=output;
    check(prepare_conversion({normalized,pcm,policy,limits,frames},{true,true,true},plan)==NumericStatus::Ok,"tiny biased PCM strict precision prepare");
    check(convert(plan,reverse,pcm_out,scratch).status==NumericStatus::PrecisionLoss && output==tiny_before,"tiny negative unsigned PCM still rejects inexact atomically");
 }
};
}
int main(){PcmConversionScenario scenario;scenario.packed24();scenario.unsigned_bias();scenario.signed16();scenario.unsigned_saturation();scenario.tiny_precision();    std::cout<<"PCM exact conversion oracles passed\n";
}
