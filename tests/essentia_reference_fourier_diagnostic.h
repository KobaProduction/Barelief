// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "essentia_reference_fixture.h"
#include <bit>

namespace essentia_reference {
struct FourierDifference {
    std::size_t count=0,first=0;float maximum=0,actual=0,expected=0;
    void add(std::size_t index,float a,float b) {
        check(std::isfinite(a) && std::isfinite(b),"finite Fourier diagnostic values");
        maximum=std::max(maximum,std::abs(a-b));
        if(std::bit_cast<std::uint32_t>(a)==std::bit_cast<std::uint32_t>(b))return;
        if(!count){first=index;actual=a;expected=b;}++count;
    }
    void report(const char* stage,std::size_t hop) const {
        std::fprintf(stderr,"Essentia Fourier diagnostic hop=%zu stage=%s bit_differences=%zu first=%zu actual=%.9g expected=%.9g max_abs=%.9g\n",
            hop,stage,count,first,double(actual),double(expected),double(maximum));
    }
};
struct FourierDiagnosticRows {
    static constexpr std::size_t length=2048,bins=1025;
    std::size_t hop=0,rows=0,seen=0;
    bool graph_real=false;
    std::vector<float> input,real,complex,magnitude,phase;
    std::array<FourierDifference,8> differences;
    std::array<std::size_t,5> publications{};
    FourierDiagnosticRows(std::size_t h,std::size_t r,bool real_graph):hop(h),rows(r),graph_real(real_graph),input(r*length),real(r*2*bins),
        complex(r*2*length),magnitude(r*bins),phase(r*bins) {}
    void capture(const rhythm_reference_ports::SpectralFrameStages& frame,
        dsp_kernels::PreparedFourierBackend& real_backend,dsp_kernels::PreparedFourierBackend& complex_backend,
        std::span<float> complex_input,std::span<float> real_output) {
        check(frame.frame_index==seen && seen<rows,"ordered source diagnostic frame");
        check(frame.framing.hop==hop && frame.zero_phase && frame.fourier.fft_size==length,"source diagnostic geometry");
        check(frame.fourier.transform_input.size()==length && frame.fourier.complex_values.size()==2*bins,"source diagnostic FFT spans");
        check(frame.magnitude.size()==bins && frame.phase.size()==bins,"source diagnostic polar spans");
        const auto offset=seen*length;std::copy(frame.fourier.transform_input.begin(),frame.fourier.transform_input.end(),input.begin()+offset);
        std::copy(frame.fourier.complex_values.begin(),frame.fourier.complex_values.end(),real.begin()+seen*2*bins);
        std::copy(frame.magnitude.begin(),frame.magnitude.end(),magnitude.begin()+seen*bins);
        std::copy(frame.phase.begin(),frame.phase.end(),phase.begin()+seen*bins);
        transform(frame,real_backend,complex_backend,complex_input,real_output);++seen;
    }
    void transform(const rhythm_reference_ports::SpectralFrameStages& frame,
        dsp_kernels::PreparedFourierBackend& rb,dsp_kernels::PreparedFourierBackend& cb,
        std::span<float> ci,std::span<float> ro) {
        for(std::size_t i=0;i<length;++i){ci[2*i]=frame.fourier.transform_input[i];ci[2*i+1]=0;}
        auto co=std::span(complex).subspan(seen*2*length,2*length);
        check(rb.process(frame.fourier.transform_input,ro) && cb.process(ci,co),"same-input real and complex KISS transforms");
        for(std::size_t i=0;i<2*bins;++i){
            differences[0].add(seen*2*bins+i,ro[i],frame.fourier.complex_values[i]);
            differences[1].add(seen*2*bins+i,co[i],ro[i]);
        }
    }
    void graph_matrix(const NumericView& view,std::size_t stage,PortId port) {
        check(view.actual[0]==rows,"graph diagnostic frame count");
        const auto columns=stage==0 || (stage==1 && !graph_real)?length:bins;
        check(view.actual[1]==columns,"graph diagnostic column count");
        ++publications[stage==0?0:stage==1?1+port:3+port];
        for(std::size_t row=0;row<rows;++row)for(std::size_t col=0;col<columns;++col){
            float value=0;check(load_element(view,{0,row,col},value)==Status::ok,"read graph diagnostic matrix");
            graph_value(row,col,stage,port,value);
        }
    }
    void graph_value(std::size_t row,std::size_t col,std::size_t stage,PortId port,float value) {
        if(stage==0){differences[2].add(row*length+col,value,input[row*length+col]);return;}
        if(stage==1){
            const auto index=row*2*(graph_real?bins:length)+2*col+port;
            differences[3].add(index,value,graph_real?real[index]:complex[index]);
            if(col<bins)differences[4].add(row*2*bins+2*col+port,value,real[row*2*bins+2*col+port]);
            return;
        }
        const auto index=row*bins+col;
        differences[5+port].add(index,value,port?phase[index]:magnitude[index]);
        if(col==0 || col==bins-1)differences[7].add(2*index+port,value,port?phase[index]:magnitude[index]);
    }
    void report() const {
        constexpr std::array names{"independent-real_vs_source-real","independent-complex_vs_independent-real",
            "graph-input_vs_source-input","graph-transform_vs_declared-independent-transform","graph-transform_vs_source-real",
            "graph-magnitude_vs_source-magnitude","graph-phase_vs_source-phase","graph-polar-DC-Nyquist_vs_source"};
        check(seen==rows,"complete source diagnostic frames");
        for(const auto count:publications)check(count==1,"single complete graph diagnostic publication");
        for(std::size_t i=0;i<names.size();++i)differences[i].report(names[i],hop);
        check(!differences[0].count && !differences[2].count && !differences[3].count,"same-input FFT first-divergence invariants");
        if(graph_real)check(!differences[4].count && !differences[5].count && !differences[6].count,"real graph FFT and ordinary polar exactly match source");
    }
};
struct EmphasisDiagnostic {
    static constexpr std::size_t rows=687,bands=40;
    std::vector<float> source_rows=std::vector<float>(rows*bands);
    std::array<float,rows> builtin_curve{},kiss_curve{};
    std::array<float,bands> graph_weights{};
    rhythm_reference_ports::PreparedBandBeatEmphasis builtin,kiss;
    std::size_t source_count=0,graph_count=0,weight_count=0;
    std::array<FourierDifference,6> differences;
    EmphasisDiagnostic() {
        check(builtin.prepare(rows),"prepare builtin emphasis diagnostic");
        check(kiss.prepare(rows,{},&desktop_fourier::kiss_backend_factory()),"prepare source KISS emphasis diagnostic");
    }
    void capture(const rhythm_reference_ports::GlobalOnsetFrameStages& frame) {
        check(frame.spectrum.frame_index==source_count && source_count<rows,"ordered source ERB rows");
        check(frame.erb_values.size()==bands && frame.erb_sample_rate==44100,"explicit source ERB domain");
        std::copy(frame.erb_values.begin(),frame.erb_values.end(),source_rows.begin()+source_count*bands);++source_count;
    }
    static bool read(const void* context,std::size_t row,std::span<float> values) noexcept {
        const auto& self=*static_cast<const EmphasisDiagnostic*>(context);
        if(row>=rows || values.size()!=bands)return false;
        std::copy_n(self.source_rows.begin()+row*bands,bands,values.begin());return true;
    }
    void process(std::span<const float> source_curve) {
        check(source_count==rows && source_curve.size()==rows,"complete source emphasis diagnostic");
        check(builtin.process({this,read},rows,builtin_curve) && kiss.process({this,read},rows,kiss_curve),"same ERB rows through builtin and KISS ACF");
        for(std::size_t i=0;i<rows;++i){
            differences[3].add(i,kiss_curve[i],source_curve[i]);differences[4].add(i,builtin_curve[i],kiss_curve[i]);
        }
        for(std::size_t i=0;i<bands;++i)differences[5].add(i,builtin.band_weights()[i],kiss.band_weights()[i]);
    }
    void graph_erb(const NumericView& view) {
        check(view.actual==std::array<std::size_t,2>{rows,bands},"graph ERB rows and bands");++graph_count;
        for(std::size_t row=0;row<rows;++row)for(std::size_t band=0;band<bands;++band){
            float value=0;check(load_element(view,{0,row,band},value)==Status::ok,"read graph ERB value");
            differences[0].add(row*bands+band,value,source_rows[row*bands+band]);
        }
    }
    void weights(const NumericView& view) {
        check(view.actual==std::array<std::size_t,2>{bands,1},"graph emphasis weight count");++weight_count;
        for(std::size_t i=0;i<bands;++i)check(load_element(view,{0,i,0},graph_weights[i])==Status::ok,"read graph emphasis weight");
    }
    void graph_differences(std::span<const float> graph_curve) {
        for(std::size_t i=0;i<rows;++i)differences[1].add(i,graph_curve[i],builtin_curve[i]);
        for(std::size_t i=0;i<bands;++i)differences[2].add(i,graph_weights[i],builtin.band_weights()[i]);
    }
    void report(std::span<const float> graph_curve,bool same_frontend) {
        check(graph_count==1 && weight_count==1 && graph_curve.size()==rows,"complete graph emphasis diagnostic");
        graph_differences(graph_curve);
        constexpr std::array names{"graph-ERB_vs_source-ERB","graph-emphasis_vs_builtin-same-ERB","graph-weights_vs_builtin-same-ERB",
            "independent-KISS-emphasis_vs_source-emphasis","builtin-emphasis_vs_KISS-emphasis","builtin-weights_vs_KISS-weights"};
        for(std::size_t i=0;i<names.size();++i)differences[i].report(names[i],512);
        check(!differences[3].count,"same source ERB and KISS ACF match source recipe");
        if(same_frontend)check(!differences[0].count && !differences[1].count && !differences[2].count,"same ERB first-divergence invariants");
    }
};
struct FourierDiagnostic {
    FourierDiagnosticRows primary,global;
    EmphasisDiagnostic band;
    dsp_kernels::PreparedFourierBackend real,complex;
    std::array<float,4096> complex_input{};std::array<float,2050> real_output{};
    explicit FourierDiagnostic(bool graph_real):primary(1024,344,graph_real),global(512,687,graph_real) {
        const auto& factory=desktop_fourier::kiss_backend_factory();
        check(real.prepare({2048,dsp_kernels::FourierBackendTransform::RealForward},factory),"diagnostic real provider");
        check(complex.prepare({2048,dsp_kernels::FourierBackendTransform::ComplexForward},factory),"diagnostic complex provider");
    }
    static void spectral(const void* context,const rhythm_reference_ports::PcmBeatFrameStages& frame) noexcept {
        auto& self=*static_cast<FourierDiagnostic*>(const_cast<void*>(context));
        self.primary.capture(frame.spectrum,self.real,self.complex,self.complex_input,self.real_output);
    }
    static void emphasis(const void* context,const rhythm_reference_ports::GlobalOnsetFrameStages& frame) noexcept {
        auto& self=*static_cast<FourierDiagnostic*>(const_cast<void*>(context));
        self.global.capture(frame.spectrum,self.real,self.complex,self.complex_input,self.real_output);
        self.band.capture(frame);
    }
    static void graph_output(void* context,const NumericPortFrameView& event) {
        if(event.status!=Status::ok || !event.frame.stamp.available)return;
        const auto id=event.identity.node;const auto port=event.identity.port;
        auto& self=*static_cast<FourierDiagnostic*>(context);
        if(id==44 && !port){self.band.graph_erb(std::get<NumericView>(event.frame.payload));return;}
        if(id==45 && port==1){self.band.weights(std::get<NumericView>(event.frame.payload));return;}
        if((id!=12 && id!=13 && id!=16 && id!=22 && id!=23 && id!=26) || port>1)return;
        auto& rows=id<20?self.primary:self.global;
        rows.graph_matrix(std::get<NumericView>(event.frame.payload),id%10==2?0:id%10==3?1:2,port);
    }
};
inline void fourier_diagnostic_source(std::span<const float> input,FourierDiagnostic& diagnostic,
    rhythm_reference_ports::PreparedPcmRhythmSummary& oracle) {
    rhythm_reference_ports::PcmBeatRecipeConfig config;config.window_precision=dsp_kernels::NumericalPrecision::DesktopReference;
    check(oracle.prepare(input.size(),config,&desktop_fourier::kiss_backend_factory()),"prepare diagnostic source");
    rhythm_reference_ports::PcmBeatStageOutputs stages{{&diagnostic,FourierDiagnostic::spectral},{&diagnostic,FourierDiagnostic::emphasis},{}};
    std::srand(17);check(oracle.process(input,stages),"source diagnostic complete session");
    diagnostic.band.process(oracle.tracker().onset_curve(3));
}
inline bool diagnostic_real_graph(const NumericProcessingGraph& g) {
    bool real=false;
    for(const auto id:{13,23}){
        const auto found=std::find_if(g.nodes.begin(),g.nodes.end(),[=](const auto& node){return node.id==id;});
        check(found!=g.nodes.end() && (found->kind=="Fourier" || found->kind=="RealFourier"),"explicit graph transform kind");
        const bool current=found->kind=="RealFourier";
        if(id==13)real=current;else check(real==current,"both diagnostic branches use declared transform kind");
    }
    return real;
}
inline void fourier_diagnostic_graph(GraphExecutor& executor,const NumericProcessingGraph& g,
    std::span<const float> input,FourierDiagnostic& diagnostic,Observation& observed) {
    observed.diagnostic_context=&diagnostic;observed.diagnostic=FourierDiagnostic::graph_output;
    std::srand(17);const auto before=allocations;push(executor,g,input,observed);
    check(allocations==before && !observed.errors,"allocation-free diagnostic graph delivery");require_publications(observed,EssentiaReferenceMethod::MultiFeature);
}
inline void report_fourier_diagnostic(FourierDiagnostic& diagnostic,const Observation& observed,
    const rhythm_reference_ports::PreparedPcmRhythmSummary& oracle) {
    diagnostic.primary.report();diagnostic.global.report();
    diagnostic.band.report(observed.curves[3],diagnostic.global.graph_real);
    for(std::size_t i=0;i<5;++i)compare(observed.curves[i],oracle.tracker().onset_curve(i),2e-4f,"diagnostic downstream ODF",i);
}
inline int fourier_diagnostic() {
    const auto input=signal();const auto g=graph(EssentiaReferenceMethod::MultiFeature);auto p=profile();GraphExecutor executor{p};
    application::PreparedExecution prepared;check(executor.prepare_numeric(g,{p.admission,p.resources},prepared)==Status::ok,"prepare diagnostic graph");
    check(executor.apply(std::move(prepared))==Status::ok,"apply diagnostic graph");
    FourierDiagnostic diagnostic{diagnostic_real_graph(g)};rhythm_reference_ports::PreparedPcmRhythmSummary oracle;
    fourier_diagnostic_source(input,diagnostic,oracle);
    Observation observed;fourier_diagnostic_graph(executor,g,input,diagnostic,observed);
    report_fourier_diagnostic(diagnostic,observed,oracle);
    return numerical_failures?1:0;
}
}
