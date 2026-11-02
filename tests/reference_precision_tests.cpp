#include <dsp_kernels/detail/prepared_reference_fourier.h>
#include <dsp_kernels/spectral_frame_recipe.h>
#include <dsp_kernels/tempogram_sequence.h>
#include <dsp_kernels/sample_rotation.h>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <limits>
#include <new>
#include <algorithm>
#include <bit>
#include <cstdint>

namespace {
std::size_t allocations=0;
void check(bool ok,const char* message) {if(!ok){std::fprintf(stderr,"%s\n",message);std::abort();}}
template<std::size_t N> std::array<double,N> signal() {
    std::array<double,N> values{};
    for(std::size_t i=0;i<N;++i)
        for(std::size_t tone=0;tone<8;++tone)values[i]+=.03*std::sin((.023+.013*double(tone))*double(i)+.00001*double(tone+1)*double(i*i));
    return values;
}
void odd_transform_and_clone() {
    const auto input=signal<257>();std::array<double,258> transformed{};std::array<double,257> result{};
    dsp_kernels::detail::PreparedReferenceFourier plan;std::size_t bound=0;
    check(plan.prepare(257) && decltype(plan)::storage_bytes(257,bound) && plan.workspace_bytes()<=bound,"checked prepared Double Fourier storage");
    auto clone=plan;const auto before=allocations;
    check(plan.forward(input,transformed) && clone.inverse(transformed,result),"odd prime Fourier roundtrip across independent clone workspace");
    check(allocations==before,"processing recreated a plan or C++ workspace");
    for(std::size_t i=0;i<input.size();++i)check(std::abs(result[i]-input[i])<1e-11,"Double Fourier packing/normalization");
    check(!decltype(plan)::storage_bytes(65537,bound) && !decltype(plan)::storage_bytes(SIZE_MAX,bound),"bounded reference transform geometry");
}
void reference_recipe_lifecycle() {
    dsp_kernels::SpectralFrameRecipeConfig c;c.precision=dsp_kernels::NumericalPrecision::DesktopReference;
    c.sample_rate=8000;c.framing.length=64;c.framing.hop_samples=17;c.periodic=true;c.include_nyquist=true;
    const auto data=signal<257>();std::array<float,257> input{};std::array<float,33> output{};
    for(std::size_t i=0;i<input.size();++i)input[i]=float(data[i]);
    dsp_kernels::PreparedSpectralFrameRecipe plan;std::size_t bound=0;
    check(plan.prepare(input.size(),c) && decltype(plan)::storage_bytes(input.size(),c,{},{},bound) && plan.workspace_bytes()<=bound,"checked recipe Double owned storage");
    auto clone=plan;const auto before=allocations;
    check(plan.process_frame(input,0,output) && clone.process_frame(input,1,output),"independent reference recipe clone");
    check(allocations==before,"reference recipe hot C++ allocation");
    input[0]=std::numeric_limits<float>::quiet_NaN();
    check(!plan.process_frame(input,0,output) && plan.values().complex.empty(),"failed reference frame invalidates stage publication");
}
void hybrid_storage_and_publication() {
    dsp_kernels::TempogramSequenceConfig c;c.method=dsp_kernels::TempogramMethod::Hybrid;
    c.frame_rate=100;c.autocorrelation.window.length=96;c.autocorrelation.window.precision=dsp_kernels::NumericalPrecision::DesktopReference;
    dsp_kernels::PreparedTempogramSequence plan;std::size_t bound=0;
    check(plan.prepare(c) && decltype(plan)::storage_bytes(c,{},bound) && plan.workspace_bytes()<=bound,"checked hybrid Double storage");
    std::array<float,173> input{};std::array<float,49> output{};
    const auto data=signal<173>();for(std::size_t i=0;i<input.size();++i)input[i]=float(std::abs(data[i]));
    const auto before=allocations;check(plan.process_frame(input,42,output),"reference hybrid evaluation");
    check(allocations==before && plan.view().values.size()==output.size(),"reference hybrid prepared processing/publication");
    check(!plan.process_frame(input,1000,output) && plan.view().values.empty(),"reference hybrid failure clears views");
}
void acf_publication_range() {
    auto c=dsp_kernels::real_linear_autocorrelation_config();c.precision=dsp_kernels::NumericalPrecision::DesktopReference;
    dsp_kernels::PreparedSpectralAutocorrelation plan;check(plan.prepare(64,c),"prepare publication-range ACF");
    std::array<float,64> input{},output{};input.fill(.25f);
    check(plan.process(input,output),"normal ACF publication before overflow");
    input.fill(1e30f);output.fill(37.f);const auto unchanged=output;
    check(!plan.process(input,output) && output==unchanged && plan.view().values.empty(),"ACF F32 overflow is atomic and invalidates views");
    std::array<double,64> promoted{},double_output{};promoted.fill(double(1e30f));double_output.fill(37.);
    const auto double_unchanged=double_output;
    check(!plan.process_double(promoted,double_output) && double_output==double_unchanged && plan.view().values.empty(),"Double ACF cannot publish unrepresentable F32 diagnostics");
    dsp_kernels::AutocorrelationTempogramConfig t;t.window.length=64;t.window.center=false;
    t.window.precision=dsp_kernels::NumericalPrecision::DesktopReference;t.normalization=dsp_kernels::VectorNormalization::None;
    dsp_kernels::PreparedAutocorrelationTempogram temporal;check(temporal.prepare(t),"prepare raw reference ACF tempogram");
    check(!temporal.process_frame(input,0,output) && output==unchanged && temporal.view().normalized_values.empty(),"normalization None does not publish overflow");
}
void window_publication_range() {
    dsp_kernels::TemporalWindowConfig c;c.length=64;c.center=false;c.precision=dsp_kernels::NumericalPrecision::DesktopReference;
    std::array<double,64> coefficients{};coefficients.fill(.25);
    dsp_kernels::PreparedTemporalWindow window;
    check(window.prepare(c,{},coefficients) && window.uses_explicit_coefficients() && window.coefficients()[0]==.25f,"Double explicit window is marked explicit");
    coefficients.fill(2.*double(std::numeric_limits<float>::max()));
    check(!window.prepare(c,{},coefficients) && window.coefficients()[0]==.25f,"unrepresentable Double coefficients rejected before publication");
    check(std::all_of(window.coefficients().begin(),window.coefficients().end(),[](float x){return std::isfinite(x);}),"window retains finite accepted coefficients");
}
void fourier_publication_range() {
    dsp_kernels::TemporalWindowConfig c;c.length=64;c.center=false;c.precision=dsp_kernels::NumericalPrecision::DesktopReference;
    dsp_kernels::PreparedFourierTempogram plan;check(plan.prepare(c),"prepare Fourier publication boundary");
    std::array<float,64> input{},coefficients{};std::array<float,66> output{};
    check(plan.process_frame(input,0,output),"valid Fourier publication before overflow");
    input.fill(1e38f);output.fill(37.f);const auto unchanged=output;
    check(!plan.process_frame(input,0,output) && output==unchanged && plan.view().complex_values.empty(),"FFT Double result checked before F32 cast/output");
    coefficients.fill(1e30f);input.fill(1e30f);check(plan.prepare(c,coefficients),"finite explicit F32 window accepted");
    check(!plan.process_frame(input,0,output) && output==unchanged && plan.view().windowed_input.empty(),"Double prewindow product checked before F32 diagnostics");
}
void source_magnitude_projection() {
    // Actual NumPy Complex64 abs outputs, numpy@a3d0ef1d, from the pinned
    // librosa PCM-strength stages; these distinguish scaled FMA from hypot.
    constexpr std::array<std::array<std::uint32_t,3>,6> probes{{
        {0xbf3c41d3,0xbf92fa66,0x3fae892b},{0xbe97993f,0xbf34f496,0x3f44308f},
        {0x3de46d35,0x3ef0a680,0x3ef75554},{0x3d788520,0x3ec638a0,0x3ec8a403},
        {0x3cf53f4c,0x3ea8cba3,0x3ea97d6e},{0x3c21bc0d,0x3e9307e4,0x3e931e20}}};
    dsp_kernels::SpectrumProjectionOptions c;c.representation=dsp_kernels::SpectrumProjection::Magnitude;
    c.magnitude_norm=dsp_kernels::ComplexMagnitudeNorm::ScaledRatioFma;
    dsp_kernels::PreparedSpectrumScaling raw,calibrated;
    check(raw.prepare_raw(c) && calibrated.prepare(8000,.5,1,c),"source norm accepted by raw/calibrated projection");
    const auto before=allocations;
    for(const auto& probe:probes){
        const auto real=std::bit_cast<float>(probe[0]),imaginary=std::bit_cast<float>(probe[1]),expected=std::bit_cast<float>(probe[2]);
        float direct=0,unscaled=0,scaled=0;
        check(dsp_kernels::complex_polar_value(real,imaginary,&direct,nullptr,{dsp_kernels::ZeroMagnitudePhase::Zero,c.magnitude_norm,false}) && direct==expected,"shared magnitude matches actual source bits");
        check(raw.project(real,imaginary,false,unscaled) && unscaled==expected,"raw projection forwards source magnitude norm");
        check(calibrated.project(real,imaginary,false,scaled) && scaled==2*expected,"calibrated projection forwards source magnitude before Double scale");
        check(std::hypot(real,imaginary)!=expected,"probe distinguishes existing hypot behavior");
    }
    check(allocations==before,"source magnitude projections allocate no workspace during processing");
}
void source_magnitude_recipe() {
    const std::array<float,8> input{std::bit_cast<float>(0xbf3c41d3u),std::bit_cast<float>(0xbf92fa66u),
        0,0,3,4,-3,4};
    dsp_kernels::SpectralFrameRecipeConfig c;c.framing.length=8;c.sample_rate=8000;
    dsp_kernels::PreparedSpectralFrameRecipe portable,reference,explicit_norm;
    check(portable.prepare(8,c),"prepare unchanged portable magnitude recipe");
    c.precision=dsp_kernels::NumericalPrecision::DesktopReference;
    check(reference.prepare(8,c),"prepare source magnitude reference recipe");
    c.precision=dsp_kernels::NumericalPrecision::PortableFloat32;c.magnitude_norm=dsp_kernels::ComplexMagnitudeNorm::ScaledRatioFma;
    check(explicit_norm.prepare(8,c),"prepare explicitly selected portable source norm");
    std::array<float,4> default_values{},reference_values{},explicit_values{};const auto before=allocations;
    check(portable.project_frame(input,{{},{},default_values,{},{}}) && reference.project_frame(input,{{},{},reference_values,{},{}}) &&
        explicit_norm.project_frame(input,{{},{},explicit_values,{},{}}),"recipe stage forwards configured magnitude mode");
    check(default_values[0]==std::hypot(input[0],input[1]) && reference_values[0]==std::bit_cast<float>(0x3fae892bu),"default hypot retained; reference uses source Float32 operation order");
    check(reference_values==explicit_values && reference_values[1]==0 && reference_values[2]==5 && reference_values[3]==5,"source norm forwards zero and signed finite bins");
    check(allocations==before,"recipe magnitude forwarding does not allocate");
}
void scalar_embedding_layout() {
    const std::array<double,5> odd{1,2,3,4,5};const std::array<double,4> even{1,2,3,4};
    std::array<double,8> padded{};std::array<double,3> short_buffer{};std::array<double,6> even_buffer{};
    const auto before=allocations;
    check(dsp_kernels::embed_fourier_frame(odd,padded,true) && padded==std::array<double,8>{3,4,5,0,0,0,1,2},"odd zero-phase embedding retains floor/ceil halves");
    check(dsp_kernels::embed_fourier_frame(odd,short_buffer,true) && short_buffer==std::array<double,3>{3,1,2},"short FFT keeps source ordered half overwrite");
    check(dsp_kernels::embed_fourier_frame(even,even_buffer,true) && even_buffer==std::array<double,6>{3,4,0,0,1,2},"even zero-phase embedding pads between halves");
    check(dsp_kernels::rotate_samples(padded,padded,-2) && padded==std::array<double,8>{1,2,3,4,5,0,0,0},"Double in-place negative rotation uses explicit FFT buffer");
    std::array<double,5> in_place=odd;
    check(dsp_kernels::embed_fourier_frame(std::span<const double>(in_place),std::span(in_place),true) && in_place==std::array<double,5>{3,4,5,1,2},"Double embedding supports exact in-place frame");
    check(!dsp_kernels::embed_fourier_frame(odd,std::span(padded).first(2),true),"each zero-phase half must fit");
    check(!dsp_kernels::rotate_samples(std::span<const double>(padded).first(4),std::span(padded).subspan(1,4),1),"Double raw rotation rejects partial overlap");
    check(!dsp_kernels::embed_fourier_frame(std::span<const double>(padded).first(4),std::span(padded).subspan(1,4)),"Double embedding rejects partial overlap");
    std::array<float,2> raw{std::numeric_limits<float>::quiet_NaN(),1};
    check(dsp_kernels::rotate_samples(raw,raw,-1) && raw[0]==1 && std::isnan(raw[1]),"existing raw Float32 rotation retains nonfinite values");
    check(allocations==before,"typed embedding and rotations need no dynamic scratch");
}
void reference_embedding_case(std::size_t length,std::span<const double> expected,bool shifted,std::ptrdiff_t rotation) {
    dsp_kernels::TemporalWindowConfig c;c.length=length;c.center=false;c.precision=dsp_kernels::NumericalPrecision::DesktopReference;
    const std::array<float,5> data{1,2,3,4,5};std::array<float,5> coefficients{};coefficients.fill(1);
    dsp_kernels::PreparedFourierTempogram plan;dsp_kernels::detail::PreparedReferenceFourier oracle;std::size_t bound=0;
    check(decltype(plan)::storage_bytes(c,std::span(coefficients).first(length),expected.size(),shifted,bound) &&
        plan.prepare_size(c,std::span(coefficients).first(length),expected.size(),shifted) && plan.workspace_bytes()<=bound,"reference shifted geometry retains checked existing storage bound");
    check(oracle.prepare(expected.size()),"prepare independent reference transform workspace");
    std::array<float,10> result{};std::array<double,10> transformed{};const auto count=2*plan.bins();const auto before=allocations;
    check(plan.process_at(std::span(data).first(length),0,std::span(result).first(count),rotation) && oracle.forward(expected,std::span(transformed).first(count)),"reference frame embeds/rotates before existing prepared FFT");
    const auto view=plan.view();
    check(view.transform_input.size()==expected.size() && view.windowed_input.size()==length,"reference stage diagnostics preserve explicit input geometry");
    for(std::size_t i=0;i<expected.size();++i)check(view.transform_input[i]==float(expected[i]),"FFT buffer layout matches explicit source coordinates");
    for(std::size_t i=0;i<count;++i)check(result[i]==float(transformed[i]),"embedded reference uses prepared transform without early Float32 rounding");
    check(allocations==before,"reference zero-phase and buffer rotation allocate no hot workspace");
    std::array<float,5> invalid=data;invalid[0]=std::numeric_limits<float>::quiet_NaN();result.fill(37);const auto unchanged=result;
    check(!plan.process_at(std::span(invalid).first(length),0,std::span(result).first(count),rotation) && plan.view().complex_values.empty() && result==unchanged,"failed shifted reference frame invalidates views before output publication");
}
void reference_embedding_geometry() {
    const std::array<double,8> odd_padded{3,4,5,0,0,0,1,2},even_rotated{2,3,4,0,0,0,0,1};
    const std::array<double,3> odd_short{3,1,2};const std::array<double,4> ordinary_short{1,2,3,4};
    reference_embedding_case(5,odd_padded,true,0);reference_embedding_case(4,even_rotated,true,-1);
    reference_embedding_case(5,odd_short,true,0);reference_embedding_case(5,ordinary_short,false,0);
    dsp_kernels::TemporalWindowConfig c;c.length=5;c.precision=dsp_kernels::NumericalPrecision::DesktopReference;
    std::size_t bytes=0;check(!dsp_kernels::PreparedFourierTempogram::storage_bytes(c,{},2,true,bytes),"reference rejects short zero-phase FFT before preparing resources");
}
}
void* operator new(std::size_t size) {++allocations;if(auto* p=std::malloc(size?size:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t size) {return ::operator new(size);}
void operator delete(void* value) noexcept {std::free(value);}
void operator delete[](void* value) noexcept {std::free(value);}
void operator delete(void* value,std::size_t) noexcept {std::free(value);}
void operator delete[](void* value,std::size_t) noexcept {std::free(value);}
int main() {
    odd_transform_and_clone();reference_recipe_lifecycle();hybrid_storage_and_publication();
    acf_publication_range();window_publication_range();fourier_publication_range();
    source_magnitude_projection();source_magnitude_recipe();
    scalar_embedding_layout();reference_embedding_geometry();
}
