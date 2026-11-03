#include <core_app/numeric_default_graph.h>
#include <dataflow_runtime/runtime.h>
#include <domain/graph/node_factory.h>
#include <desktop/numeric_project_document.h>
#include <offline/audio_decoder.h>
#include <speex_backend.h>
#include <dsp_kernels/window_math.h>
#include <dsp_kernels/musical_features.h>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QCryptographicHash>
#include <QFileInfo>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numbers>
#include <source_location>
#include <utility>
namespace {
using namespace domain;using namespace signal_contracts;using namespace dataflow_runtime;
constexpr FrameLimits frames{1,1048576,4194304,1048576,4096,65536};
constexpr ProcessingAdmissionLimits limits{128,512,128,128,4194304,16777216,frames};
template<class T> void check(const T& ok,const char* message,const std::source_location where=std::source_location::current()) {
    if(!ok){std::fprintf(stderr,"FAIL %u: %s\n",where.line(),message);std::exit(1);}
}
const ConfiguredProcessingNode& node(const NumericProcessingGraph& graph,NodeId id) {
    for(const auto& n:graph.nodes)if(n.id==id)return n;
    std::abort();
}
NumericRuntimeProfile profile() {
    NumericRuntimeProfile p{limits,{{16777216,16777216,16777216,1048576},0,67108864},
        {1048576,8,4194304,4194304,4194304},{true,true,true}};
    p.music_enabled=p.experimental_resampling_enabled=true;p.resampler_backend=&desktop_resampling::speex_backend_factory();return p;
}
float element(const NumericView& v,std::size_t index) {
    if(v.contract->dtype==DType::UInt8){std::uint8_t value=0;check(load_element(v,{0,index,0},value)==Status::ok,"load UInt8 code");return float(value);}
    float value=0;const auto status=load_element(v,{0,index,0},value);
    if(status!=Status::ok)std::fprintf(stderr,"load status=%u dtype=%u rank=%u actual=%zu,%zu bytes=%zu index=%zu\n",unsigned(status),unsigned(v.contract->dtype),unsigned(v.contract->shape.rank),v.actual[0],v.actual[1],v.storage.size(),index);
    check(status==Status::ok,"load vector");return value;
}
struct Observation {
    std::array<float,2049> spectrum{};
    std::array<float,96> row{};
    std::array<std::array<std::uint8_t,96>,512> recent{};
    std::array<std::array<std::uint8_t,2048>,64> classical_recent{};
    std::uint64_t classical_rows=0;
    std::vector<float> boundaries;
    std::uint64_t rows=0,saturated=0,zero=0;
    std::int64_t last_ticks=0;
    void broadband(const NumericView& v) {
        for(std::size_t band=0;band<48;++band) {
            float maximum=0;unsigned bins=0;
            for(unsigned k=1;k<=1820;++k)if(k*(36000./4096)>=boundaries[band] && k*(36000./4096)<boundaries[band+1]){maximum=std::max(maximum,spectrum[k]);++bins;}
            check(bins>0 && std::abs(element(v,band)-maximum)<1e-6,"seven-section nonempty maximum oracle");
            if(band<12)check(bins==2,"each bass channel preserves the maximum of exactly two FFT bins");row[band]=maximum;
        }
    }
    void notes(const NumericView& v) {
        for(unsigned j=0;j<48;++j) {
            const double at=440*std::exp2((double(j)+48-69)/12.)*4096/36000.;const auto left=std::size_t(at);const double fraction=at-left;
            const double expected=spectrum[left]*(1-fraction)+spectrum[left+1]*fraction;
            check(std::abs(element(v,j)-expected)<2e-5,"note amplitude interpolation oracle");row[48+j]=element(v,j);
        }
    }
    void codes(const NumericView& v,const FrameStamp& stamp) {
        check(v.contract->dtype==DType::UInt8 && v.actual[0]==96,"compact code contract");
        if(rows)check(stamp.timestamp_ticks-last_ticks==600,"exact hop600 timestamps");last_ticks=stamp.timestamp_ticks;
        for(unsigned j=0;j<96;++j) {
            const auto expected=unsigned(std::floor(255*std::clamp(double(row[j]),0.,1.)+.5));
            check(element(v,j)==float(expected),"shared UInt8 amplitude scale oracle");recent[rows%512][j]=std::uint8_t(expected);
            saturated+=row[j]>=1;zero+=expected==0;
        }
        ++rows;
    }
    void classical_codes(const NumericView& v) {
        check(v.contract->dtype==DType::UInt8 && v.actual[0]==2048,"classical branch publishes all 2048 FFT values");
        for(size_t i=0;i<2048;++i) {
            const auto expected=std::uint8_t(std::floor(255*std::clamp(double(spectrum[i+1]),0.,1.)+.5));
            check(element(v,i)==expected,"classical UInt8 amplitudes use the common linear scale");classical_recent[classical_rows%64][i]=expected;
        }
        ++classical_rows;
    }
    static void receive(void* user,const NumericPortFrameView& event) {
        if(event.direction!=PortDirection::Output || !event.frame.stamp.available || event.identity.port)return;
        auto& self=*static_cast<Observation*>(user);check(event.status==Status::ok,"available output status");
        const auto& v=std::get<NumericView>(event.frame.payload);
        switch(event.identity.node) {
        case 10:self.classical_codes(v);break;
        case 15:for(unsigned k=1;k<2049;++k)self.spectrum[k]=element(v,k-1);break;
        case 31:self.broadband(v);break;
        case 32:self.notes(v);break;
        case 33:self.codes(v,event.frame.stamp);break;
        default:break;
        }
    }
};
struct Fixture {
    NumericProcessingGraph graph=numeric_compact_spectrogram_graph();
    NumericContract input=std::get<NumericContract>(node(graph,13).outputs[0].contract.view());
    std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> state;
    Observation observed;std::uint64_t position=0,packets=0;
    std::array<float,1024> packet_storage{};
    QJsonObject source_evidence;
    explicit Fixture(NumericProcessingGraph configured=numeric_compact_spectrogram_graph()):graph(std::move(configured)) {
        check(node(graph,4).kind=="RealFourier" && node(graph,5).kind=="AmplitudePhase","Existing split Fourier and amplitude/phase nodes");
        check(node(graph,4).outputs.size()==3 && node(graph,5).outputs.size()==2,"Separate Re/Im, DC and amplitude/phase ports");
        check(node(graph,3).outputs.size()==1,"WindowFunction publishes only coefficients");
        check(std::any_of(graph.edges.begin(),graph.edges.end(),[](const auto& edge){return edge.from==3 && edge.to==5 && edge.output_port==0 && edge.input_port==window_coefficients_input;}),"Actual window coefficients explicitly feed AmplitudePhase");
        check(node(graph,31).kind=="BroadbandFeatures","existing configurable maximum reducer");
        const auto& fields=std::get<TransformProcessingParameters>(node(graph,31).parameters).values;
        for(const auto& field:fields)if(field.key=="boundaries_hz")observed.boundaries=std::get<NumericArray>(field.value).values;
        check(observed.boundaries.size()==49 && observed.boundaries.front()==40 && observed.boundaries.back()==16000,"48 explicit bands in 40..16000 Hz");
        const std::array joins{0,12,16,22,30,36,40,48};const std::array hz{40.f,250.f,800.f,1500.f,3500.f,6000.f,10000.f,16000.f};
        for(size_t i=0;i<joins.size();++i)check(observed.boundaries[size_t(joins[i])]==hz[i],"exact seven-section joins and channel budget");
        check(std::any_of(graph.edges.begin(),graph.edges.end(),[](const auto& e){return e.from==1003 && e.to==31 && e.input_port==frequency_step_input;}),"explicit frequency step connection");
        const auto result=PreparedGraph::prepare_numeric(graph,profile(),plan,state);
        if(result!=Status::ok)std::fprintf(stderr,"prepare=%u\n",unsigned(result));check(result==Status::ok,"prepare complete spectrogram graph");
    }
    void feed(std::span<const float> values) {
        const PlaneLayout layout{0,{sizeof(float),0}};FrameStamp stamp;
        stamp.available=true;stamp.sequence=++packets;stamp.producer_epoch=1;stamp.epoch=1;
        stamp.timestamp_ticks=std::int64_t(position);stamp.sample_begin=std::int64_t(position);stamp.sample_count=values.size();stamp.has_sample_interval=true;
        check(values.size()<=packet_storage.size(),"bounded PCM ingress");std::copy(values.begin(),values.end(),packet_storage.begin());
        const NumericView v{&input,std::as_bytes(std::span(packet_storage)),{&layout,1},{values.size(),1}};
        const auto status=plan->process_numeric_ports(13,{stamp,v},*state,{}, {Observation::receive,nullptr,nullptr,&observed});
        if(status!=Status::ok)std::fprintf(stderr,"packet position=%llu count=%zu status=%u\n",static_cast<unsigned long long>(position),values.size(),unsigned(status));
        check(status==Status::ok,"process PCM packet");
        position+=values.size();
    }
    void classical_history() {
        DataFrameView result;std::uint64_t filled=0;check(plan->history(11,0,*state,result,&filled)==Status::ok,"read classical history");
        const auto& view=std::get<NumericView>(result.payload);
        check(filled==64 && view.actual==std::array<size_t,2>{64,2048} && view.storage.size()==131072,"complete 64x2048 UInt8 history");
        check(observed.classical_rows==observed.rows,"classical history advances on every compact analysis frame");
        for(size_t row=0;row<64;++row)for(size_t column=0;column<2048;++column) {
            std::uint8_t value=0;check(load_element(view,{0,row,column},value)==Status::ok,"read classical code");
            check(value==observed.classical_recent[(observed.classical_rows-64+row)%64][column],"classical ring retains every bin of every recent frame");
        }
    }
    DataFrameView history() {
        DataFrameView result;std::uint64_t filled=0;check(plan->history(7,0,*state,result,&filled)==Status::ok,"read compact history");
        const auto& v=std::get<NumericView>(result.payload);
        check(filled==512 && v.contract->dtype==DType::UInt8 && v.actual==std::array<std::size_t,2>{512,96},"512x96 UInt8 history");
        check(v.storage.size()==49152,"49152-byte processing history");
        for(unsigned r=0;r<512;++r)for(unsigned c=0;c<96;++c) {
            std::uint8_t actual=0;check(load_element(v,{0,r,c},actual)==Status::ok,"read logical history");
            check(actual==observed.recent[(observed.rows-512+r)%512][c],"ring wrap preserves every code and chronological row");
        }
        classical_history();
        for(const auto& n:graph.nodes)if(auto stats=plan->node_stats(n.id,*state))check(stats->errors==0,"node runtime errors");
        return result;
    }
};
void synthetic(Fixture& f) {
    std::array<float,256> pcm{};
    for(unsigned packet=0;packet<2400;++packet) {
        for(unsigned i=0;i<pcm.size();++i) {
            double value=0;const double t=(f.position+i)/48000.;
            for(const double hz:{70.,130.8127826502993,440.,1100.,1975.533205024496,2400.,4500.,7800.,13200.})value+=.025*std::sin(2*std::numbers::pi*(hz*t+.08*std::sin(t)));
            pcm[i]=float(value);
        }
        f.feed(std::span(pcm).first(packet%3==0?127:256));
    }
    check(f.observed.rows>512,"ring wrapped");(void)f.history();
}
QJsonObject spectrogram_view_layout() {
    const auto example=numeric_compact_spectrogram_example();const auto& source=example.compact_layouts.front();
    QJsonArray bounds;for(double value:source.broadband_centers_hz)bounds.append(value);
    QJsonArray sections;
    for(const auto& section:example.sections)if(section.view==8 && section.begin<48)
        sections.append(QJsonObject{{"label",section.label},{"begin",qint64(section.begin)},{"count",qint64(section.count)}});
    return {{"broadband_centers_hz",bounds},{"first_midi",int(source.first_midi)},{"broadband_sections",sections}};
}
QJsonObject spectrogram_view_settings(const NumericProcessingGraph& graph) {
    const auto graph_id=QString::fromStdString(graph.graph_id);
    const auto key=QString::fromLatin1(QJsonDocument(QJsonArray{"local",graph_id,8}).toJson(QJsonDocument::Compact).toHex());
    const QJsonArray sections{QJsonObject{{"label","Broadband"},{"begin",0},{"count",48}},QJsonObject{{"label","Notes"},{"begin",48},{"count",48}}};
    const QJsonObject view{{"node",8},{"device","local"},{"graph",graph_id},{"kind","CompactSpectrogramView"},{"instance",0},{"open",true},
        {"plot_sections",sections},{"compact_layout",spectrogram_view_layout()},{"value_scale","linear"},
        {"auto_range",false},{"minimum",0},{"maximum",255}};
    return {{"autosave_enabled",false},{"pipeline_auto_apply",true},{"graph_views",QJsonObject{{key,view}}}};
}
void persistence(const NumericProcessingGraph& graph,const QString& path={}) {
    desktop::NumericDraftPresentation presentation;
    presentation.application_settings={{"ui",spectrogram_view_settings(graph)}};
    presentation.node_presentations["200"]=QJsonObject{{"stream_names",QJsonArray{"Loopback","Microphone","Generator"}}};
    desktop::NumericProjectDocument document;check(desktop::create_numeric_project(graph,presentation,document),"create spectrogram project");
    QByteArray before,after;check(desktop::encode_numeric_graph(graph,before),"encode graph");
    NumericProcessingGraph restored;check(desktop::read_numeric_draft(document,restored),"read saved graph");
    check(desktop::encode_numeric_graph(restored,after) && before==after,"all coefficients, boundaries, routes and conversion policies survive persistence");
    QTemporaryDir temporary;check(temporary.isValid(),"temporary persistence fixture");
    const auto target=path.isEmpty()?temporary.filePath("spectrogram.json"):path;desktop::NumericSaveConsent consent;
    check(desktop::inspect_numeric_save_destination(target,consent) && desktop::save_numeric_project(document,target,&consent),"save standalone spectrogram config");
    desktop::NumericProjectDocument loaded;check(desktop::load_numeric_project(target,loaded,restored),"reload spectrogram config from disk");
    check(desktop::encode_numeric_graph(restored,after) && before==after,"on-disk graph preserves exact parameters");
}
void provenance(Fixture& f,const QString& path,std::uint32_t rate,double offset,double seconds) {
    QFile file(path);check(file.open(QIODevice::ReadOnly),"open read-only source for provenance");QCryptographicHash hash(QCryptographicHash::Sha256);
    check(hash.addData(&file),"hash actual music source");
    f.source_evidence={{"path",QFileInfo(file).absoluteFilePath()},{"sha256",QString::fromLatin1(hash.result().toHex())},{"native_rate",double(rate)},
        {"offset_seconds",offset},{"duration_seconds",seconds},{"file_frontend","existing_fir to48000; explicit PCM16 conversion before Q15"}};
}
void music(Fixture& f,const QString& path,double seconds,double offset) {
    desktop_analysis::offline::AudioDecoder decoder;QString error;
    check(decoder.open(path,error),qPrintable(error));
    provenance(f,path,decoder.sample_rate(),offset,seconds);
    dsp_kernels::PreparedResampler frontend;check(frontend.prepare(decoder.sample_rate(),48000,1),"explicit file frontend to48k");
    std::vector<float> output(frontend.maximum_output(256,decoder.sample_rate(),48000));
    std::array<float,256> pcm{};std::size_t count=0;std::uint64_t skipped=0;
    while(skipped<std::uint64_t(offset*decoder.sample_rate())) {
        const auto take=std::min<std::uint64_t>(pcm.size(),std::uint64_t(offset*decoder.sample_rate())-skipped);
        check(decoder.read(std::span(pcm).first(take),count,error) && count,"decode offset");skipped+=count;
    }
    while(f.position<std::uint64_t(seconds*48000)) {
        check(decoder.read(pcm,count,error),qPrintable(error));if(!count)break;
        std::size_t written=0;check(frontend.process(pcm,count,pcm.size(),output,output.size(),written),"file resampling to declared48k");
        for(std::size_t i=0;i<written && f.position<std::uint64_t(seconds*48000);) {
            const auto take=std::min({std::size_t{256},written-i,std::size_t(seconds*48000)-std::size_t(f.position)});
            f.feed(std::span(output).subspan(i,take));i+=take;
        }
    }
    check(f.observed.rows>=512,"music fills complete history");
}
void periodic_window() {
    using namespace dsp_kernels;PreparedWindows prepared(4096,true);std::array<float,512> smaller{};
    check(prepared.coefficients(WindowFunction::Hann,smaller),"prepared periodic smaller window");
    for(unsigned i=0;i<smaller.size();++i)check(std::abs(smaller[i]-(.5-.5*std::cos(2*std::numbers::pi*i/512)))<1e-6,"periodic Hann oracle");
    check(std::abs(window_sum(4096,WindowFunction::Hann,true)-2048)<1e-4,"periodic Hann coherent gain");
    check(window_coefficient(4095,4096,WindowFunction::Hann,true)>0 && window_coefficient(4095,4096,WindowFunction::Hann)==0,"symmetric and periodic endpoints distinct");
}
std::vector<std::int16_t> resampled(dsp_kernels::PreparedResampler& operation,std::span<const std::int16_t> input,std::size_t packet) {
    std::vector<std::int16_t> output(operation.maximum_output(input.size(),48000,36000)),result;
    for(std::size_t at=0;at<input.size();) {
        const auto count=std::min(packet,input.size()-at);std::size_t written=0;
        check(operation.process_pcm16(input.subspan(at,count),count,count,output,output.size(),written),"Q15 packet process");
        result.insert(result.end(),output.begin(),output.begin()+written);at+=count;
    }
    return result;
}
void q15_partition() {
    using namespace dsp_kernels;const ResamplerConfig config{ResamplerMode::SpeexDspQ15,48000,36000,1,5};
    const auto& factory=desktop_resampling::speex_backend_factory();PreparedResampler whole,fragmented;
    check(whole.prepare(config,&factory) && fragmented.prepare(config,&factory),"Q15 prepare");
    std::array<std::int16_t,8192> input{};
    for(unsigned i=0;i<input.size();++i) {
        double value=0;for(unsigned j=0;j<7;++j)value+=.025*std::sin(2*std::numbers::pi*(110+j*673.)*i/48000.);
        input[i]=std::int16_t(std::lround(32768*value));
    }
    const auto reference=resampled(whole,input,input.size());
    check(reference==resampled(fragmented,input,127),"exact Q15 packet partition invariance");
    PreparedResampler copied=fragmented;
    check(resampled(copied,input,256)==resampled(fragmented,input,256),"Q15 independent copied state");
    ResamplerResources query;check(PreparedResampler::resource_requirements(config,&factory,query) && query.owned_bytes==whole.resources().owned_bytes,"Q15 admitted and actual memory agree");
    std::array<float,8> floats{};std::array<float,9> output{};std::size_t written=99;
    check(!whole.process(floats,8,8,output,output.size(),written),"Q15 refuses implicit Float32 transport");
}
struct PcmWindowObservation {
    std::int16_t expected=1000;bool complete=false;
    static void receive(void* user,const NumericPortFrameView& event) {
        if(event.identity.node!=2 || event.direction!=PortDirection::Output || !event.frame.stamp.available)return;
        auto& result=*static_cast<PcmWindowObservation*>(user);const auto& v=std::get<NumericView>(event.frame.payload);
        check(v.contract->dtype==DType::Int16,"Int16 window storage contract");result.complete=true;
        for(unsigned i=0;i<4096;++i){std::int16_t value=0;check(load_element(v,{0,i,0},value)==Status::ok,"Int16 window element");result.complete=result.complete && value==result.expected;}
    }
};
void pcm16_window_silence() {
    ConfiguredProcessingNode source,window;check(make_processing_node("NetworkInput",1,limits,source,{36000,4096,256,512}),"make Int16 source");
    auto data=source.outputs[0].contract.view();auto& contract=std::get<NumericContract>(data);
    contract.dtype=DType::Int16;contract.representation.alignment=alignof(std::int16_t);
    OwnedProcessingContract owned;check(own_processing_contract(contract,frames,65536,owned)==Status::ok,"own Int16 source");
    source.outputs[0].contract=owned;const auto input=std::get<NumericContract>(owned.view());
    check(make_processing_node("Window",2,limits,window,{36000,4096,256,512}) && configure_processing_input(window,input,limits),"make Int16 accumulator");
    NumericProcessingGraph graph;graph.graph_id="pcm16-window";graph.execution_interval={1,60};graph.nodes={source,window};graph.edges={{1,2}};
    std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> state;
    check(PreparedGraph::prepare_numeric(graph,profile(),plan,state)==Status::ok,"prepare Int16 accumulation");
    std::array<std::int16_t,256> packet;packet.fill(1000);const PlaneLayout layout{0,{2,0}};PcmWindowObservation result;
    for(unsigned index=0;index<48;++index) {
        FrameStamp stamp;stamp.sequence=index+1;stamp.producer_epoch=1;stamp.timestamp_ticks=index*256;
        stamp.has_sample_interval=true;stamp.sample_begin=stamp.timestamp_ticks;stamp.sample_count=256;
        NumericView v{&input,std::as_bytes(std::span(packet)),{&layout,1},{256,1}};v.semantic_silence=index>=24;
        if(index>=24)result.expected=0;
        check(plan->process_numeric_ports(1,{stamp,v},*state,{}, {PcmWindowObservation::receive,nullptr,nullptr,&result})==Status::ok,"append Int16 packet");
        if(index==23 || index==47)check(result.complete,"semantic silence replaces nonzero backing storage without Float32 accumulation");
    }
}
void export_history(Fixture& f,const QString& directory) {
    QDir root(directory);check(root.mkpath("."),"create artifact directory");
    const auto result=f.history();const auto& v=std::get<NumericView>(result.payload);QByteArray bytes(49152,Qt::Uninitialized);
    for(unsigned r=0;r<512;++r)for(unsigned c=0;c<96;++c) {std::uint8_t x=0;check(load_element(v,{0,r,c},x)==Status::ok,"export code");bytes[r*96+c]=char(x);}
    QFile binary(root.filePath("history-512x96.u8"));check(binary.open(QIODevice::WriteOnly) && binary.write(bytes)==bytes.size(),"write compact history");
    QJsonArray edges;for(float x:f.observed.boundaries)edges.append(double(x));
    const QJsonObject evidence{{"rows",double(f.observed.rows)},{"history_bytes",49152},{"sample_rate",36000},{"fft_size",4096},{"hop",600},
        {"history_seconds",511./60},{"broadband_boundaries_hz",edges},{"broadband_centers_hz",spectrogram_view_layout().value("broadband_centers_hz")},{"note_first_midi",48},{"note_count",48},{"saturated",double(f.observed.saturated)},{"zero_codes",double(f.observed.zero)},
        {"source",f.source_evidence},{"renderer_reference",255},{"value_scale","linear"},{"broadband_sections",spectrogram_view_layout().value("broadband_sections")}};
    QFile json(root.filePath("processing.json"));check(json.open(QIODevice::WriteOnly),"open evidence");json.write(QJsonDocument(evidence).toJson());
    persistence(f.graph,root.filePath("spectrogram-loopback.json"));
}
}
NumericProcessingGraph configured_graph(int argc,char** argv) {
    if(argc!=4)return numeric_compact_spectrogram_graph();
    NumericProcessingGraph graph;desktop::NumericProjectDocument document;
    check(desktop::load_numeric_project(QString::fromLocal8Bit(argv[QString::fromLocal8Bit(argv[1])=="--synthetic"?2:3]),document,graph),"load the selected saved spectrogram project");return graph;
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    if(argc==3 && QString::fromLocal8Bit(argv[1])=="--export-project"){persistence(numeric_compact_spectrogram_graph(),QString::fromLocal8Bit(argv[2]));return 0;}
    periodic_window();q15_partition();pcm16_window_silence();Fixture fixture(configured_graph(argc,argv));
    if(argc>=3 && QString::fromLocal8Bit(argv[1])!="--synthetic"){music(fixture,QString::fromLocal8Bit(argv[1]),20,30);export_history(fixture,QString::fromLocal8Bit(argv[2]));}
    else {synthetic(fixture);if(argc==4)export_history(fixture,QString::fromLocal8Bit(argv[3]));else persistence(fixture.graph);}
    std::printf("SPECTROGRAM_PIPELINE_PASS frames=%llu bytes=49152\n",static_cast<unsigned long long>(fixture.observed.rows));return 0;
}
