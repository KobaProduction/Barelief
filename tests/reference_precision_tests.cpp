#include <dsp_kernels/detail/prepared_reference_fourier.h>
#include <dsp_kernels/convolution.h>
#include <dsp_kernels/tempo_analysis_sequence.h>
#include <dsp_kernels/filter_projection.h>
#include <dsp_kernels/log_frequency_filter_bank.h>
#include <dsp_kernels/mel_filter_bank.h>
#include <string_view>
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

// Actual NumPy 1.26.4/OpenBLAS c2f4bdbb Haswell raw64 controls, independently
// captured from the existing v65 ODF. Fixture SHA256 ae741277ddd4f5a9bebd09e1d23e6f5207a966880f6f77d44e12ba77b58c311a.
template<std::size_t N> std::array<double,N> reference_raw_values(const std::array<std::uint64_t,N>& bits) {
    std::array<double,N> values{};
    for(std::size_t i=0;i<N;++i)values[i]=std::bit_cast<double>(bits[i]);
    return values;
}
template<std::size_t N> void reference_dot_control(const std::array<std::uint64_t,N>& x,const std::array<std::uint64_t,N>& y,std::uint64_t expected) {
    const auto input=reference_raw_values(x);auto kernel=reference_raw_values(y);std::reverse(kernel.begin(),kernel.end());
    std::array<double,N> output{};const auto before=allocations;
    check(dsp_kernels::convolve_same(input,kernel,output),"reference dot via public convolution");
    check(std::bit_cast<std::uint64_t>(output[N/2])==expected,"reference dot matches independently captured native bits");
    check(allocations==before,"reference dot allocates no processing storage");
}
template<std::size_t N,std::size_t M,std::size_t O> void reference_crop_control(const std::array<std::uint64_t,N>& x,const std::array<std::uint64_t,M>& y,const std::array<std::uint64_t,O>& expected,dsp_kernels::SameConvolutionCrop crop) {
    const auto input=reference_raw_values(x);const auto kernel=reference_raw_values(y);std::array<double,O> output{};
    const auto before=allocations;
    check(dsp_kernels::convolve_same(input,kernel,output,crop),"reference clipped/cropped convolution");
    for(std::size_t i=0;i<O;++i)check(std::bit_cast<std::uint64_t>(output[i])==expected[i],"reference actual-overlap and crop native bits");
    check(allocations==before,"reference crop allocates no processing storage");
}
void reference_native_dot_15(){
    constexpr std::array<std::uint64_t,15> x{0x4005719f60000000ull,0x403372b9e0000000ull,0x4040e683a0000000ull,0x4038b0efe0000000ull,0x40207dafa0000000ull,0x3feef151c0000000ull,0x4005719f60000000ull,0x403372b9e0000000ull,0x4040e683a0000000ull,0x4038b0efe0000000ull,0x40207dafa0000000ull,0x3feef151c0000000ull,0x4005719f60000000ull,0x403372b9e0000000ull,0x4040e683a0000000ull};
    constexpr std::array<std::uint64_t,15> y{0x3fb47ae147ae147cull,0x3fbc6e9c282b38f8ull,0x3fc99b6e81cf3d1eull,0x3fd56ffd8ffcb926ull,0x3fdfd7f772879a47ull,0x3fe54eee020c42feull,0x3fea2699a39fdab8ull,0x3fedcb91fb0bdaadull,0x3fefbfd65226411eull,0x3fefbfd65226411eull,0x3fedcb91fb0bdaadull,0x3fea2699a39fdab8ull,0x3fe54eee020c42feull,0x3fdfd7f772879a47ull,0x3fd56ffd8ffcb926ull};
    reference_dot_control(x,y,0x406077964a06aa61ull);
}
void reference_native_dot_16(){
    constexpr std::array<std::uint64_t,16> x{0x4005719f60000000ull,0x403372b9e0000000ull,0x4040e683a0000000ull,0x4038b0efe0000000ull,0x40207dafa0000000ull,0x3feef151c0000000ull,0x4005719f60000000ull,0x403372b9e0000000ull,0x4040e683a0000000ull,0x4038b0efe0000000ull,0x40207dafa0000000ull,0x3feef151c0000000ull,0x4005719f60000000ull,0x403372b9e0000000ull,0x4040e683a0000000ull,0x4038b0efe0000000ull};
    constexpr std::array<std::uint64_t,16> y{0x3fb47ae147ae147cull,0x3fbc6e9c282b38f8ull,0x3fc99b6e81cf3d1eull,0x3fd56ffd8ffcb926ull,0x3fdfd7f772879a47ull,0x3fe54eee020c42feull,0x3fea2699a39fdab8ull,0x3fedcb91fb0bdaadull,0x3fefbfd65226411eull,0x3fefbfd65226411eull,0x3fedcb91fb0bdaadull,0x3fea2699a39fdab8ull,0x3fe54eee020c42feull,0x3fdfd7f772879a47ull,0x3fd56ffd8ffcb926ull,0x3fc99b6e81cf3d1eull};
    reference_dot_control(x,y,0x406115a797b0e066ull);
}
void reference_native_dot_18(){
    constexpr std::array<std::uint64_t,18> x{0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x4005719f60000000ull,0x403372b9e0000000ull,0x4040e683a0000000ull,0x4038b0efe0000000ull,0x40207dafa0000000ull,0x3feef151c0000000ull,0x0000000000000000ull,0x0000000000000000ull};
    constexpr std::array<std::uint64_t,18> y{0x3fb47ae147ae147cull,0x3fbc6e9c282b38f8ull,0x3fc99b6e81cf3d1eull,0x3fd56ffd8ffcb926ull,0x3fdfd7f772879a47ull,0x3fe54eee020c42feull,0x3fea2699a39fdab8ull,0x3fedcb91fb0bdaadull,0x3fefbfd65226411eull,0x3fefbfd65226411eull,0x3fedcb91fb0bdaadull,0x3fea2699a39fdab8ull,0x3fe54eee020c42feull,0x3fdfd7f772879a47ull,0x3fd56ffd8ffcb926ull,0x3fc99b6e81cf3d1eull,0x3fbc6e9c282b38f8ull,0x3fb47ae147ae147cull};
    reference_dot_control(x,y,0x404c118e8e9b3438ull);
}
void reference_native_dot_31(){
    constexpr std::array<std::uint64_t,31> x{0x4005719f60000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x403372b9e0000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull};
    constexpr std::array<std::uint64_t,31> y{0x3fb47ae147ae147cull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x3fdfd7f772879a47ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull};
    reference_dot_control(x,y,0x4023c830231353d2ull);
}
void reference_native_dot_32(){
    constexpr std::array<std::uint64_t,32> x{0x4005719f60000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x403372b9e0000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull};
    constexpr std::array<std::uint64_t,32> y{0x3fb47ae147ae147cull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x3fdfd7f772879a47ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull};
    reference_dot_control(x,y,0x4023c830231353d3ull);
}
void reference_native_dot_33(){
    constexpr std::array<std::uint64_t,33> x{0x4005719f60000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x403372b9e0000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x4040e683a0000000ull};
    constexpr std::array<std::uint64_t,33> y{0x3fb47ae147ae147cull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x3fdfd7f772879a47ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x3fd56ffd8ffcb926ull};
    reference_dot_control(x,y,0x40353683f5a8e832ull);
}
void reference_native_crop_0(){
    constexpr std::array<std::uint64_t,33> x{0x4005719f60000000ull,0x403372b9e0000000ull,0x4040e683a0000000ull,0x4038b0efe0000000ull,0x40207dafa0000000ull,0x3feef151c0000000ull,0x4005719f60000000ull,0x403372b9e0000000ull,0x4040e683a0000000ull,0x4038b0efe0000000ull,0x40207dafa0000000ull,0x3feef151c0000000ull,0x4005719f60000000ull,0x403372b9e0000000ull,0x4040e683a0000000ull,0x4038b0efe0000000ull,0x40207dafa0000000ull,0x3feef151c0000000ull,0x4005719f60000000ull,0x403372b9e0000000ull,0x4040e683a0000000ull,0x4038b0efe0000000ull,0x40207dafa0000000ull,0x3feef151c0000000ull,0x4005719f60000000ull,0x403372b9e0000000ull,0x4040e683a0000000ull,0x4038b0efe0000000ull,0x40207dafa0000000ull,0x3feef151c0000000ull,0x4005719f60000000ull,0x403372b9e0000000ull,0x4040e683a0000000ull};
    constexpr std::array<std::uint64_t,18> y{0x3fb47ae147ae147cull,0x3fbc6e9c282b38f8ull,0x3fc99b6e81cf3d1eull,0x3fd56ffd8ffcb926ull,0x3fdfd7f772879a47ull,0x3fe54eee020c42feull,0x3fea2699a39fdab8ull,0x3fedcb91fb0bdaadull,0x3fefbfd65226411eull,0x3fefbfd65226411eull,0x3fedcb91fb0bdaadull,0x3fea2699a39fdab8ull,0x3fe54eee020c42feull,0x3fdfd7f772879a47ull,0x3fd56ffd8ffcb926ull,0x3fc99b6e81cf3d1eull,0x3fbc6e9c282b38f8ull,0x3fb47ae147ae147cull};
    constexpr std::array<std::uint64_t,33> expected{0x4052aa23031d7f52ull,0x40568610b13ed51eull,0x4059ed900bcd6a0bull,0x405ce10b7272ec0dull,0x405f3b806d4df1edull,0x406080e009e47351ull,0x406111c2408d8061ull,0x406141630c56a816ull,0x40613939b3edefc0ull,0x4061356f40b1dbd4ull,0x40614d82dba35ca1ull,0x40616e4ef4afb049ull,0x40617476831eb3a1ull,0x406159ee91ed2c35ull,0x40613bb3688ca811ull,0x4061356f40b1dbd4ull,0x40614d82dba35ca1ull,0x40616e4ef4afb049ull,0x40617476831eb3a1ull,0x406159ee91ed2c35ull,0x40613bb3688ca811ull,0x4061356f40b1dbd4ull,0x40614d82dba35ca1ull,0x40616e4ef4afb049ull,0x40617476831eb3a1ull,0x40611ab8f8a57e21ull,0x4060ced6fc0fa58aull,0x406077964a06aa61ull,0x406005c4d16457c6ull,0x405e96973f52fd34ull,0x405c57c22bccd258ull,0x4059892594a815d1ull,0x40565ed0ee663c0bull};
    reference_crop_control(x,y,expected,dsp_kernels::SameConvolutionCrop::Largest);
}
void reference_native_crop_1(){
    constexpr std::array<std::uint64_t,18> x{0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x4005719f60000000ull,0x403372b9e0000000ull,0x4040e683a0000000ull,0x4038b0efe0000000ull,0x40207dafa0000000ull,0x3feef151c0000000ull,0x0000000000000000ull,0x0000000000000000ull};
    constexpr std::array<std::uint64_t,33> y{0x3fb47ae147ae147cull,0x3fbc6e9c282b38f8ull,0x3fc99b6e81cf3d1eull,0x3fd56ffd8ffcb926ull,0x3fdfd7f772879a47ull,0x3fe54eee020c42feull,0x3fea2699a39fdab8ull,0x3fedcb91fb0bdaadull,0x3fefbfd65226411eull,0x3fefbfd65226411eull,0x3fedcb91fb0bdaadull,0x3fea2699a39fdab8ull,0x3fe54eee020c42feull,0x3fdfd7f772879a47ull,0x3fd56ffd8ffcb926ull,0x3fc99b6e81cf3d1eull,0x3fbc6e9c282b38f8ull,0x3fb47ae147ae147cull,0x3fb47ae147ae147cull,0x3fbc6e9c282b38f8ull,0x3fc99b6e81cf3d1eull,0x3fd56ffd8ffcb926ull,0x3fdfd7f772879a47ull,0x3fe54eee020c42feull,0x3fea2699a39fdab8ull,0x3fedcb91fb0bdaadull,0x3fefbfd65226411eull,0x3fefbfd65226411eull,0x3fedcb91fb0bdaadull,0x3fea2699a39fdab8ull,0x3fe54eee020c42feull,0x3fdfd7f772879a47ull,0x3fd56ffd8ffcb926ull};
    constexpr std::array<std::uint64_t,33> expected{0x0000000000000000ull,0x0000000000000000ull,0x3fcb72b7851eb853ull,0x3ffda827a0ee0be3ull,0x401599e3a75f0267ull,0x4025092837d32a05ull,0x4032031402aac448ull,0x403cb754dd358bcaull,0x4044fe39065502acull,0x404c118e8e9b3438ull,0x40515084c8a78f2aull,0x4053e4ebaa446fdfull,0x40556ccb0b22b8faull,0x4055b3363b25891aull,0x4054aeaa8e60e13bull,0x405282582df79456ull,0x404ef2c2fbfb02e7ull,0x4047f96acedcf618ull,0x404109c6b81ceaebull,0x4036274c3bc32e6cull,0x402b69f52ddab57eull,0x40228bf3a0d190fcull,0x4020f835fcc2b51bull,0x402691e0913b6c0cull,0x403216e1a7a086d8ull,0x403cb754dd358bcaull,0x4044fe39065502adull,0x404c118e8e9b3438ull,0x40515084c8a78f2bull,0x4053e4ebaa446fe0ull,0x40556ccb0b22b8faull,0x4055b3363b25891bull,0x4054aeaa8e60e13cull};
    reference_crop_control(x,y,expected,dsp_kernels::SameConvolutionCrop::Largest);
}
void reference_native_crop_2(){
    constexpr std::array<std::uint64_t,18> x{0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x0000000000000000ull,0x4005719f60000000ull,0x403372b9e0000000ull,0x4040e683a0000000ull,0x4038b0efe0000000ull,0x40207dafa0000000ull,0x3feef151c0000000ull,0x0000000000000000ull,0x0000000000000000ull};
    constexpr std::array<std::uint64_t,33> y{0x3fb47ae147ae147cull,0x3fbc6e9c282b38f8ull,0x3fc99b6e81cf3d1eull,0x3fd56ffd8ffcb926ull,0x3fdfd7f772879a47ull,0x3fe54eee020c42feull,0x3fea2699a39fdab8ull,0x3fedcb91fb0bdaadull,0x3fefbfd65226411eull,0x3fefbfd65226411eull,0x3fedcb91fb0bdaadull,0x3fea2699a39fdab8ull,0x3fe54eee020c42feull,0x3fdfd7f772879a47ull,0x3fd56ffd8ffcb926ull,0x3fc99b6e81cf3d1eull,0x3fbc6e9c282b38f8ull,0x3fb47ae147ae147cull,0x3fb47ae147ae147cull,0x3fbc6e9c282b38f8ull,0x3fc99b6e81cf3d1eull,0x3fd56ffd8ffcb926ull,0x3fdfd7f772879a47ull,0x3fe54eee020c42feull,0x3fea2699a39fdab8ull,0x3fedcb91fb0bdaadull,0x3fefbfd65226411eull,0x3fefbfd65226411eull,0x3fedcb91fb0bdaadull,0x3fea2699a39fdab8ull,0x3fe54eee020c42feull,0x3fdfd7f772879a47ull,0x3fd56ffd8ffcb926ull};
    constexpr std::array<std::uint64_t,18> expected{0x4044fe39065502acull,0x404c118e8e9b3438ull,0x40515084c8a78f2aull,0x4053e4ebaa446fdfull,0x40556ccb0b22b8faull,0x4055b3363b25891aull,0x4054aeaa8e60e13bull,0x405282582df79456ull,0x404ef2c2fbfb02e7ull,0x4047f96acedcf618ull,0x404109c6b81ceaebull,0x4036274c3bc32e6cull,0x402b69f52ddab57eull,0x40228bf3a0d190fcull,0x4020f835fcc2b51bull,0x402691e0913b6c0cull,0x403216e1a7a086d8ull,0x403cb754dd358bcaull};
    reference_crop_control(x,y,expected,dsp_kernels::SameConvolutionCrop::Input);
}
void reference_float_convolution_unchanged(){
    const std::array<float,3> input{1,2,3},kernel{.25f,.5f,.25f};std::array<float,3> output{};
    check(dsp_kernels::convolve_same(input,kernel,output) && output==std::array<float,3>{1,2,2},"Float32 convolution remains unchanged");
}
void reference_small_double_convolution(){
    const std::array<double,3> input{1,2,3},kernel{.25,.5,.25};std::array<double,3> output{};
    const auto before=allocations;
    check(dsp_kernels::convolve_same(input,kernel,output) && output==std::array<double,3>{1,2,2},"short Double scalar convolution remains available");
    check(dsp_kernels::convolve_same(input,std::span<const double>{},output) && output==input,"empty Double kernel bypass remains available");
    check(allocations==before,"short Double and bypass allocate no processing storage");
}
bool reference_convolution_admission(){
    std::array<double,16> input{},kernel{},output{};output.fill(17);
    const auto before=allocations;const bool available=dsp_kernels::convolve_same(input,kernel,output);
    check(std::all_of(output.begin(),output.end(),[available](double value){return value==(available?0.:17.);}),"unavailable fused convolution rejects before output writes");
    dsp_kernels::TempoAnalysisSequenceConfig config;config.precision=dsp_kernels::NumericalPrecision::DesktopReference;
    config.histogram.method=dsp_kernels::TempoHistogramEstimator::Comb;std::size_t bytes=17;
    check(dsp_kernels::PreparedTempoAnalysisSequence::storage_bytes(128,config,{},{},{},bytes)==available,"reference tempo cold admission agrees with fused convolution availability");
    check(available || bytes==17,"unavailable reference admission preserves storage output");
    check(allocations==before,"reference fused capability admission allocates no storage");
    return available;
}
void reference_convolution_controls(){
    reference_float_convolution_unchanged();reference_small_double_convolution();reference_native_dot_15();
    if(!reference_convolution_admission())return;
    reference_native_dot_16();reference_native_dot_18();reference_native_dot_31();reference_native_dot_32();reference_native_dot_33();reference_native_crop_0();reference_native_crop_1();reference_native_crop_2();
}

dsp_kernels::FilterProjectionOptions source_projection_options(){
    return {false,false,dsp_kernels::FilterProjectionAccumulation::SequentialFma};
}
void reference_fma_projection_controls(){
    // Independent actual UCRT fmaf raw32 controls, including double-rounding
    // counterexamples at normal, subnormal and overflow boundaries and signs.
    constexpr std::array<std::array<std::uint32_t,4>,6> cases{{
        {0x397ffffeu,0x39800001u,0x3f800001u,0x3f800001u},
        {0xb97ffffeu,0x39800001u,0xbf800001u,0xbf800001u},
        {0x19fffffeu,0x1a000001u,0x00400001u,0x00400001u},
        {0x99fffffeu,0x1a000001u,0x80400001u,0x80400001u},
        {0x58fffffeu,0x59800001u,0x7f7fffffu,0x7f7fffffu},
        {0xd8fffffeu,0x59800001u,0xff7fffffu,0xff7fffffu}}};
    const auto options=source_projection_options();const auto before=allocations;
    for(const auto& row:cases){
        const std::array<float,2> input{std::bit_cast<float>(row[2]),std::bit_cast<float>(row[0])};
        const std::array<float,2> weights{1.f,std::bit_cast<float>(row[1])};float value=0;
        check(dsp_kernels::project_filter_weights(input,weights,value,options) && std::bit_cast<std::uint32_t>(value)==row[3],"sequential fused projection native FMA bits");
    }
    check(allocations==before,"sequential fused projection has no processing allocations");
}
void reference_projection_failures(){
    const std::array<float,1> input{std::numeric_limits<float>::max()},weights{2};float value=17;
    auto options=source_projection_options();
    check(!dsp_kernels::project_filter_weights(input,weights,value,options) && value==17,"fused overflow preserves publication");
    options.square_input=true;
    check(!dsp_kernels::project_filter_weights(input,std::span(weights),value,options) && value==17,"Float32 input square overflow rejected before fused arithmetic");
    options.square_input=false;options.compensated=true;
    check(!dsp_kernels::project_filter_weights(input,weights,value,options) && value==17,"conflicting fused compensation rejected");
    dsp_kernels::MelFilterBank bank;
    check(bank.prepare_matrix(1,1,std::array<float,1>{1}),"prepare original projection before rejection");
    check(!bank.prepare_matrix(1,1,weights,{},options),"invalid projection policy rejects before replacing prepared bank");
    std::array<float,1> output{};
    check(bank.process(std::array<float,1>{3},output) && output[0]==3,"failed policy preparation preserves prior projection");
}
struct ReferenceWeightValue {std::size_t index;std::uint32_t bits;};
// Actual NumPy weight bytes from native_weights_aligned.npy, SHA256 a12d8fd6a0fd43427a81f82440718aa7a88565a1c35f5bff6a2d3a8d4ba654f1.
// Input source columns k1..1024 are mapped back to k0..1023 with zero DC.
// This is a sparse encoding of captured data, not a triangle/normalizer oracle.
constexpr std::array<ReferenceWeightValue,1268> reference_weight_values{{
    {2,0x3f800000u},
    {1027,0x3f800000u},
    {2052,0x3f800000u},
    {3077,0x3f800000u},
    {4102,0x3f800000u},
    {5127,0x3f800000u},
    {6152,0x3f800000u},
    {7177,0x3f800000u},
    {8202,0x3f800000u},
    {9227,0x3f800000u},
    {10252,0x3f800000u},
    {11277,0x3f800000u},
    {12302,0x3f800000u},
    {13327,0x3f800000u},
    {14352,0x3f800000u},
    {15377,0x3f800000u},
    {16402,0x3f800000u},
    {17427,0x3f800000u},
    {18452,0x3f800000u},
    {19477,0x3f800000u},
    {20502,0x3f800000u},
    {21527,0x3f800000u},
    {22552,0x3f800000u},
    {23577,0x3f800000u},
    {24602,0x3f800000u},
    {25627,0x3f800000u},
    {26652,0x3f800000u},
    {27677,0x3f800000u},
    {28702,0x3f800000u},
    {29727,0x3f800000u},
    {30752,0x3f800000u},
    {31777,0x3f800000u},
    {32802,0x3f800000u},
    {33827,0x3f800000u},
    {34852,0x3f2aaaabu},
    {34853,0x3eaaaaabu},
    {35877,0x3eaaaaabu},
    {35878,0x3f2aaaabu},
    {36903,0x3f800000u},
    {37928,0x3f800000u},
    {38953,0x3f800000u},
    {39978,0x3f800000u},
    {41003,0x3f2aaaabu},
    {41004,0x3eaaaaabu},
    {42028,0x3eaaaaabu},
    {42029,0x3f2aaaabu},
    {43054,0x3f800000u},
    {44079,0x3f2aaaabu},
    {44080,0x3eaaaaabu},
    {45104,0x3eaaaaabu},
    {45105,0x3f2aaaabu},
    {46130,0x3f2aaaabu},
    {46131,0x3eaaaaabu},
    {47155,0x3eaaaaabu},
    {47156,0x3f2aaaabu},
    {48181,0x3f2aaaabu},
    {48182,0x3eaaaaabu},
    {49206,0x3eaaaaabu},
    {49207,0x3f2aaaabu},
    {50232,0x3f2aaaabu},
    {50233,0x3eaaaaabu},
    {51257,0x3e800000u},
    {51258,0x3f000000u},
    {51259,0x3e800000u},
    {52283,0x3eaaaaabu},
    {52284,0x3f2aaaabu},
    {53309,0x3f2aaaabu},
    {53310,0x3eaaaaabu},
    {54334,0x3e800000u},
    {54335,0x3f000000u},
    {54336,0x3e800000u},
    {55360,0x3e800000u},
    {55361,0x3f000000u},
    {55362,0x3e800000u},
    {56386,0x3e800000u},
    {56387,0x3f000000u},
    {56388,0x3e800000u},
    {57412,0x3e800000u},
    {57413,0x3f000000u},
    {57414,0x3e800000u},
    {58438,0x3e800000u},
    {58439,0x3f000000u},
    {58440,0x3e800000u},
    {59464,0x3e800000u},
    {59465,0x3f000000u},
    {59466,0x3e800000u},
    {60490,0x3e800000u},
    {60491,0x3f000000u},
    {60492,0x3e800000u},
    {61516,0x3e4ccccdu},
    {61517,0x3ecccccdu},
    {61518,0x3e888889u},
    {61519,0x3e088889u},
    {62542,0x3e088889u},
    {62543,0x3e888889u},
    {62544,0x3ecccccdu},
    {62545,0x3e4ccccdu},
    {63569,0x3e800000u},
    {63570,0x3f000000u},
    {63571,0x3e800000u},
    {64595,0x3e4ccccdu},
    {64596,0x3ecccccdu},
    {64597,0x3e888889u},
    {64598,0x3e088889u},
    {65621,0x3e088889u},
    {65622,0x3e888889u},
    {65623,0x3ecccccdu},
    {65624,0x3e4ccccdu},
    {66648,0x3e4ccccdu},
    {66649,0x3ecccccdu},
    {66650,0x3e888889u},
    {66651,0x3e088889u},
    {67674,0x3de38e39u},
    {67675,0x3e638e39u},
    {67676,0x3eaaaaabu},
    {67677,0x3e638e39u},
    {67678,0x3de38e39u},
    {68701,0x3e088889u},
    {68702,0x3e888889u},
    {68703,0x3ecccccdu},
    {68704,0x3e4ccccdu},
    {69728,0x3e4ccccdu},
    {69729,0x3ecccccdu},
    {69730,0x3e888889u},
    {69731,0x3e088889u},
    {70754,0x3de38e39u},
    {70755,0x3e638e39u},
    {70756,0x3eaaaaabu},
    {70757,0x3e638e39u},
    {70758,0x3de38e39u},
    {71781,0x3de38e39u},
    {71782,0x3e638e39u},
    {71783,0x3eaaaaabu},
    {71784,0x3e638e39u},
    {71785,0x3de38e39u},
    {72808,0x3de38e39u},
    {72809,0x3e638e39u},
    {72810,0x3eaaaaabu},
    {72811,0x3e638e39u},
    {72812,0x3de38e39u},
    {73835,0x3dc30c31u},
    {73836,0x3e430c31u},
    {73837,0x3e924925u},
    {73838,0x3e5b6db7u},
    {73839,0x3e124925u},
    {73840,0x3d924925u},
    {74862,0x3d924925u},
    {74863,0x3e124925u},
    {74864,0x3e5b6db7u},
    {74865,0x3e924925u},
    {74866,0x3e430c31u},
    {74867,0x3dc30c31u},
    {75890,0x3de38e39u},
    {75891,0x3e638e39u},
    {75892,0x3eaaaaabu},
    {75893,0x3e638e39u},
    {75894,0x3de38e39u},
    {76917,0x3dc30c31u},
    {76918,0x3e430c31u},
    {76919,0x3e924925u},
    {76920,0x3e5b6db7u},
    {76921,0x3e124925u},
    {76922,0x3d924925u},
    {77944,0x3d924925u},
    {77945,0x3e124925u},
    {77946,0x3e5b6db7u},
    {77947,0x3e924925u},
    {77948,0x3e430c31u},
    {77949,0x3dc30c31u},
    {78972,0x3dc30c31u},
    {78973,0x3e430c31u},
    {78974,0x3e924925u},
    {78975,0x3e5b6db7u},
    {78976,0x3e124925u},
    {78977,0x3d924925u},
    {79999,0x3d800000u},
    {80000,0x3e000000u},
    {80001,0x3e400000u},
    {80002,0x3e800000u},
    {80003,0x3e400000u},
    {80004,0x3e000000u},
    {80005,0x3d800000u},
    {81027,0x3d800000u},
    {81028,0x3e000000u},
    {81029,0x3e400000u},
    {81030,0x3e800000u},
    {81031,0x3e400000u},
    {81032,0x3e000000u},
    {81033,0x3d800000u},
    {82055,0x3d800000u},
    {82056,0x3e000000u},
    {82057,0x3e400000u},
    {82058,0x3e800000u},
    {82059,0x3e400000u},
    {82060,0x3e000000u},
    {82061,0x3d800000u},
    {83083,0x3d800000u},
    {83084,0x3e000000u},
    {83085,0x3e400000u},
    {83086,0x3e800000u},
    {83087,0x3e400000u},
    {83088,0x3e000000u},
    {83089,0x3d800000u},
    {84111,0x3d800000u},
    {84112,0x3e000000u},
    {84113,0x3e400000u},
    {84114,0x3e800000u},
    {84115,0x3e400000u},
    {84116,0x3e000000u},
    {84117,0x3d800000u},
    {85139,0x3d638e39u},
    {85140,0x3de38e39u},
    {85141,0x3e2aaaabu},
    {85142,0x3e638e39u},
    {85143,0x3e360b61u},
    {85144,0x3e088889u},
    {85145,0x3db60b61u},
    {85146,0x3d360b61u},
    {86167,0x3d360b61u},
    {86168,0x3db60b61u},
    {86169,0x3e088889u},
    {86170,0x3e360b61u},
    {86171,0x3e638e39u},
    {86172,0x3e2aaaabu},
    {86173,0x3de38e39u},
    {86174,0x3d638e39u},
    {87196,0x3d638e39u},
    {87197,0x3de38e39u},
    {87198,0x3e2aaaabu},
    {87199,0x3e638e39u},
    {87200,0x3e360b61u},
    {87201,0x3e088889u},
    {87202,0x3db60b61u},
    {87203,0x3d360b61u},
    {88224,0x3d23d70au},
    {88225,0x3da3d70au},
    {88226,0x3df5c290u},
    {88227,0x3e23d70au},
    {88228,0x3e4ccccdu},
    {88229,0x3e23d70au},
    {88230,0x3df5c290u},
    {88231,0x3da3d70au},
    {88232,0x3d23d70au},
    {89253,0x3d23d70au},
    {89254,0x3da3d70au},
    {89255,0x3df5c290u},
    {89256,0x3e23d70au},
    {89257,0x3e4ccccdu},
    {89258,0x3e23d70au},
    {89259,0x3df5c290u},
    {89260,0x3da3d70au},
    {89261,0x3d23d70au},
    {90282,0x3d23d70au},
    {90283,0x3da3d70au},
    {90284,0x3df5c290u},
    {90285,0x3e23d70au},
    {90286,0x3e4ccccdu},
    {90287,0x3e23d70au},
    {90288,0x3df5c290u},
    {90289,0x3da3d70au},
    {90290,0x3d23d70au},
    {91311,0x3d23d70au},
    {91312,0x3da3d70au},
    {91313,0x3df5c290u},
    {91314,0x3e23d70au},
    {91315,0x3e4ccccdu},
    {91316,0x3e23d70au},
    {91317,0x3df5c290u},
    {91318,0x3da3d70au},
    {91319,0x3d23d70au},
    {92340,0x3d23d70au},
    {92341,0x3da3d70au},
    {92342,0x3df5c290u},
    {92343,0x3e23d70au},
    {92344,0x3e4ccccdu},
    {92345,0x3e23d70au},
    {92346,0x3df5c290u},
    {92347,0x3da3d70au},
    {92348,0x3d23d70au},
    {93369,0x3d14f209u},
    {93370,0x3d94f209u},
    {93371,0x3ddf6b0fu},
    {93372,0x3e14f209u},
    {93373,0x3e3a2e8cu},
    {93374,0x3e1b26c9u},
    {93375,0x3df83e10u},
    {93376,0x3dba2e8cu},
    {93377,0x3d783e10u},
    {93378,0x3cf83e10u},
    {94398,0x3cf83e10u},
    {94399,0x3d783e10u},
    {94400,0x3dba2e8cu},
    {94401,0x3df83e10u},
    {94402,0x3e1b26c9u},
    {94403,0x3e3a2e8cu},
    {94404,0x3e14f209u},
    {94405,0x3ddf6b0fu},
    {94406,0x3d94f209u},
    {94407,0x3d14f209u},
    {95428,0x3d14f209u},
    {95429,0x3d94f209u},
    {95430,0x3ddf6b0fu},
    {95431,0x3e14f209u},
    {95432,0x3e3a2e8cu},
    {95433,0x3e1b26c9u},
    {95434,0x3df83e10u},
    {95435,0x3dba2e8cu},
    {95436,0x3d783e10u},
    {95437,0x3cf83e10u},
    {96457,0x3ce38e3bu},
    {96458,0x3d638e3bu},
    {96459,0x3daaaaacu},
    {96460,0x3de38e3bu},
    {96461,0x3e0e38e4u},
    {96462,0x3e2aaaacu},
    {96463,0x3e0e38e4u},
    {96464,0x3de38e3bu},
    {96465,0x3daaaaacu},
    {96466,0x3d638e3bu},
    {96467,0x3ce38e3bu},
    {97487,0x3cd20d21u},
    {97488,0x3d520d21u},
    {97489,0x3d9d89d9u},
    {97490,0x3dd20d21u},
    {97491,0x3e034834u},
    {97492,0x3e1d89d9u},
    {97493,0x3e070871u},
    {97494,0x3de10e11u},
    {97495,0x3db40b41u},
    {97496,0x3d870871u},
    {97497,0x3d340b41u},
    {97498,0x3cb40b41u},
    {98517,0x3cb40b41u},
    {98518,0x3d340b41u},
    {98519,0x3d870871u},
    {98520,0x3db40b41u},
    {98521,0x3de10e11u},
    {98522,0x3e070871u},
    {98523,0x3e1d89d9u},
    {98524,0x3e034834u},
    {98525,0x3dd20d21u},
    {98526,0x3d9d89d9u},
    {98527,0x3d520d21u},
    {98528,0x3cd20d21u},
    {99548,0x3cd20d21u},
    {99549,0x3d520d21u},
    {99550,0x3d9d89d9u},
    {99551,0x3dd20d21u},
    {99552,0x3e034834u},
    {99553,0x3e1d89d9u},
    {99554,0x3e070871u},
    {99555,0x3de10e11u},
    {99556,0x3db40b41u},
    {99557,0x3d870871u},
    {99558,0x3d340b41u},
    {99559,0x3cb40b41u},
    {100578,0x3cb40b41u},
    {100579,0x3d340b41u},
    {100580,0x3d870871u},
    {100581,0x3db40b41u},
    {100582,0x3de10e11u},
    {100583,0x3e070871u},
    {100584,0x3e1d89d9u},
    {100585,0x3e034834u},
    {100586,0x3dd20d21u},
    {100587,0x3d9d89d9u},
    {100588,0x3d520d21u},
    {100589,0x3cd20d21u},
    {101609,0x3cd20d21u},
    {101610,0x3d520d21u},
    {101611,0x3d9d89d9u},
    {101612,0x3dd20d21u},
    {101613,0x3e034834u},
    {101614,0x3e1d89d9u},
    {101615,0x3e070871u},
    {101616,0x3de10e11u},
    {101617,0x3db40b41u},
    {101618,0x3d870871u},
    {101619,0x3d340b41u},
    {101620,0x3cb40b41u},
    {102639,0x3c9c09c1u},
    {102640,0x3d1c09c1u},
    {102641,0x3d6a0ea1u},
    {102642,0x3d9c09c1u},
    {102643,0x3dc30c31u},
    {102644,0x3dea0ea1u},
    {102645,0x3e088889u},
    {102646,0x3deeeeefu},
    {102647,0x3dcccccdu},
    {102648,0x3daaaaabu},
    {102649,0x3d888889u},
    {102650,0x3d4ccccdu},
    {102651,0x3d088889u},
    {102652,0x3c888889u},
    {103670,0x3c888889u},
    {103671,0x3d088889u},
    {103672,0x3d4ccccdu},
    {103673,0x3d888889u},
    {103674,0x3daaaaabu},
    {103675,0x3dcccccdu},
    {103676,0x3deeeeefu},
    {103677,0x3e088889u},
    {103678,0x3dea0ea1u},
    {103679,0x3dc30c31u},
    {103680,0x3d9c09c1u},
    {103681,0x3d6a0ea1u},
    {103682,0x3d1c09c1u},
    {103683,0x3c9c09c1u},
    {104702,0x3c9c09c1u},
    {104703,0x3d1c09c1u},
    {104704,0x3d6a0ea1u},
    {104705,0x3d9c09c1u},
    {104706,0x3dc30c31u},
    {104707,0x3dea0ea1u},
    {104708,0x3e088889u},
    {104709,0x3deeeeefu},
    {104710,0x3dcccccdu},
    {104711,0x3daaaaabu},
    {104712,0x3d888889u},
    {104713,0x3d4ccccdu},
    {104714,0x3d088889u},
    {104715,0x3c888889u},
    {105733,0x3c888889u},
    {105734,0x3d088889u},
    {105735,0x3d4ccccdu},
    {105736,0x3d888889u},
    {105737,0x3daaaaabu},
    {105738,0x3dcccccdu},
    {105739,0x3deeeeefu},
    {105740,0x3e088889u},
    {105741,0x3dea0ea1u},
    {105742,0x3dc30c31u},
    {105743,0x3d9c09c1u},
    {105744,0x3d6a0ea1u},
    {105745,0x3d1c09c1u},
    {105746,0x3c9c09c1u},
    {106765,0x3c924925u},
    {106766,0x3d124925u},
    {106767,0x3d5b6db7u},
    {106768,0x3d924925u},
    {106769,0x3db6db6eu},
    {106770,0x3ddb6db7u},
    {106771,0x3e000000u},
    {106772,0x3de38e39u},
    {106773,0x3dc71c72u},
    {106774,0x3daaaaabu},
    {106775,0x3d8e38e4u},
    {106776,0x3d638e39u},
    {106777,0x3d2aaaabu},
    {106778,0x3ce38e39u},
    {106779,0x3c638e39u},
    {107796,0x3c562b81u},
    {107797,0x3cd62b81u},
    {107798,0x3d20a0a1u},
    {107799,0x3d562b81u},
    {107800,0x3d85db31u},
    {107801,0x3da0a0a1u},
    {107802,0x3dbb6611u},
    {107803,0x3dd62b81u},
    {107804,0x3df0f0f1u},
    {107805,0x3dd2d2d3u},
    {107806,0x3db4b4b5u},
    {107807,0x3d969697u},
    {107808,0x3d70f0f1u},
    {107809,0x3d34b4b5u},
    {107810,0x3cf0f0f1u},
    {107811,0x3c70f0f1u},
    {108829,0x3c800000u},
    {108830,0x3d000000u},
    {108831,0x3d400000u},
    {108832,0x3d800000u},
    {108833,0x3da00000u},
    {108834,0x3dc00000u},
    {108835,0x3de00000u},
    {108836,0x3e000000u},
    {108837,0x3de00000u},
    {108838,0x3dc00000u},
    {108839,0x3da00000u},
    {108840,0x3d800000u},
    {108841,0x3d400000u},
    {108842,0x3d000000u},
    {108843,0x3c800000u},
    {109861,0x3c70f0f1u},
    {109862,0x3cf0f0f1u},
    {109863,0x3d34b4b5u},
    {109864,0x3d70f0f1u},
    {109865,0x3d969697u},
    {109866,0x3db4b4b5u},
    {109867,0x3dd2d2d3u},
    {109868,0x3df0f0f1u},
    {109869,0x3dd62b81u},
    {109870,0x3dbb6611u},
    {109871,0x3da0a0a1u},
    {109872,0x3d85db31u},
    {109873,0x3d562b81u},
    {109874,0x3d20a0a1u},
    {109875,0x3cd62b81u},
    {109876,0x3c562b81u},
    {110893,0x3c4a4588u},
    {110894,0x3cca4588u},
    {110895,0x3d17b426u},
    {110896,0x3d4a4588u},
    {110897,0x3d7cd6ebu},
    {110898,0x3d97b426u},
    {110899,0x3db0fcd7u},
    {110900,0x3dca4588u},
    {110901,0x3de38e39u},
    {110902,0x3dca4588u},
    {110903,0x3db0fcd7u},
    {110904,0x3d97b426u},
    {110905,0x3d7cd6ebu},
    {110906,0x3d4a4588u},
    {110907,0x3d17b426u},
    {110908,0x3cca4588u},
    {110909,0x3c4a4588u},
    {111926,0x3c3fa030u},
    {111927,0x3cbfa030u},
    {111928,0x3d0fb824u},
    {111929,0x3d3fa030u},
    {111930,0x3d6f883du},
    {111931,0x3d8fb824u},
    {111932,0x3da7ac2au},
    {111933,0x3dbfa030u},
    {111934,0x3dd79436u},
    {111935,0x3dc20563u},
    {111936,0x3dac7692u},
    {111937,0x3d96e7bfu},
    {111938,0x3d8158edu},
    {111939,0x3d579436u},
    {111940,0x3d2c7692u},
    {111941,0x3d0158edu},
    {111942,0x3cac7692u},
    {111943,0x3c2c7692u},
    {112959,0x3c2c7693u},
    {112960,0x3cac7693u},
    {112961,0x3d0158eeu},
    {112962,0x3d2c7693u},
    {112963,0x3d579437u},
    {112964,0x3d8158eeu},
    {112965,0x3d96e7c0u},
    {112966,0x3dac7693u},
    {112967,0x3dc20565u},
    {112968,0x3dd79437u},
    {112969,0x3dbfa031u},
    {112970,0x3da7ac2bu},
    {112971,0x3d8fb825u},
    {112972,0x3d6f883eu},
    {112973,0x3d3fa031u},
    {112974,0x3d0fb825u},
    {112975,0x3cbfa031u},
    {112976,0x3c3fa031u},
    {113993,0x3c3fa030u},
    {113994,0x3cbfa030u},
    {113995,0x3d0fb824u},
    {113996,0x3d3fa030u},
    {113997,0x3d6f883du},
    {113998,0x3d8fb824u},
    {113999,0x3da7ac2au},
    {114000,0x3dbfa030u},
    {114001,0x3dd79436u},
    {114002,0x3dc20563u},
    {114003,0x3dac7692u},
    {114004,0x3d96e7bfu},
    {114005,0x3d8158edu},
    {114006,0x3d579436u},
    {114007,0x3d2c7692u},
    {114008,0x3d0158edu},
    {114009,0x3cac7692u},
    {114010,0x3c2c7692u},
    {115026,0x3c23d70au},
    {115027,0x3ca3d70au},
    {115028,0x3cf5c290u},
    {115029,0x3d23d70au},
    {115030,0x3d4ccccdu},
    {115031,0x3d75c290u},
    {115032,0x3d8f5c29u},
    {115033,0x3da3d70au},
    {115034,0x3db851ebu},
    {115035,0x3dcccccdu},
    {115036,0x3db851ebu},
    {115037,0x3da3d70au},
    {115038,0x3d8f5c29u},
    {115039,0x3d75c290u},
    {115040,0x3d4ccccdu},
    {115041,0x3d23d70au},
    {115042,0x3cf5c290u},
    {115043,0x3ca3d70au},
    {115044,0x3c23d70au},
    {116060,0x3c1c09c2u},
    {116061,0x3c9c09c2u},
    {116062,0x3cea0ea3u},
    {116063,0x3d1c09c2u},
    {116064,0x3d430c32u},
    {116065,0x3d6a0ea3u},
    {116066,0x3d888889u},
    {116067,0x3d9c09c2u},
    {116068,0x3daf8af9u},
    {116069,0x3dc30c32u},
    {116070,0x3db150e8u},
    {116071,0x3d9f959du},
    {116072,0x3d8dda53u},
    {116073,0x3d783e11u},
    {116074,0x3d54c77du},
    {116075,0x3d3150e8u},
    {116076,0x3d0dda53u},
    {116077,0x3cd4c77du},
    {116078,0x3c8dda53u},
    {116079,0x3c0dda53u},
    {117094,0x3c0dda52u},
    {117095,0x3c8dda52u},
    {117096,0x3cd4c77bu},
    {117097,0x3d0dda52u},
    {117098,0x3d3150e7u},
    {117099,0x3d54c77bu},
    {117100,0x3d783e0fu},
    {117101,0x3d8dda52u},
    {117102,0x3d9f959cu},
    {117103,0x3db150e7u},
    {117104,0x3dc30c31u},
    {117105,0x3daf8af8u},
    {117106,0x3d9c09c1u},
    {117107,0x3d888888u},
    {117108,0x3d6a0ea2u},
    {117109,0x3d430c31u},
    {117110,0x3d1c09c1u},
    {117111,0x3cea0ea2u},
    {117112,0x3c9c09c1u},
    {117113,0x3c1c09c1u},
    {118129,0x3c14f209u},
    {118130,0x3c94f209u},
    {118131,0x3cdf6b0fu},
    {118132,0x3d14f209u},
    {118133,0x3d3a2e8cu},
    {118134,0x3d5f6b0fu},
    {118135,0x3d8253c8u},
    {118136,0x3d94f209u},
    {118137,0x3da7904au},
    {118138,0x3dba2e8cu},
    {118139,0x3daaaaabu},
    {118140,0x3d9b26c9u},
    {118141,0x3d8ba2e9u},
    {118142,0x3d783e10u},
    {118143,0x3d59364du},
    {118144,0x3d3a2e8cu},
    {118145,0x3d1b26c9u},
    {118146,0x3cf83e10u},
    {118147,0x3cba2e8cu},
    {118148,0x3c783e10u},
    {118149,0x3bf83e10u},
    {119163,0x3bed7304u},
    {119164,0x3c6d7304u},
    {119165,0x3cb21643u},
    {119166,0x3ced7304u},
    {119167,0x3d1467e2u},
    {119168,0x3d321643u},
    {119169,0x3d4fc4a3u},
    {119170,0x3d6d7304u},
    {119171,0x3d8590b2u},
    {119172,0x3d9467e2u},
    {119173,0x3da33f13u},
    {119174,0x3db21643u},
    {119175,0x3da1e5b1u},
    {119176,0x3d91b51fu},
    {119177,0x3d81848eu},
    {119178,0x3d62a7f8u},
    {119179,0x3d4246d5u},
    {119180,0x3d21e5b1u},
    {119181,0x3d01848eu},
    {119182,0x3cc246d5u},
    {119183,0x3c81848eu},
    {119184,0x3c01848eu},
    {120199,0x3c01848fu},
    {120200,0x3c81848fu},
    {120201,0x3cc246d6u},
    {120202,0x3d01848fu},
    {120203,0x3d21e5b2u},
    {120204,0x3d4246d6u},
    {120205,0x3d62a7f9u},
    {120206,0x3d81848fu},
    {120207,0x3d91b520u},
    {120208,0x3da1e5b2u},
    {120209,0x3db21644u},
    {120210,0x3da33f14u},
    {120211,0x3d9467e3u},
    {120212,0x3d8590b3u},
    {120213,0x3d6d7305u},
    {120214,0x3d4fc4a4u},
    {120215,0x3d321644u},
    {120216,0x3d1467e3u},
    {120217,0x3ced7305u},
    {120218,0x3cb21644u},
    {120219,0x3c6d7305u},
    {120220,0x3bed7305u},
    {121234,0x3be38e39u},
    {121235,0x3c638e39u},
    {121236,0x3caaaaabu},
    {121237,0x3ce38e39u},
    {121238,0x3d0e38e3u},
    {121239,0x3d2aaaabu},
    {121240,0x3d471c71u},
    {121241,0x3d638e39u},
    {121242,0x3d800000u},
    {121243,0x3d8e38e3u},
    {121244,0x3d9c71c7u},
    {121245,0x3daaaaabu},
    {121246,0x3d9c71c7u},
    {121247,0x3d8e38e3u},
    {121248,0x3d800000u},
    {121249,0x3d638e39u},
    {121250,0x3d471c71u},
    {121251,0x3d2aaaabu},
    {121252,0x3d0e38e3u},
    {121253,0x3ce38e39u},
    {121254,0x3caaaaabu},
    {121255,0x3c638e39u},
    {121256,0x3be38e39u},
    {122270,0x3be38e39u},
    {122271,0x3c638e39u},
    {122272,0x3caaaaabu},
    {122273,0x3ce38e39u},
    {122274,0x3d0e38e3u},
    {122275,0x3d2aaaabu},
    {122276,0x3d471c71u},
    {122277,0x3d638e39u},
    {122278,0x3d800000u},
    {122279,0x3d8e38e3u},
    {122280,0x3d9c71c7u},
    {122281,0x3daaaaabu},
    {122282,0x3d9c71c7u},
    {122283,0x3d8e38e3u},
    {122284,0x3d800000u},
    {122285,0x3d638e39u},
    {122286,0x3d471c71u},
    {122287,0x3d2aaaabu},
    {122288,0x3d0e38e3u},
    {122289,0x3ce38e39u},
    {122290,0x3caaaaabu},
    {122291,0x3c638e39u},
    {122292,0x3be38e39u},
    {123306,0x3bda740du},
    {123307,0x3c5a740du},
    {123308,0x3ca3d709u},
    {123309,0x3cda740du},
    {123310,0x3d088888u},
    {123311,0x3d23d709u},
    {123312,0x3d3f258bu},
    {123313,0x3d5a740du},
    {123314,0x3d75c28eu},
    {123315,0x3d888888u},
    {123316,0x3d962fc9u},
    {123317,0x3da3d709u},
    {123318,0x3d973ca6u},
    {123319,0x3d8aa243u},
    {123320,0x3d7c0fc0u},
    {123321,0x3d62dafau},
    {123322,0x3d49a633u},
    {123323,0x3d30716du},
    {123324,0x3d173ca6u},
    {123325,0x3cfc0fc0u},
    {123326,0x3cc9a633u},
    {123327,0x3c973ca6u},
    {123328,0x3c49a633u},
    {123329,0x3bc9a633u},
    {124342,0x3bc1e4bcu},
    {124343,0x3c41e4bcu},
    {124344,0x3c916b8du},
    {124345,0x3cc1e4bcu},
    {124346,0x3cf25debu},
    {124347,0x3d116b8du},
    {124348,0x3d29a825u},
    {124349,0x3d41e4bcu},
    {124350,0x3d5a2154u},
    {124351,0x3d725debu},
    {124352,0x3d854d41u},
    {124353,0x3d916b8du},
    {124354,0x3d9d89d9u},
    {124355,0x3d916b8du},
    {124356,0x3d854d41u},
    {124357,0x3d725debu},
    {124358,0x3d5a2154u},
    {124359,0x3d41e4bcu},
    {124360,0x3d29a825u},
    {124361,0x3d116b8du},
    {124362,0x3cf25debu},
    {124363,0x3cc1e4bcu},
    {124364,0x3c916b8du},
    {124365,0x3c41e4bcu},
    {124366,0x3bc1e4bcu},
    {125379,0x3bbab657u},
    {125380,0x3c3ab657u},
    {125381,0x3c8c08c1u},
    {125382,0x3cbab657u},
    {125383,0x3ce963ecu},
    {125384,0x3d0c08c1u},
    {125385,0x3d235f8cu},
    {125386,0x3d3ab657u},
    {125387,0x3d520d21u},
    {125388,0x3d6963ecu},
    {125389,0x3d805d5bu},
    {125390,0x3d8c08c1u},
    {125391,0x3d97b426u},
    {125392,0x3d8cde23u},
    {125393,0x3d820821u},
    {125394,0x3d6e643bu},
    {125395,0x3d58b837u},
    {125396,0x3d430c31u},
    {125397,0x3d2d602cu},
    {125398,0x3d17b426u},
    {125399,0x3d020821u},
    {125400,0x3cd8b837u},
    {125401,0x3cad602cu},
    {125402,0x3c820821u},
    {125403,0x3c2d602cu},
    {125404,0x3bad602cu},
    {126416,0x3ba72f05u},
    {126417,0x3c272f05u},
    {126418,0x3c7ac687u},
    {126419,0x3ca72f05u},
    {126420,0x3cd0fac6u},
    {126421,0x3cfac687u},
    {126422,0x3d124924u},
    {126423,0x3d272f05u},
    {126424,0x3d3c14e5u},
    {126425,0x3d50fac6u},
    {126426,0x3d65e0a6u},
    {126427,0x3d7ac687u},
    {126428,0x3d87d633u},
    {126429,0x3d924924u},
    {126430,0x3d87d633u},
    {126431,0x3d7ac687u},
    {126432,0x3d65e0a6u},
    {126433,0x3d50fac6u},
    {126434,0x3d3c14e5u},
    {126435,0x3d272f05u},
    {126436,0x3d124924u},
    {126437,0x3cfac687u},
    {126438,0x3cd0fac6u},
    {126439,0x3ca72f05u},
    {126440,0x3c7ac687u},
    {126441,0x3c272f05u},
    {126442,0x3ba72f05u},
    {127454,0x3ba72f05u},
    {127455,0x3c272f05u},
    {127456,0x3c7ac687u},
    {127457,0x3ca72f05u},
    {127458,0x3cd0fac6u},
    {127459,0x3cfac687u},
    {127460,0x3d124924u},
    {127461,0x3d272f05u},
    {127462,0x3d3c14e5u},
    {127463,0x3d50fac6u},
    {127464,0x3d65e0a6u},
    {127465,0x3d7ac687u},
    {127466,0x3d87d633u},
    {127467,0x3d924924u},
    {127468,0x3d87d633u},
    {127469,0x3d7ac687u},
    {127470,0x3d65e0a6u},
    {127471,0x3d50fac6u},
    {127472,0x3d3c14e5u},
    {127473,0x3d272f05u},
    {127474,0x3d124924u},
    {127475,0x3cfac687u},
    {127476,0x3cd0fac6u},
    {127477,0x3ca72f05u},
    {127478,0x3c7ac687u},
    {127479,0x3c272f05u},
    {127480,0x3ba72f05u},
    {128492,0x3ba16b32u},
    {128493,0x3c216b32u},
    {128494,0x3c7220cbu},
    {128495,0x3ca16b32u},
    {128496,0x3cc9c5ffu},
    {128497,0x3cf220cbu},
    {128498,0x3d0d3dccu},
    {128499,0x3d216b32u},
    {128500,0x3d359898u},
    {128501,0x3d49c5ffu},
    {128502,0x3d5df364u},
    {128503,0x3d7220cbu},
    {128504,0x3d832718u},
    {128505,0x3d8d3dccu},
    {128506,0x3d83d347u},
    {128507,0x3d74d183u},
    {128508,0x3d61fc79u},
    {128509,0x3d4f276fu},
    {128510,0x3d3c5265u},
    {128511,0x3d297d5bu},
    {128512,0x3d16a851u},
    {128513,0x3d03d347u},
    {128514,0x3ce1fc79u},
    {128515,0x3cbc5265u},
    {128516,0x3c96a851u},
    {128517,0x3c61fc79u},
    {128518,0x3c16a851u},
    {128519,0x3b96a851u},
    {129530,0x3b91a2b5u},
    {129531,0x3c11a2b5u},
    {129532,0x3c5a740fu},
    {129533,0x3c91a2b5u},
    {129534,0x3cb60b62u},
    {129535,0x3cda740fu},
    {129536,0x3cfedcbcu},
    {129537,0x3d11a2b5u},
    {129538,0x3d23d70bu},
    {129539,0x3d360b62u},
    {129540,0x3d483fb8u},
    {129541,0x3d5a740fu},
    {129542,0x3d6ca865u},
    {129543,0x3d7edcbcu},
    {129544,0x3d888889u},
    {129545,0x3d7edcbcu},
    {129546,0x3d6ca865u},
    {129547,0x3d5a740fu},
    {129548,0x3d483fb8u},
    {129549,0x3d360b62u},
    {129550,0x3d23d70bu},
    {129551,0x3d11a2b5u},
    {129552,0x3cfedcbcu},
    {129553,0x3cda740fu},
    {129554,0x3cb60b62u},
    {129555,0x3c91a2b5u},
    {129556,0x3c5a740fu},
    {129557,0x3c11a2b5u},
    {129558,0x3b91a2b5u},
    {130569,0x3b8cf009u},
    {130570,0x3c0cf009u},
    {130571,0x3c53680du},
    {130572,0x3c8cf009u},
    {130573,0x3cb02c0bu},
    {130574,0x3cd3680du},
    {130575,0x3cf6a40fu},
    {130576,0x3d0cf009u},
    {130577,0x3d1e8e0au},
    {130578,0x3d302c0bu},
    {130579,0x3d41ca0cu},
    {130580,0x3d53680du},
    {130581,0x3d65060eu},
    {130582,0x3d76a40fu},
    {130583,0x3d842108u},
    {130584,0x3d77bdefu},
    {130585,0x3d6739ceu},
    {130586,0x3d56b5adu},
    {130587,0x3d46318cu},
    {130588,0x3d35ad6bu},
    {130589,0x3d25294au},
    {130590,0x3d14a529u},
    {130591,0x3d042108u},
    {130592,0x3ce739ceu},
    {130593,0x3cc6318cu},
    {130594,0x3ca5294au},
    {130595,0x3c842108u},
    {130596,0x3c46318cu},
    {130597,0x3c042108u},
    {130598,0x3b842108u},
    {131608,0x3b800000u},
    {131609,0x3c000000u},
    {131610,0x3c400000u},
    {131611,0x3c800000u},
    {131612,0x3ca00000u},
    {131613,0x3cc00000u},
    {131614,0x3ce00000u},
    {131615,0x3d000000u},
    {131616,0x3d100000u},
    {131617,0x3d200000u},
    {131618,0x3d300000u},
    {131619,0x3d400000u},
    {131620,0x3d500000u},
    {131621,0x3d600000u},
    {131622,0x3d700000u},
    {131623,0x3d800000u},
    {131624,0x3d700000u},
    {131625,0x3d600000u},
    {131626,0x3d500000u},
    {131627,0x3d400000u},
    {131628,0x3d300000u},
    {131629,0x3d200000u},
    {131630,0x3d100000u},
    {131631,0x3d000000u},
    {131632,0x3ce00000u},
    {131633,0x3cc00000u},
    {131634,0x3ca00000u},
    {131635,0x3c800000u},
    {131636,0x3c400000u},
    {131637,0x3c000000u},
    {131638,0x3b800000u},
    {132648,0x3b783e10u},
    {132649,0x3bf83e10u},
    {132650,0x3c3a2e8cu},
    {132651,0x3c783e10u},
    {132652,0x3c9b26cau},
    {132653,0x3cba2e8cu},
    {132654,0x3cd9364eu},
    {132655,0x3cf83e10u},
    {132656,0x3d0ba2e9u},
    {132657,0x3d1b26cau},
    {132658,0x3d2aaaabu},
    {132659,0x3d3a2e8cu},
    {132660,0x3d49b26du},
    {132661,0x3d59364eu},
    {132662,0x3d68ba2fu},
    {132663,0x3d783e10u},
    {132664,0x3d69a3d2u},
    {132665,0x3d5b0995u},
    {132666,0x3d4c6f58u},
    {132667,0x3d3dd51bu},
    {132668,0x3d2f3adeu},
    {132669,0x3d20a0a1u},
    {132670,0x3d120664u},
    {132671,0x3d036c27u},
    {132672,0x3ce9a3d2u},
    {132673,0x3ccc6f58u},
    {132674,0x3caf3adeu},
    {132675,0x3c920664u},
    {132676,0x3c69a3d2u},
    {132677,0x3c2f3adeu},
    {132678,0x3be9a3d2u},
    {132679,0x3b69a3d2u},
    {133688,0x3b62c4a7u},
    {133689,0x3be2c4a7u},
    {133690,0x3c2a137du},
    {133691,0x3c62c4a7u},
    {133692,0x3c8dbae8u},
    {133693,0x3caa137du},
    {133694,0x3cc66c12u},
    {133695,0x3ce2c4a7u},
    {133696,0x3cff1d3cu},
    {133697,0x3d0dbae8u},
    {133698,0x3d1be733u},
    {133699,0x3d2a137du},
    {133700,0x3d383fc8u},
    {133701,0x3d466c12u},
    {133702,0x3d54985cu},
    {133703,0x3d62c4a7u},
    {133704,0x3d70f0f1u},
    {133705,0x3d62c4a7u},
    {133706,0x3d54985cu},
    {133707,0x3d466c12u},
    {133708,0x3d383fc8u},
    {133709,0x3d2a137du},
    {133710,0x3d1be733u},
    {133711,0x3d0dbae8u},
    {133712,0x3cff1d3cu},
    {133713,0x3ce2c4a7u},
    {133714,0x3cc66c12u},
    {133715,0x3caa137du},
    {133716,0x3c8dbae8u},
    {133717,0x3c62c4a7u},
    {133718,0x3c2a137du},
    {133719,0x3be2c4a7u},
    {133720,0x3b62c4a7u},
    {134729,0x3b62c4a7u},
    {134730,0x3be2c4a7u},
    {134731,0x3c2a137du},
    {134732,0x3c62c4a7u},
    {134733,0x3c8dbae8u},
    {134734,0x3caa137du},
    {134735,0x3cc66c12u},
    {134736,0x3ce2c4a7u},
    {134737,0x3cff1d3cu},
    {134738,0x3d0dbae8u},
    {134739,0x3d1be733u},
    {134740,0x3d2a137du},
    {134741,0x3d383fc8u},
    {134742,0x3d466c12u},
    {134743,0x3d54985cu},
    {134744,0x3d62c4a7u},
    {134745,0x3d70f0f1u},
    {134746,0x3d62c4a7u},
    {134747,0x3d54985cu},
    {134748,0x3d466c12u},
    {134749,0x3d383fc8u},
    {134750,0x3d2a137du},
    {134751,0x3d1be733u},
    {134752,0x3d0dbae8u},
    {134753,0x3cff1d3cu},
    {134754,0x3ce2c4a7u},
    {134755,0x3cc66c12u},
    {134756,0x3caa137du},
    {134757,0x3c8dbae8u},
    {134758,0x3c62c4a7u},
    {134759,0x3c2a137du},
    {134760,0x3be2c4a7u},
    {134761,0x3b62c4a7u},
    {135770,0x3b5c4a01u},
    {135771,0x3bdc4a01u},
    {135772,0x3c253781u},
    {135773,0x3c5c4a01u},
    {135774,0x3c89ae41u},
    {135775,0x3ca53781u},
    {135776,0x3cc0c0c1u},
    {135777,0x3cdc4a01u},
    {135778,0x3cf7d342u},
    {135779,0x3d09ae41u},
    {135780,0x3d1772e1u},
    {135781,0x3d253781u},
    {135782,0x3d32fc21u},
    {135783,0x3d40c0c1u},
    {135784,0x3d4e8561u},
    {135785,0x3d5c4a01u},
    {135786,0x3d6a0ea1u},
    {135787,0x3d5d0dd0u},
    {135788,0x3d500d01u},
    {135789,0x3d430c30u},
    {135790,0x3d360b61u},
    {135791,0x3d290a90u},
    {135792,0x3d1c09c1u},
    {135793,0x3d0f08f0u},
    {135794,0x3d020821u},
    {135795,0x3cea0ea1u},
    {135796,0x3cd00d01u},
    {135797,0x3cb60b61u},
    {135798,0x3c9c09c1u},
    {135799,0x3c820821u},
    {135800,0x3c500d01u},
    {135801,0x3c1c09c1u},
    {135802,0x3bd00d01u},
    {135803,0x3b500d01u},
    {136811,0x3b44ce09u},
    {136812,0x3bc4ce09u},
    {136813,0x3c139a87u},
    {136814,0x3c44ce09u},
    {136815,0x3c76018cu},
    {136816,0x3c939a87u},
    {136817,0x3cac3448u},
    {136818,0x3cc4ce09u},
    {136819,0x3cdd67cau},
    {136820,0x3cf6018cu},
    {136821,0x3d074da6u},
    {136822,0x3d139a87u},
    {136823,0x3d1fe767u},
    {136824,0x3d2c3448u},
    {136825,0x3d388128u},
    {136826,0x3d44ce09u},
    {136827,0x3d511ae9u},
    {136828,0x3d5d67cau},
    {136829,0x3d51c0a5u},
    {136830,0x3d46197fu},
    {136831,0x3d3a7259u},
    {136832,0x3d2ecb34u},
    {136833,0x3d23240eu},
    {136834,0x3d177ce9u},
    {136835,0x3d0bd5c3u},
    {136836,0x3d002e9du},
    {136837,0x3ce90ef0u},
    {136838,0x3cd1c0a5u},
    {136839,0x3cba7259u},
    {136840,0x3ca3240eu},
    {136841,0x3c8bd5c3u},
    {136842,0x3c690ef0u},
    {136843,0x3c3a7259u},
    {136844,0x3c0bd5c3u},
    {136845,0x3bba7259u},
    {136846,0x3b3a7259u},
    {137853,0x3b358a48u},
    {137854,0x3bb58a48u},
    {137855,0x3c0827b6u},
    {137856,0x3c358a48u},
    {137857,0x3c62ecdbu},
    {137858,0x3c8827b6u},
    {137859,0x3c9ed8ffu},
    {137860,0x3cb58a48u},
    {137861,0x3ccc3b92u},
    {137862,0x3ce2ecdbu},
    {137863,0x3cf99e23u},
    {137864,0x3d0827b6u},
    {137865,0x3d13805bu},
    {137866,0x3d1ed8ffu},
    {137867,0x3d2a31a4u},
    {137868,0x3d358a48u},
    {137869,0x3d40e2edu},
    {137870,0x3d4c3b92u},
    {137871,0x3d579436u},
    {137872,0x3d4c3b92u},
    {137873,0x3d40e2edu},
    {137874,0x3d358a48u},
    {137875,0x3d2a31a4u},
    {137876,0x3d1ed8ffu},
    {137877,0x3d13805bu},
    {137878,0x3d0827b6u},
    {137879,0x3cf99e23u},
    {137880,0x3ce2ecdbu},
    {137881,0x3ccc3b92u},
    {137882,0x3cb58a48u},
    {137883,0x3c9ed8ffu},
    {137884,0x3c8827b6u},
    {137885,0x3c62ecdbu},
    {137886,0x3c358a48u},
    {137887,0x3c0827b6u},
    {137888,0x3bb58a48u},
    {137889,0x3b358a48u},
    {138896,0x3b30e2a4u},
    {138897,0x3bb0e2a4u},
    {138898,0x3c04a9fau},
    {138899,0x3c30e2a4u},
    {138900,0x3c5d1b4du},
    {138901,0x3c84a9fau},
    {138902,0x3c9ac64fu},
    {138903,0x3cb0e2a4u},
    {138904,0x3cc6fef8u},
    {138905,0x3cdd1b4du},
    {138906,0x3cf337a1u},
    {138907,0x3d04a9fau},
    {138908,0x3d0fb825u},
    {138909,0x3d1ac64fu},
    {138910,0x3d25d47au},
    {138911,0x3d30e2a4u},
    {138912,0x3d3bf0ceu},
    {138913,0x3d46fef8u},
    {138914,0x3d520d22u},
    {138915,0x3d478c7au},
    {138916,0x3d3d0bd2u},
    {138917,0x3d328b2au},
    {138918,0x3d280a82u},
    {138919,0x3d1d89dau},
    {138920,0x3d130931u},
    {138921,0x3d088889u},
    {138922,0x3cfc0fc3u},
    {138923,0x3ce70e73u},
    {138924,0x3cd20d22u},
    {138925,0x3cbd0bd2u},
    {138926,0x3ca80a82u},
    {138927,0x3c930931u},
    {138928,0x3c7c0fc3u},
    {138929,0x3c520d22u},
    {138930,0x3c280a82u},
    {138931,0x3bfc0fc3u},
    {138932,0x3ba80a82u},
    {138933,0x3b280a82u},
    {139939,0x3b23d70au},
    {139940,0x3ba3d70au},
    {139941,0x3bf5c290u},
    {139942,0x3c23d70au},
    {139943,0x3c4ccccdu},
    {139944,0x3c75c290u},
    {139945,0x3c8f5c29u},
    {139946,0x3ca3d70au},
    {139947,0x3cb851ebu},
    {139948,0x3ccccccdu},
    {139949,0x3ce147aeu},
    {139950,0x3cf5c290u},
    {139951,0x3d051eb8u},
    {139952,0x3d0f5c29u},
    {139953,0x3d19999au},
    {139954,0x3d23d70au},
    {139955,0x3d2e147bu},
    {139956,0x3d3851ebu},
    {139957,0x3d428f5cu},
    {139958,0x3d4ccccdu},
    {139959,0x3d428f5cu},
    {139960,0x3d3851ebu},
    {139961,0x3d2e147bu},
    {139962,0x3d23d70au},
    {139963,0x3d19999au},
    {139964,0x3d0f5c29u},
    {139965,0x3d051eb8u},
    {139966,0x3cf5c290u},
    {139967,0x3ce147aeu},
    {139968,0x3ccccccdu},
    {139969,0x3cb851ebu},
    {139970,0x3ca3d70au},
    {139971,0x3c8f5c29u},
    {139972,0x3c75c290u},
    {139973,0x3c4ccccdu},
    {139974,0x3c23d70au},
    {139975,0x3bf5c290u},
    {139976,0x3ba3d70au},
    {139977,0x3b23d70au}
}};
void check_reference_weight_values(std::span<const float> weights){
    check(weights.size()==137*1024,"reference weight matrix geometry");std::size_t selected=0;
    for(std::size_t i=0;i<weights.size();++i){
        const bool present=selected<reference_weight_values.size() && reference_weight_values[selected].index==i;
        const auto expected=present?reference_weight_values[selected++].bits:0u;
        check(std::bit_cast<std::uint32_t>(weights[i])==expected,"all reference weights match actual native bytes");
    }
    check(selected==reference_weight_values.size(),"all captured sparse weights consumed");
}
void reference_weight_construction(){
    std::array<float,1024> frequencies{};for(std::size_t i=0;i<frequencies.size();++i)frequencies[i]=float(i*48000)/2048.f;
    dsp_kernels::LogFrequencyFilterBank portable,reference;dsp_kernels::LogFrequencyFilterConfig config;
    check(portable.prepare(frequencies,config),"prepare unchanged default weight construction");
    config.precision=dsp_kernels::NumericalPrecision::DesktopReference;
    check(reference.prepare(frequencies,config) && reference.bands()==137,"prepare explicit source weight construction");
    const auto before=allocations;check_reference_weight_values(reference.weights());
    check(std::bit_cast<std::uint32_t>(portable.weights()[61516])==0x3e4cccceu,"default construction retains captured prior value");
    check(std::bit_cast<std::uint32_t>(reference.weights()[61516])==0x3e4ccccdu,"source construction selects independently captured native value");
    check(allocations==before,"checking prepared weights allocates no workspace");
}
void reference_filter_projection_controls(){
    reference_fma_projection_controls();reference_projection_failures();reference_weight_construction();
}
}
void* operator new(std::size_t size) {++allocations;if(auto* p=std::malloc(size?size:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t size) {return ::operator new(size);}
void operator delete(void* value) noexcept {std::free(value);}
void operator delete[](void* value) noexcept {std::free(value);}
void operator delete(void* value,std::size_t) noexcept {std::free(value);}
void operator delete[](void* value,std::size_t) noexcept {std::free(value);}
int main(int argc,char** argv) {
    if(argc==2 && std::string_view(argv[1])=="--reference-filter-only"){reference_filter_projection_controls();return 0;}
    if(argc==2 && std::string_view(argv[1])=="--reference-convolution-only"){reference_convolution_controls();return 0;}
    odd_transform_and_clone();reference_recipe_lifecycle();hybrid_storage_and_publication();
    acf_publication_range();window_publication_range();fourier_publication_range();
    source_magnitude_projection();source_magnitude_recipe();
    scalar_embedding_layout();reference_embedding_geometry();reference_convolution_controls();reference_filter_projection_controls();
}
