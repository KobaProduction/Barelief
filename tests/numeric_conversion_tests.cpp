#include "../libs/dsp_kernels/src/numeric_arithmetic.h"
#include <dsp_kernels/numeric_convert.h>
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

namespace {
void require(bool condition,const char* message) {
    if(!condition){std::cerr<<message<<'\n';std::exit(3);}
}
void public_converter_oracles() {
    using namespace signal_contracts;using namespace dsp_kernels;
    const std::array<std::uint32_t,1> channels{0};
    NumericContract input;input.dtype=DType::UInt64;input.semantic=Semantic::Scalar;input.channel_order=channels;
    auto output=input;output.dtype=DType::Float64;
    const KernelLimits limits{256,4,4096,4096,4096};
    const FrameLimits frames{4,256,4096,4096,256,4096};
    ConversionRequest request{input,output,{},limits,frames};
    PreparedConversion plan;
    require(prepare_conversion(request,{true,true,true},plan)==NumericStatus::Ok,"public Converter prepares exact64 source contract");
    const std::uint64_t source_value=9007199254740993ULL;
    std::array<std::byte,8> source{},destination{};std::memcpy(source.data(),&source_value,8);destination.fill(std::byte{0xa5});
    const auto before=destination;
    const std::array<PlaneLayout,1> planes{{{0,{0,0}}}};
    const NumericView in{&input,source,planes,{1,1},{},{}};
    MutableNumericView out{&output,destination,planes,{1,1},{},{}};
    std::array<std::byte,4096> scratch{};
    const auto rejected=convert(plan,in,out,scratch);
    require(rejected.status==NumericStatus::PrecisionLoss && destination==before,"public UInt64→Float64 loss rejected atomically");
    request.policy.precision_loss=PrecisionLoss::AllowDeclared;
    require(prepare_conversion(request,{true,true,true},plan)==NumericStatus::Ok,"public explicitly allowed rounded converter prepares");
    require(convert(plan,in,out,scratch).status==NumericStatus::Ok,"public UInt64→Float64 explicit rounded conversion");
    std::uint64_t bits=0;std::memcpy(&bits,destination.data(),8);
    require(bits==0x4340000000000000ULL,"public exact UInt64 input ties-even Float64 oracle");
}
struct Fixture {
    using DType=signal_contracts::DType;
    std::array<std::uint32_t,1> channels{0};
    signal_contracts::NumericContract input,output;
    signal_contracts::ConversionPolicy policy;
    std::array<std::byte,64> source{},destination{},scratch{};
    std::array<signal_contracts::PlaneLayout,1> source_planes{{{0,{8,0}}}},output_planes{{{0,{8,0}}}};
    dsp_kernels::PreparedConversion plan;
    Fixture(DType from,DType to) {
        input.dtype=from;input.channel_order=channels;output=input;output.dtype=to;
        destination.fill(std::byte{0xa5});
    }
    dsp_kernels::NumericStatus prepare() {
        return dsp_kernels::prepare_conversion({input,output,policy,{8,1,64,64,64},{1,8,64,1024,256,64}},{true,true,true},plan);
    }
    dsp_kernels::NumericResult run(std::array<std::size_t,2> actual={1,1}) {
        const signal_contracts::NumericView in{&input,source,source_planes,actual,{},{}};
        signal_contracts::MutableNumericView out{&output,destination,output_planes,actual,{},{}};
        return dsp_kernels::convert(plan,in,out,scratch);
    }
    void source_bits(std::uint64_t bits,std::size_t offset=0) {std::memcpy(source.data()+offset,&bits,8);}
    std::uint64_t output_bits(std::size_t bytes=8,std::size_t offset=0) const {
        std::uint64_t value=0;std::memcpy(&value,destination.data()+offset,bytes);return value;
    }
};
void conversion_edges() {
    using namespace signal_contracts;using namespace dsp_kernels;
    require(declared_ieee64_environment_supported()==numeric_detail::declared_ieee64_environment_available,
            "public native arithmetic query matches actual compiled library capability");
    std::size_t converters=0,reducers=0;
    for(const auto& row:kernel_support_table()) {
        converters+=row.kernel==KernelKind::Converter;reducers+=row.kernel==KernelKind::Reduction;
        if(row.kernel==KernelKind::FFT || row.kernel==KernelKind::Mel || row.kernel==KernelKind::StereoProcessor)
            require(row.dtype==DType::Float32 && !row.optimized,"backend inventory never implies Float64 FFT/ESP acceleration");
    }
    require(converters==10 && reducers==10,"source dtype inventory has all ten fixed types");
    for(const auto& from:dtype_descriptors())for(const auto& to:dtype_descriptors()) {
        Fixture f(from.type,to.type);
        f.source_bits(from.ieee?from.type==DType::Float32?0x3f800000:0x3ff0000000000000ULL:1);
        require(f.prepare()==NumericStatus::Ok,"all100 dtype pairs prepare for identity");
        require(f.run().status==NumericStatus::Ok,"all100 dtype pairs convert exact one");
        const auto expected=to.ieee?to.type==DType::Float32?0x3f800000:0x3ff0000000000000ULL:1;
        require(f.output_bits(to.bytes)==expected,"all100 dtype pairs exact one independent bit oracle");
    }
    Fixture f(DType::Float64,DType::UInt64);f.source_bits(0x43f0000000000000ULL);
    require(f.prepare()==NumericStatus::Ok && f.run().status==NumericStatus::Overflow,"Float64 2^64 rejects UInt64 bound before cast");
    require(f.output_bits()==0xa5a5a5a5a5a5a5a5ULL,"overflow atomic output sentinel");
    f.source_bits(0x43efffffffffffffULL);
    require(f.run().status==NumericStatus::Ok && f.output_bits()==0xfffffffffffff800ULL,"largest Float64 below2^64 exact UInt64");
    Fixture s(DType::Float64,DType::Int64);s.source_bits(0xc3e0000000000000ULL);
    require(s.prepare()==NumericStatus::Ok && s.run().status==NumericStatus::Ok && s.output_bits()==0x8000000000000000ULL,"Int64min exact negative boundary");
    s.source_bits(0x43e0000000000000ULL);require(s.run().status==NumericStatus::Overflow,"positive2^63 rejects Int64");
    Fixture ties(DType::Float64,DType::Int32);ties.source_bits(0x4004000000000000ULL); //2.5
    require(ties.prepare()==NumericStatus::Ok && ties.run().status==NumericStatus::PrecisionLoss,"rounding alone never permits inexact");
    ties.policy.precision_loss=PrecisionLoss::AllowDeclared;require(ties.prepare()==NumericStatus::Ok,"explicit loss policy prepare");
    for(const auto mode:{FE_TONEAREST,FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO}) {
        require(std::fesetround(mode)==0,"host fenv selectable");
        require(ties.run().status==NumericStatus::Ok && ties.output_bits(4)==2,"ties-even unaffected by ambient fenv");
    }
    std::fesetround(FE_TONEAREST);
    ties.source_bits(0x400c000000000000ULL);require(ties.run().status==NumericStatus::Ok && ties.output_bits(4)==4,"odd3.5 tiesup4");
    ties.source_bits(0xc004000000000000ULL);require(ties.run().status==NumericStatus::Ok && ties.output_bits(4)==0xfffffffeULL,"negative2.5 ties-even-2");
    Fixture narrow(DType::Float64,DType::Float32);narrow.source_bits(0x36a0000000000000ULL);
    require(narrow.prepare()==NumericStatus::Ok && narrow.run().status==NumericStatus::Ok && narrow.output_bits(4)==1,"Float64 2^-149 exact Float32 minsubnormal");
    narrow.source_bits(0x3690000000000000ULL);require(narrow.run().status==NumericStatus::PrecisionLoss,"halfsubnormal rejected");
    narrow.policy.precision_loss=PrecisionLoss::AllowDeclared;require(narrow.prepare()==NumericStatus::Ok,"subnormal allow prepare");
    const auto underflow=narrow.run();require(underflow.status==NumericStatus::Ok && narrow.output_bits(4)==0 && (underflow.loss_flags&std::uint32_t(LossFlag::Underflow)),"underflow flagged atomically");
    Fixture special(DType::Float64,DType::Float64);special.source_bits(0x7ff0000000000001ULL);
    require(special.prepare()==NumericStatus::Ok && special.run().status==NumericStatus::NonFiniteInput,"signaling NaN rejected by bit classification");
    special.policy.nonfinite=NonFinite::ReplaceFinite;special.policy.precision_loss=PrecisionLoss::AllowDeclared;
    special.policy.nan_replacement=std::uint64_t{9007199254740993ULL};
    require(special.prepare()==NumericStatus::Ok && special.run().status==NumericStatus::Ok && special.output_bits()==0x4340000000000000ULL,"exact tagged64 NaN replacement uses explicit rounding");
    Fixture affine(DType::UInt64,DType::UInt64);affine.policy.scaling.mode=ScaleMode::IntegerAffine;
    affine.policy.scaling.gain.numerator=std::uint64_t{1};affine.policy.scaling.offset.numerator=std::int64_t{-1};
    affine.source_bits(UINT64_MAX);require(affine.prepare()==NumericStatus::Ok && affine.run().status==NumericStatus::Ok && affine.output_bits()==UINT64_MAX-1,"affine exact UInt64 max-1 no double trap");
    Fixture late(DType::UInt64,DType::UInt8);late.input.semantic=Semantic::Numeric;late.input.shape.rank=Rank::Vector;
    late.input.shape.axes={Axis::Declared,Axis::None};late.input.shape.extents[0]={3,3};late.output=late.input;late.output.dtype=DType::UInt8;
    late.source_bits(1);late.source_bits(2,8);late.source_bits(256,16);
    const auto before=late.destination;
    require(late.prepare()==NumericStatus::Ok,"vector bounded staging prepare");
    const auto failed=late.run({3,1});require(failed.status==NumericStatus::Overflow && failed.error_plane==0 && failed.error_element==2 && late.destination==before,"late element failure never partially commits");
    Fixture copied(DType::UInt64,DType::UInt64);copied.input.clock.domain="owned";copied.output.clock.domain="owned";copied.source_bits(UINT64_MAX);
    require(copied.prepare()==NumericStatus::Ok,"own snapshot prepares");
    auto moved=copied.plan;copied.plan={};copied.plan=moved;
    require(copied.run().status==NumericStatus::Ok && copied.output_bits()==UINT64_MAX,"prepared copies rebase owned metadata");
    Fixture declared(DType::Float64,DType::Float64);declared.source_bits(0x3ff0000000000001ULL);
    declared.policy.precision_loss=PrecisionLoss::AllowDeclared;declared.policy.scaling.mode=ScaleMode::Binary64Affine;
    declared.policy.scaling.gain.numerator=std::bit_cast<double>(UINT64_C(0x3feffffffffffffe));
    declared.policy.scaling.offset.numerator=-1.0;
    require(declared.prepare()==NumericStatus::Ok,"declared separate IEEE64 affine prepares");
    for(const auto mode:{FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO}) {
        std::fesetround(mode);std::feraiseexcept(FE_INVALID);
        const auto computed=declared.run();
        require(computed.status==NumericStatus::Ok && declared.output_bits()==0,"binary64 affine separate multiply/add no FMA");
        require(std::fegetround()==mode && std::fetestexcept(FE_INVALID),"binary64 affine restores ambient rounding and flags");
    }
    std::fesetround(FE_TONEAREST);std::feclearexcept(FE_ALL_EXCEPT);
    Fixture inactive(DType::UInt64,DType::UInt64);inactive.source_bits(UINT64_MAX);
    inactive.policy.scaling.gain.numerator=std::bit_cast<double>(UINT64_C(0x7ff80000000000ab));
    inactive.policy.scaling.gain.exponent2=32767;inactive.policy.nan_replacement=std::bit_cast<double>(UINT64_C(0x7ff0000000000001));
    require(inactive.prepare()==NumericStatus::Ok && inactive.run().status==NumericStatus::Ok && inactive.output_bits()==UINT64_MAX,"inactive exact policy fields never activate semantics");
    require(std::bit_cast<std::uint64_t>(std::get<double>(inactive.plan.policy.scaling.gain.numerator))==0x7ff80000000000abULL && inactive.plan.policy.scaling.gain.exponent2==32767,"inactive literal payload exactly preserved in prepared replay");
    Fixture native_tiny(DType::Float64,DType::Float64);native_tiny.source_bits(1);
    native_tiny.policy.precision_loss=PrecisionLoss::AllowDeclared;native_tiny.policy.scaling.mode=ScaleMode::Binary64Affine;
    native_tiny.policy.scaling.gain.numerator=0.5;native_tiny.policy.scaling.offset.numerator=0.0;
    require(native_tiny.prepare()==NumericStatus::Ok,"native affine half minimumsubnormal prepare");
    for(const auto mode:{FE_TONEAREST,FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO}) {
        std::fesetround(mode);std::feclearexcept(FE_ALL_EXCEPT);std::feraiseexcept(FE_INVALID|FE_DIVBYZERO);
        const int ambient_flags=std::fetestexcept(FE_ALL_EXCEPT);
        const auto tiny=native_tiny.run();
        require(tiny.status==NumericStatus::Ok && native_tiny.output_bits()==0 && tiny.loss_flags==9,"native multiply underflow captured before environment restores");
        require(std::fegetround()==mode && std::fetestexcept(FE_ALL_EXCEPT)==ambient_flags,"native affine underflow preserves exact ambient flags");
        native_tiny.policy.scaling.gain.numerator=1.0;
        require(native_tiny.prepare()==NumericStatus::Ok,"native exact minimumsubnormal prepare");
        const auto exact_subnormal=native_tiny.run();
        require(exact_subnormal.status==NumericStatus::Ok && native_tiny.output_bits()==1 && exact_subnormal.loss_flags==0,"native exact minimumsubnormal has no numerical loss");
        require(std::fegetround()==mode && std::fetestexcept(FE_ALL_EXCEPT)==ambient_flags,"native exact subnormal does not leak denormal flags");
        native_tiny.policy.scaling.gain.numerator=0.5;
        require(native_tiny.prepare()==NumericStatus::Ok,"restore native half-subnormal plan");
    }
    native_tiny.source_bits(0x3ff0000000000000ULL);
    native_tiny.policy.scaling.gain.numerator=std::bit_cast<double>(UINT64_C(1));native_tiny.policy.scaling.gain.exponent2=-1;
    require(native_tiny.prepare()==NumericStatus::Ok,"software prepacking of tiny affine gain prepare");
    require(native_tiny.run().loss_flags==9 && native_tiny.output_bits()==0,"software coefficient underflow survives native exactzero multiply");
    native_tiny.policy.scaling.gain.numerator=1.0;native_tiny.policy.scaling.gain.exponent2=0;
    native_tiny.policy.scaling.offset.numerator=std::bit_cast<double>(UINT64_C(1));native_tiny.policy.scaling.offset.exponent2=-1;
    require(native_tiny.prepare()==NumericStatus::Ok,"software tiny affine offset prepare");
    require(native_tiny.run().loss_flags==9 && native_tiny.output_bits()==0x3ff0000000000000ULL,"lost tiny offset flags retained despite normal final result");
    std::fesetround(FE_TONEAREST);std::feclearexcept(FE_ALL_EXCEPT);
    watch_allocations=true;
    for(unsigned i=0;i<32;++i)require(inactive.run().status==NumericStatus::Ok,"prepared converter steady-state run");
    watch_allocations=false;require(allocations==0,"no C++ heap allocation after prepare");
    NumericView alias_in{&inactive.input,inactive.source,inactive.source_planes,{1,1},{},{}};
    MutableNumericView alias_out{&inactive.output,inactive.source,inactive.output_planes,{1,1},{},{}};
    require(convert(inactive.plan,alias_in,alias_out,inactive.scratch).status==NumericStatus::Ok,"input destination alias allowed via disjoint transaction staging");
    const auto alias_before=inactive.source;
    require(convert(inactive.plan,alias_in,alias_out,inactive.source).status==NumericStatus::InvalidLayout && inactive.source==alias_before,"scratch alias rejected before touching source/destination");
    require(convert(inactive.plan,alias_in,alias_out,std::span(inactive.scratch).first(7)).status==NumericStatus::BufferTooSmall && inactive.source==alias_before,"insufficient scratch refused atomically");
    Fixture clamped(DType::Float64,DType::UInt64);clamped.source_bits(0x7ff0000000000000ULL);
    clamped.policy.precision_loss=PrecisionLoss::AllowDeclared;clamped.policy.nonfinite=NonFinite::ClampFinite;
    clamped.policy.negative_inf_replacement=std::uint64_t{0};clamped.policy.positive_inf_replacement=UINT64_MAX;
    require(clamped.prepare()==NumericStatus::Ok && clamped.run().status==NumericStatus::Ok && clamped.output_bits()==UINT64_MAX,"declared exactUInt64 clamp endpoint preserved");
    clamped.policy.negative_inf_replacement=UINT64_MAX;clamped.policy.positive_inf_replacement=std::uint64_t{0};
    require(clamped.prepare()==NumericStatus::InvalidPolicy,"reversed explicit clamp endpoints rejected at prepare");
    Fixture bad_replacement(DType::Float64,DType::UInt8);bad_replacement.policy.precision_loss=PrecisionLoss::AllowDeclared;
    bad_replacement.policy.nonfinite=NonFinite::ReplaceFinite;bad_replacement.policy.nan_replacement=std::uint64_t{256};
    require(bad_replacement.prepare()==NumericStatus::InvalidPolicy,"active replacement outside destination bounds rejected before publication");
    struct Endpoint {DType type;std::uint64_t low,high;std::size_t bytes;};
    for(const auto endpoint:std::array<Endpoint,8>{{
        {DType::Int8,0x80,0x7f,1},{DType::UInt8,0,0xff,1},
        {DType::Int16,0x8000,0x7fff,2},{DType::UInt16,0,0xffff,2},
        {DType::Int32,0x80000000,0x7fffffff,4},{DType::UInt32,0,0xffffffff,4},
        {DType::Int64,0x8000000000000000ULL,0x7fffffffffffffffULL,8},{DType::UInt64,0,UINT64_MAX,8}}}) {
        Fixture exact(endpoint.type,endpoint.type);
        require(exact.prepare()==NumericStatus::Ok,"integer endpoints identity prepare");
        for(const auto value:{endpoint.low,endpoint.high}) {
            exact.source_bits(value);require(exact.run().status==NumericStatus::Ok && exact.output_bits(endpoint.bytes)==value,"integer minmax never passed through float");
        }
    }
    Fixture dyadic(DType::UInt64,DType::Float64);dyadic.source_bits(9007199254740993ULL);
    dyadic.policy.scaling.mode=ScaleMode::DyadicAffine;dyadic.policy.scaling.gain.exponent2=-1;
    dyadic.policy.scaling.offset.numerator=std::int64_t{-1};dyadic.policy.scaling.offset.exponent2=-1;
    require(dyadic.prepare()==NumericStatus::Ok && dyadic.run().status==NumericStatus::Ok && dyadic.output_bits()==0x4330000000000000ULL,"exact dyadic64 cancellation before narrowing");
    dyadic.policy.scaling.gain.exponent2=-64;require(dyadic.prepare()==NumericStatus::UnsupportedCombination,"active dyadic unsupported exponent rejected at prepare");
    const std::array<std::uint32_t,2> matrix_channels{9,3};
    NumericContract matrix;matrix.dtype=DType::UInt16;matrix.semantic=Semantic::Numeric;matrix.channel_order=matrix_channels;
    matrix.shape.rank=Rank::Matrix;matrix.shape.axes={Axis::Declared,Axis::Declared};matrix.shape.extents={ExtentBounds{2,2},ExtentBounds{2,2}};
    auto matrix_output=matrix;matrix_output.dtype=DType::Float32;matrix_output.representation.layout=ChannelLayout::Interleaved;
    std::array<std::byte,64> matrix_source{},matrix_destination{},matrix_scratch{};matrix_destination.fill(std::byte{0xa5});
    const std::array<PlaneLayout,2> matrix_in_planes{{{0,{6,2}},{16,{6,2}}}},matrix_out_planes{{{0,{16,8}},{4,{16,8}}}};
    for(std::size_t plane=0;plane<2;++plane)for(std::size_t row=0;row<2;++row)for(std::size_t col=0;col<2;++col) {
        const auto value=std::uint16_t(plane*4+row*2+col);
        std::memcpy(matrix_source.data()+matrix_in_planes[plane].offset+row*6+col*2,&value,2);
    }
    PreparedConversion matrix_plan;
    require(prepare_conversion({matrix,matrix_output,{}, {8,2,64,64,64},{2,8,64,1024,256,64}},{true,true,true},matrix_plan)==NumericStatus::Ok,"rank2 two-plane strided conversion prepare");
    NumericView matrix_in{&matrix,matrix_source,matrix_in_planes,{2,2},{},{}};
    MutableNumericView matrix_out{&matrix_output,matrix_destination,matrix_out_planes,{2,2},{},{}};
    require(convert(matrix_plan,matrix_in,matrix_out,matrix_scratch).status==NumericStatus::Ok,"rank2 explicit interleaved output conversion");
    const std::array<std::uint32_t,8> matrix_bits{0,0x40800000,0x3f800000,0x40a00000,0x40000000,0x40c00000,0x40400000,0x40e00000};
    for(std::size_t i=0;i<8;++i){std::uint32_t result;std::memcpy(&result,matrix_destination.data()+i*4,4);require(result==matrix_bits[i],"matrix axes and declared channelorder preserved by scatter");}
    require(matrix_destination[32]==std::byte{0xa5},"unused output padding unchanged");
    auto refused=matrix_plan;const auto function_before=refused.function;
    auto too_small_limits=KernelLimits{7,2,64,64,64};
    require(prepare_conversion({matrix,matrix_output,{},too_small_limits,{2,8,64,1024,256,64}},{true,true,true},refused)==NumericStatus::LimitExceeded && refused.function==function_before,"Prepare limit refusal never republishes partial plan");
}
}
int main() {
    public_converter_oracles();
    conversion_edges();
    using namespace dsp_kernels::numeric_detail;
    // Independently specified integer/IEEE literals, not computed by the tested packer.
    require(pack_ieee({Magnitude::from(1),0,false},8,23,true,false).bits==0x3f800000,
            "integer1 IEEE32 exact oracle");
    require(pack_ieee({Magnitude::from(16777217),0,false},8,23,true,false).error==ArithmeticError::Inexact,
            "2^24+1 must reject Float32 precision loss");
    require(pack_ieee({Magnitude::from(16777217),0,false},8,23,true,true).bits==0x4b800000,
            "2^24+1 ties to even down");
    require(pack_ieee({Magnitude::from(16777219),0,false},8,23,true,true).bits==0x4b800002,
            "2^24+3 ties to even up");
    require(pack_ieee({Magnitude::from(9007199254740993ULL),0,false},11,52,true,false).error==ArithmeticError::Inexact,
            "2^53+1 must reject Float64 loss");
    require(pack_ieee({Magnitude::from(0xffffffffffffffffULL),0,false},11,52,true,true).bits==0x43f0000000000000ULL,
            "UInt64max rounds to2^64, exact integer never went through double");
    require(pack_ieee({Magnitude::from(1),-149,false},8,23,true,false).bits==1,
            "minimum Float32 subnormal exact");
    require(pack_ieee({Magnitude::from(1),-150,false},8,23,true,false).error==ArithmeticError::Inexact,
            "half minimum subnormal rejects loss");
    require(pack_ieee({Magnitude::from(1),-150,false},8,23,true,true).bits==0,
            "half minimum subnormal tie rounds zero");
    require(pack_ieee({Magnitude::from(3),-150,false},8,23,true,true).bits==2,
            "subnormal odd tie rounds even2");
    require(pack_ieee({Magnitude::from(0),0,true},11,52,true,false).bits==0x8000000000000000ULL,
            "negative zero IEEE bits preserved");
    require(pack_ieee({Magnitude::from(1),128,false},8,23,true,true).error==ArithmeticError::Overflow,
            "2^128 rejects Float32 before unsafe cast");
    std::cout<<"numeric conversion exact arithmetic oracles passed\n";
}
