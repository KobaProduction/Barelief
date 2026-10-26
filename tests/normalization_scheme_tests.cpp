#include <core_app/frequency_profile_binding.h>
#include <project_storage/numeric_array_codec.h>
#include <core_app/music_analysis_dialog.h>
#include <core_app/numeric_default_graph.h>
#include <core_app/scheme_editor/numeric_editor_model.h>
#include <core_app/scheme_editor/audio_node_model.h>
#include <core_app/scheme_editor/node_parameter_model.h>
#include <barelief/ui_text.h>
#include <barelief/numeric_image_view.h>
#include <barelief/charts/line.h>
#include <barelief/numeric_series_view.h>
#include <QTableView>
#include <desktop/numeric_project_document.h>
#include <dataflow_runtime/runtime.h>
#include <QApplication>
#include <QComboBox>
#include <QStandardItemModel>
#include <QCheckBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QDir>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QTableWidget>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QThread>
#include <QSpinBox>
#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <stdexcept>
#include <future>
#include <chrono>
#include <QQuickWindow>
#include <atomic>

namespace {
using namespace domain;using namespace signal_contracts;using namespace core_app;
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
desktop_analysis::offline::AnalysisServices analysis_services(){
    using namespace desktop_analysis::offline;
    return {analyze_music_files,analyze_music_preview,scan_audio_files,save_profile,load_profile,load_profile_diagnostic,scan_music_files,save_music_scan,load_music_scan};
}
void select_cadence(MusicAnalysisDialog& dialog,bool with_target,bool rational){
    auto* cadence=dialog.findChild<QComboBox*>("musicCadenceMode");auto* hop=dialog.findChild<QSpinBox*>("musicHop");
    require(cadence && hop,"explicit cadence controls exist");
    if(with_target)require(cadence->currentIndex()==1 && !hop->isEnabled(),"scanner owns its rational cadence independently of target");
    cadence->setCurrentIndex(rational?1:0);hop->setValue(128);
    require(hop->isEnabled()!=rational,"only fixed mode edits hop");
}
dsp_kernels::ProfileTraining capture_cadence(bool with_target,bool rational){
    using namespace desktop_analysis::offline;
    FrequencyProfileTarget target;target.options.training.interval_numerator=1;target.options.training.interval_denominator=119;
    auto targets=[&]{return with_target?std::vector{target}:std::vector<FrequencyProfileTarget>{};};
    std::promise<dsp_kernels::ProfileTraining> captured;auto result=captured.get_future();
    AnalysisServices services;services.analyze_files=[&](const QStringList&,const AnalysisOptions& options,const std::atomic_bool&,const AnalysisServices::Progress&){captured.set_value(options.training);return AnalysisResult{};};
    MusicAnalysisDialog dialog(targets,{},std::move(services));
    select_cadence(dialog,with_target,rational);
    dialog.findChild<QListWidget*>("musicFiles")->addItem("injected-service-fixture.wav");
    dialog.findChild<QPushButton*>("musicRun")->click();
    require(result.wait_for(std::chrono::seconds(5))==std::future_status::ready,"injected analysis receives explicit cadence");return result.get();
}
void dialog_cadence_contract(){
    const auto target=capture_cadence(true,true);require(target.interval_numerator==1 && target.interval_denominator==120,"graph target does not replace scanner cadence");
    const auto fixed=capture_cadence(true,false);require(fixed.interval_numerator==0 && fixed.hop==128,"fixed choice replaces rational target mode");
    const auto standalone=capture_cadence(false,true);require(standalone.interval_numerator==1 && standalone.interval_denominator==120,"standalone declared interval is explicit");
    require(standalone.gain_mode==dsp_kernels::ProfileGainMode::PeakNormalization,
            "Standalone analyzer defaults to peak normalization");
}
void put(ConfiguredProcessingNode& n,const char* key,ProcessingParameterValue v){auto& p=std::get<TransformProcessingParameters>(n.parameters).values;for(auto& f:p)if(f.key==key){f.value=std::move(v);return;}p.push_back({key,std::move(v)});}
ConfiguredProcessingNode& get(NumericProcessingGraph& g,NodeId id){return *std::find_if(g.nodes.begin(),g.nodes.end(),[&](const auto& n){return n.id==id;});}
dsp_kernels::FrequencyProfile train(const FrequencyProfileTarget& t){
    dsp_kernels::CompositionStatistics s;s.frames=s.active_frames=10;s.rms=.1;s.mean_spectrum.resize(t.options.compatibility.fft_size/2+1);
    for(std::size_t k=0;k<s.mean_spectrum.size();++k)s.mean_spectrum[k]=k?1.+k%11:1000000.;
    auto options=t.options.training;options.smoothing_radius=0;options.min_gain=.001;options.max_gain=1000;
    dsp_kernels::FrequencyProfile p;std::string error;require(dsp_kernels::build_frequency_profile(t.options.compatibility,options,std::span(&s,1),p,error),"train analytical profile");return p;
}
void binary_coefficients(){
    ConfiguredProcessingNode fill;fill.kind="ArrayFill";fill.parameters=TransformProcessingParameters{};
    for(const std::size_t size:{1,2,3,2048,32768}){
        std::vector<float> values(size);for(std::size_t i=0;i<size;++i)values[i]=float(i%37-18.)/7;
        values[0]=-0.f;if(size>1)values[1]=std::numeric_limits<float>::denorm_min();
        const auto encoded=project_storage::encode_array_generator_values(values);
        require(encoded.starts_with("f32le-base64:") && encoded.size()==13+4*((4*size+2)/3),"bounded binary array size including padding");
        NumericArray values_owner;require(project_storage::parse_numeric_array(encoded,values_owner)==Status::ok,"decode adapter coefficient format");put(fill,"values",std::move(values_owner));std::vector<float> decoded(size);std::size_t count=0;
        require(array_generator_values(fill,decoded,count)==Status::ok && count==size,"decode all binary coefficients");
        for(std::size_t i=0;i<size;++i)require(std::bit_cast<std::uint32_t>(decoded[i])==std::bit_cast<std::uint32_t>(values[i]),"binary values preserve exact Float32 bits");
    }
    NumericArray empty;
    require(project_storage::parse_numeric_array(project_storage::encode_array_generator_values({}),empty)==Status::ok && empty.values.empty(),"canonical empty coefficient roundtrip");
    const std::array<float,1> one{1};require(project_storage::encode_array_generator_values(one)=="f32le-base64:AACAPw==","portable little-endian IEEE754 fixture");
    for(const auto* invalid:{"f32le-base64:AACAPw=","f32le-base64:AACAPx==","f32le-base64:AACAPw==AAAA","f32le-base64:AAAA","f32le-base64:AACA/w==","f32le-base64:AACAfw==","f32le-base64:!!!!"}){
        NumericArray decoded;require(project_storage::parse_numeric_array(invalid,decoded)!=Status::ok,"adapter rejects malformed binary coefficient payload");
    }
}
void incompatible_routes(){
    auto routes=frequency_profile_targets(numeric_startup_graph());require(routes.size()==1,"one profile route fixture");
    const auto profile=train(routes[0]);auto incompatible=routes[0];incompatible.coefficients+=10000;incompatible.options.compatibility.sample_rate+=1;incompatible.label="Different Fs";routes.push_back(incompatible);
    QTemporaryDir temp;QString error;desktop_analysis::offline::ProfileDocument document;document.profile=profile;
    const auto path=temp.filePath("profile.json");require(desktop_analysis::offline::save_profile(path,document,error),"write compatibility fixture");
    MusicAnalysisDialog dialog([&]{return routes;},[](const auto&,const auto&,QString&){return true;},analysis_services());
    require(dialog.load_profile_from(path,error),"load compatibility fixture");auto* combo=dialog.findChild<QComboBox*>("musicProfileTarget");
    auto* items=qobject_cast<QStandardItemModel*>(combo->model());auto* apply=dialog.findChild<QPushButton*>("musicApplyProfile");
    require(combo->count()==2 && items->item(0)->isEnabled() && !items->item(1)->isEnabled() && items->item(1)->toolTip().contains(QString::number(incompatible.options.compatibility.sample_rate)),"all routes shown; incompatible grid disabled with reason");
    require(apply->isEnabled(),"compatible route remains applicable");
    combo->setCurrentIndex(1);require(!apply->isEnabled(),"even programmatic selection cannot apply incompatible route");
    dialog.findChild<QPushButton*>("musicNewScan")->click();require(items->item(1)->isEnabled(),"new scan may select previously incompatible route");
    dialog.findChild<QPushButton*>("musicShowResults")->click();require(!items->item(1)->isEnabled() && !apply->isEnabled(),"returning to completed results restores incompatibility");
}
FrequencyProfileTarget explicit_target(const NumericProcessingGraph& graph){
    const auto targets=frequency_profile_targets(graph);require(targets.size()==1,"one declared Fill frontend");return targets.front();
}
void verify_bound_coefficients(const NumericProcessingGraph& graph,const FrequencyProfileTarget& target,const dsp_kernels::FrequencyProfile& profile){
    ConfiguredProcessingNode fill,arithmetic;QString error;
    require(frequency_profile_edits(graph,target,profile,fill,arithmetic,error),"declared profile applies");
    std::vector<float> values(target.gain_count);std::size_t count=0;
    require(array_generator_values(fill,values,count)==Status::ok && count==values.size(),"declared gain slice is exact");
    for(std::size_t i=0;i<values.size();++i)require(values[i]==profile.gains[target.gain_first_bin+i],"every sliced coefficient preserved");
}
void profile_route_isolation(){
    auto graph=numeric_startup_graph();
    const auto target=explicit_target(graph);
    const auto profile=train(target);
    const auto accepted=application::apply_frequency_profile(graph,target.route,profile.gains,DspProfileMatcher{&profile});
    require(bool(accepted) && accepted.coefficients.id==target.coefficients &&
        accepted.arithmetic.id==target.arithmetic,"single-consumer route stages both edits");
    auto stale=target.route;stale.coefficient_input=0;
    require(application::apply_frequency_profile(graph,stale,profile.gains,DspProfileMatcher{&profile}).refusal==
        application::ProfileApplyRefusal::RouteUnavailable,"coefficient input identity is required");
    stale=target.route;stale.frame_source+=10000;
    require(application::apply_frequency_profile(graph,stale,profile.gains,DspProfileMatcher{&profile}).refusal==
        application::ProfileApplyRefusal::RouteUnavailable,"frame source identity is required");
    stale=target.route;stale.graph_id="another-graph";
    require(application::apply_frequency_profile(graph,stale,profile.gains,DspProfileMatcher{&profile}).refusal==
        application::ProfileApplyRefusal::RouteUnavailable,"graph identity is required");
    auto incompatible=profile;incompatible.compatibility.sample_rate+=1;
    const auto mismatch=application::apply_frequency_profile(graph,target.route,incompatible.gains,DspProfileMatcher{&incompatible});
    require(mismatch.refusal==application::ProfileApplyRefusal::ProfileMismatch &&
        !mismatch.mismatches.empty(),"frontend mismatch has typed reason");
    auto substituted=profile.gains;substituted[target.gain_first_bin]+=1;
    const auto altered=application::apply_frequency_profile(graph,target.route,substituted,DspProfileMatcher{&profile});
    require(altered.refusal==application::ProfileApplyRefusal::ProfileMismatch &&
        altered.mismatches.front()==application::ProfileMismatchReason::InvalidCoefficient,
        "matcher validates the exact staged coefficient sequence");
    std::vector<float> before(target.gain_count),after(target.gain_count);
    std::size_t count=0;
    require(array_generator_values(get(graph,target.coefficients),before,count)==Status::ok,
        "read original coefficient source");
    const auto clone_id=NodeId{9015};
    auto clone=get(graph,target.arithmetic);clone.id=clone_id;graph.nodes.push_back(clone);
    graph.edges.push_back({target.route.coefficients,clone_id,target.route.coefficient_output,target.route.coefficient_input});
    graph.edges.push_back({target.route.frame_source,clone_id,target.route.frame_output,target.route.frame_input});
    const auto routes=application::frequency_profile_routes(graph,DspProfileMatcher{});
    require(routes.size()==2,"both shared Fill routes remain visible for diagnosis");
    for(const auto& route:routes){
        const auto refused=application::apply_frequency_profile(graph,route,profile.gains,DspProfileMatcher{&profile});
        require(refused.refusal==application::ProfileApplyRefusal::SharedCoefficients,
            "shared coefficient source refuses isolated apply");
    }
    require(array_generator_values(get(graph,target.coefficients),after,count)==Status::ok &&
        after==before,"refusal does not mutate source graph");
}
void explicit_resampler_frontends(){
    for(const auto& policy:signal_contracts::resampler_policies()){
        auto graph=numeric_startup_graph();auto target=explicit_target(graph);
        auto& fill=get(graph,target.coefficients);put(fill,"resampler_mode",std::string(policy.id));put(fill,"resampler_quality",NumericLiteral{std::uint64_t{7}});
        target=explicit_target(graph);require(target.options.compatibility.resampler_mode==policy.mode && target.options.compatibility.resampler_quality==7,"frontend comes from declared Fill");
        const auto profile=train(target);verify_bound_coefficients(graph,target,profile);
        for(auto& source:graph.nodes)if(source.kind=="PcmResampler"){put(source,"mode",std::string{"bypass"});put(source,"quality",NumericLiteral{std::uint64_t{0}});source.enabled=false;}
        const auto unchanged=explicit_target(graph);
        require(unchanged.options.compatibility.resampler_mode==target.options.compatibility.resampler_mode && unchanged.options.compatibility.resampler_quality==7,"upstream resampler edits do not rewrite declared frontend");
        verify_bound_coefficients(graph,target,profile);
    }
}
void explicit_spectrum_frontend(bool compensate,const char* representation){
    auto graph=numeric_startup_graph();auto target=explicit_target(graph);auto& fill=get(graph,target.coefficients);
    put(fill,"representation",std::string{representation});put(fill,"normalize",compensate);target=explicit_target(graph);
    const auto profile=train(target);require(profile.compatibility.projection.compensate==compensate,"declared compensation reaches analyzer");
    verify_bound_coefficients(graph,target,profile);
    auto wrong=profile;wrong.compatibility.channel_policy=dsp_kernels::ChannelPolicy::Select;wrong.compatibility.channel_index=1;
    require(!frequency_profile_incompatibility(target,wrong).isEmpty(),"channel mismatch explained");
    put(fill,"normalize",!compensate);
    ConfiguredProcessingNode coefficients,arithmetic;QString error;
    require(!frequency_profile_edits(graph,target,profile,coefficients,arithmetic,error),"changed declared frontend rejects earlier profile");
}
void representations(){
    explicit_resampler_frontends();
    for(const bool compensate:{false,true})for(const auto* representation:{"magnitude","peak_amplitude","power","psd"})explicit_spectrum_frontend(compensate,representation);
    for(const auto* representation:{"amplitude_db","power_db","psd_db"}){
        auto graph=numeric_startup_graph();const auto target=explicit_target(graph);put(get(graph,target.coefficients),"representation",std::string{representation});
        require(frequency_profile_targets(graph).empty(),"display-only dB is not an analysis frontend");
    }
}
desktop_analysis::offline::ProfileDocument preview_document(const FrequencyProfileTarget& target){
    using namespace desktop_analysis::offline;
    ProfileDocument document;document.profile=train(target);document.profile.compositions[0].source="explicit-preview";
    SpectrogramPreview preview;preview.source="explicit-preview";preview.rows=2;preview.bands=2;preview.duration_seconds=.2;
    preview.sample_rate=target.options.compatibility.sample_rate;preview.values={1,2,3,4};preview.counts={1,1};
    preview.time_seconds={.01,.16};preview.frequency_hz={13,97};document.previews.push_back(std::move(preview));return document;
}
void inspect_preview_document(const desktop_analysis::offline::ProfileDocument& document,bool valid){
    using namespace desktop_analysis::offline;
    AnalysisServices services;services.load=[document](const QString&,ProfileDocument& output,QString&){output=document;return true;};
    MusicAnalysisDialog dialog({}, {},std::move(services));QString error;
    require(dialog.load_profile_from("injected-profile",error),"load explicit preview fixture");
    const auto* image=dialog.findChild<NumericImageView*>();require(image,"generic preview image exists");
    if(!valid){
        dialog.findChild<QCheckBox*>("musicPreviewCorrection")->setChecked(true);
        require(image->value_count()==0,"malformed corrected preview does not invent zero cells");return;
    }
    const auto& axes=image->source_axis_metadata();
    require(axes.ages==document.previews[0].time_seconds && axes.columns==document.previews[0].frequency_hz,"UI preserves supplied non-endpoint bucket centers");
}
void explicit_preview_coordinates(){
    const auto target=explicit_target(numeric_startup_graph());auto document=preview_document(target);
    inspect_preview_document(document,true);document.previews[0].values.pop_back();inspect_preview_document(document,false);
}
void loaded_frontend_baseline(){
    using namespace desktop_analysis::offline;
    const auto target=explicit_target(numeric_startup_graph());auto document=preview_document(target);
    document.profile.compatibility.channel_policy=dsp_kernels::ChannelPolicy::Select;document.profile.compatibility.channel_index=1;
    document.profile.compatibility.resampler_mode=dsp_kernels::ResamplerMode::CompactFir;
    std::promise<dsp_kernels::ProfileCompatibility> captured;auto result=captured.get_future();AnalysisServices services;
    services.load=[document](const QString&,ProfileDocument& output,QString&){output=document;return true;};
    services.analyze_files=[&](const QStringList&,const AnalysisOptions& options,const std::atomic_bool&,const AnalysisServices::Progress&){captured.set_value(options.compatibility);return AnalysisResult{};};
    MusicAnalysisDialog dialog([target]{return std::vector{target};},{},std::move(services));QString error;
    require(dialog.load_profile_from("injected-profile",error),"loaded frontend becomes explicit baseline");
    dialog.findChild<QPushButton*>("musicNewScan")->click();dialog.findChild<QPushButton*>("musicRun")->click();
    require(result.wait_for(std::chrono::seconds(5))==std::future_status::ready,"new scan uses injected analysis service");
    const auto frontend=result.get();
    require(frontend.channel_policy==dsp_kernels::ChannelPolicy::Select && frontend.channel_index==1 && frontend.resampler_mode==dsp_kernels::ResamplerMode::CompactFir,"selected target does not silently overwrite loaded frontend");
}
void typed_mismatch_messages(){
    const auto target=explicit_target(numeric_startup_graph());auto profile=train(target);
    profile.compatibility.channel_policy=dsp_kernels::ChannelPolicy::Select;profile.compatibility.channel_index=1;profile.compatibility.coherent_gain=100;
    profile.compatibility.reference=2;profile.compatibility.floor_db=-100;
    const auto reasons=dsp_kernels::profile_mismatches(profile,target.options.compatibility);
    for(const auto* language:{"en","ru"}){
        ui::Language::instance().set(language);const auto message=frequency_profile_incompatibility(target,profile);
        require(message.split('\n').size()==qsizetype(reasons.size()) && reasons.size()>=5,"every typed mismatch has a localized explanation");
        require(message.contains(language==std::string_view{"ru"}?QString::fromUtf8("канал"):QStringLiteral("channel"),Qt::CaseInsensitive),"channel mismatch has a human explanation");
    }
    ui::Language::instance().set("en");
}
auto normalization_length(const auto& port,const NumericContract& vector,const NumericContract& scalar) {
    ConfiguredProcessingNode length;length.id=1;length.kind="ArrayLength";length.parameters=TransformProcessingParameters{};length.inputs={port(PortDirection::Input,0,vector)};length.outputs={port(PortDirection::Output,0,scalar)};
return length;
}

auto normalization_source(const auto& port,const NumericContract& vector) {
    ConfiguredProcessingNode source;source.id=3;source.kind="NetworkInput";source.parameters=EndpointProcessingParameters{};source.outputs={port(PortDirection::Output,0,vector)};
return source;
}

auto normalization_fill(const auto& port,const NumericContract& scalar,bool logarithmic) {
    ConfiguredProcessingNode fill;fill.id=2;fill.kind="ArrayFill";fill.parameters=TransformProcessingParameters{};put(fill,"size",NumericLiteral{std::uint64_t{4}});put(fill,"values",logarithmic?NumericArray{{10,-10,40,-40}}:NumericArray{{.5f,2,1,4}});fill.inputs={port(PortDirection::Input,0,scalar)};
return fill;
}

auto normalization_product(const auto& port,const NumericContract& vector,const OwnedProcessingContract& coefficients,bool logarithmic) {
    ConfiguredProcessingNode op;op.id=4;op.kind="ArrayMultiply";op.parameters=TransformProcessingParameters{};put(op,"operation",std::string{logarithmic?"add":"multiply"});put(op,"frame_input",NumericLiteral{std::int64_t{0}});put(op,"floor_enabled",logarithmic);
    op.inputs={port(PortDirection::Input,0,vector),{1,PortDirection::Input,coefficients}};OwnedProcessingContract output;require(array_product_output_contract(op,output)==Status::ok,"derive arithmetic array");op.outputs={{0,PortDirection::Output,output}};
return op;
}

auto normalization_runtime_graph(const NumericContract& vector,const NumericContract& scalar,bool logarithmic,ConfiguredProcessingNode& fill,OwnedProcessingContract& coefficients) {
constexpr FrameLimits limits{1,32,4096,4096,256,4096};
    auto port=[&](PortDirection side,PortId id,const NumericContract& c){OwnedProcessingContract out;require(own_processing_contract(c,limits,65536,out)==Status::ok,"own primitive port");return ProcessingPort{id,side,std::move(out)};};
auto length=normalization_length(port,vector,scalar);auto source=normalization_source(port,vector);fill=normalization_fill(port,scalar,logarithmic);
require(array_generator_output_contract(fill,scalar,coefficients)==Status::ok,"derive coefficient array");fill.outputs={{0,PortDirection::Output,coefficients}};
auto op=normalization_product(port,vector,coefficients,logarithmic);
    NumericProcessingGraph graph;graph.graph_id="numeric-normalization";graph.nodes={length,fill,source,op};graph.edges={{3,1},{1,2},{2,4,0,1},{3,4}};
return graph;
}

struct NormalizationResult{std::array<float,4> values{};unsigned calls=0;};
void normalization_result(void* user,const dataflow_runtime::NumericPortFrameView& event) {
if(event.identity.node!=4 || event.status!=Status::ok)return;auto& r=*static_cast<NormalizationResult*>(user);++r.calls;const auto& v=std::get<NumericView>(event.frame.payload);for(std::size_t i=0;i<4;++i)require(load_element(v,{0,i,0},r.values[i])==Status::ok,"read corrected value");}

static void replace_runtime_coefficients(bool logarithmic,domain::NumericProcessingGraph& graph,const dataflow_runtime::NumericRuntimeProfile& profile,std::shared_ptr<const dataflow_runtime::PreparedGraph>& plan,std::unique_ptr<dataflow_runtime::RuntimeContext>& state,NormalizationResult& result,domain::ConfiguredProcessingNode& fill,const NumericContract& scalar,domain::OwnedProcessingContract& coefficients,const auto& feed,const NumericContract& vector,const std::array<float,4>& samples) {
    using namespace dataflow_runtime;
    const std::array<float,4> replacement=logarithmic?std::array<float,4>{0,0,0,0}:std::array<float,4>{2,2,2,2};
    put(get(graph,2),"values",NumericArray{std::vector<float>(replacement.begin(),replacement.end())});auto previous=std::move(plan);auto old=std::move(state);
    require(PreparedGraph::prepare_numeric(graph,profile,plan,state,previous.get(),old.get())==Status::ok,"replace correction in a running compatible graph");feed(3,vector,std::as_bytes(std::span(samples)),4);
    require(result.calls==2 && result.values==(logarithmic?samples:std::array<float,4>{4,6,0,2}),"live Apply uses new coefficients instead of retaining the old prepared buffer");
    put(fill,"values",NumericArray{{1,std::numeric_limits<float>::quiet_NaN(),1,1}});std::size_t count=0;require(array_generator_values(fill,{},count)!=Status::ok,"nonfinite coefficient refused");
    put(fill,"values",NumericArray{{1,1}});require(array_generator_output_contract(fill,scalar,coefficients)!=Status::ok,"wrong coefficient length refused");
}
void runtime_arrays(bool logarithmic){
    using namespace dataflow_runtime;constexpr std::array<std::uint32_t,1> mono{0};constexpr FrameLimits limits{1,32,4096,4096,256,4096};
    NumericContract vector;vector.channel_order=mono;vector.clock={"fixture",{1,48000},48000};vector.shape={Rank::Vector,{Axis::Sample,Axis::None},{ExtentBounds{4,4},ExtentBounds{}}};vector.representation.alignment=alignof(float);
    OwnedProcessingContract measured;require(numeric_metadata_output_contract(vector,false,measured)==Status::ok,"measure actual input length");const auto scalar=std::get<NumericContract>(measured.view());
    ConfiguredProcessingNode fill;OwnedProcessingContract coefficients;
    auto graph=normalization_runtime_graph(vector,scalar,logarithmic,fill,coefficients);
    NumericRuntimeProfile profile{{16,32,8,32,1048576,4*1048576,limits},{{1048576,1048576,1048576,131072},0,16*1048576},{4096,1,4096,4096,1048576},{true,true,true}};
    for(const auto& n:graph.nodes){ResolvedProcessingNode resolved;const auto diagnostic=resolve_processing_node(n,profile.admission,resolved);if(!diagnostic)std::fprintf(stderr,"primitive admission node=%lld error=%u port=%u\n",static_cast<long long>(n.id),unsigned(diagnostic.error),unsigned(diagnostic.port));}
    NumericProcessingGraph evaluated,executable;const auto binding=resolve_numeric_parameter_bindings(graph,profile.admission,evaluated,executable);if(!binding)std::fprintf(stderr,"primitive binding node=%lld error=%u port=%u\n",static_cast<long long>(binding.node),unsigned(binding.error),unsigned(binding.port));
    std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> state;const auto prepared=PreparedGraph::prepare_numeric(graph,profile,plan,state);if(prepared!=Status::ok)std::fprintf(stderr,"primitive prepare status=%u\n",unsigned(prepared));require(prepared==Status::ok,"prepare numeric normalization runtime");
    NormalizationResult result;
    const auto callback=normalization_result;
    std::uint64_t sequence=0;auto feed=[&](NodeId id,const NumericContract& c,std::span<const std::byte> bytes,std::size_t count){const PlaneLayout layout{0,{4,0}};FrameStamp stamp;stamp.available=true;stamp.sequence=stamp.source_sequence=++sequence;stamp.generation=stamp.producer_epoch=1;
        DataFrameView frame{stamp,NumericView{&c,bytes,std::span(&layout,1),{count,1},{},{}}};require(plan->process_numeric_ports(id,0,frame,*state,{},NumericRuntimeCallbacks{callback,nullptr,nullptr,&result})==Status::ok,"process correction input");};
    const std::array<float,4> samples=logarithmic?std::array<float,4>{-60,-50,-120,-119}:std::array<float,4>{2,3,0,1};feed(3,vector,std::as_bytes(std::span(samples)),4);
    require(result.calls==1,"one corrected frame, not one per parameter update");
    require(result.values==(logarithmic?std::array<float,4>{-50,-60,-120,-120}:std::array<float,4>{1,6,0,4}),"actual runtime applies numeric coefficients and preserves the silence floor");
    replace_runtime_coefficients(logarithmic,graph,profile,plan,state,result,fill,scalar,coefficients,feed,vector,samples);
}
void wait_for_plot_frame(NumericSeriesView& chart){
    std::atomic_bool rendered=false;
    const auto connection=QObject::connect(chart.quickWindow(),&QQuickWindow::afterRendering,&chart,[&]{rendered=true;},Qt::DirectConnection);
    chart.quickWindow()->update();QElapsedTimer deadline;deadline.start();
    while(!rendered && deadline.elapsed()<2000){QCoreApplication::processEvents();QThread::msleep(2);}
    QObject::disconnect(connection);
    require(rendered,"new plot frame follows the theme and language change");
}
void capture_profile_plots(MusicAnalysisDialog& dialog,const QString& evidence,const char* language,const char* theme){
    for(const auto* name:{"musicMeanCurve","musicGainCurve"}){
        auto* chart=dynamic_cast<NumericSeriesView*>(dialog.findChild<QWidget*>(name));require(chart,"shared numeric comparison plot");
        QElapsedTimer deadline;deadline.start();
        while(deadline.elapsed()<2000){QCoreApplication::processEvents();if(!chart->numeric_raster().isNull())break;QThread::msleep(2);}
        require(!chart->numeric_raster().isNull(),"wait for actual plot geometry before capture");
        wait_for_plot_frame(*chart);
        const auto pixels=chart->numeric_raster();
        require(pixels.pixelColor(pixels.width()-1,0)==chart->palette().color(QPalette::Base),"numeric raster follows the current application palette");
        require(chart->presentation_model()->state().value("title").toString().contains(ui::text(qstrcmp(name,"musicMeanCurve")==0?"Measurements":"Gain, dB")),"comparison names follow the current language");
        require(chart->grabFramebuffer().save(evidence+QString("/%1-%2-%3.png").arg(name,language,theme)),"save owned Quick plot framebuffer");
    }
}
void normalization_dialog_images(MusicAnalysisDialog& dialog) {
    const auto evidence=QDir(QFileInfo(QString::fromUtf8(__FILE__)).absolutePath()).absoluteFilePath("../.agent-workspace/normalization-ui");require(QDir().mkpath(evidence),"create own-renderer image directory");
    for(const auto* language:{"ru","en"})for(const auto* theme:{"light","dark"}){ui::Language::instance().set(language);ui::Theme::instance().set(theme);dialog.findChild<QTabWidget*>()->setCurrentIndex(4);QCoreApplication::processEvents();
        dialog.findChild<QPushButton*>("musicApplyStep")->click();QCoreApplication::processEvents();
        require(dialog.grab().save(evidence+QString("/application-%1-%2.png").arg(language,theme)),"save owned application-stage renderer");
        dialog.findChild<QPushButton*>("musicCurveStep")->click();QCoreApplication::processEvents();
        require(dialog.grab().save(evidence+QString("/analysis-%1-%2.png").arg(language,theme)),"save owned dialog renderer image");
        dialog.findChild<QTabWidget*>()->setCurrentIndex(0);QCoreApplication::processEvents();
        capture_profile_plots(dialog,evidence,language,theme);
        require(dialog.grab().save(evidence+QString("/curve-comparison-%1-%2.png").arg(language,theme)),"save measured and approximated spectra");
        dialog.findChild<QTabWidget*>()->setCurrentIndex(2);QCoreApplication::processEvents();
        require(dialog.grab().save(evidence+QString("/results-preview-%1-%2.png").arg(language,theme)),"save owned cached-preview renderer");
        dialog.findChild<QTabWidget*>()->setCurrentIndex(4);QCoreApplication::processEvents();
        require(dialog.grab().save(evidence+QString("/results-track-spectrum-%1-%2.png").arg(language,theme)),"save owned per-track spectrum renderer");
        dialog.findChild<QPushButton*>("musicNewScan")->click();QCoreApplication::processEvents();
        require(dialog.grab().save(evidence+QString("/setup-%1-%2.png").arg(language,theme)),"save owned scan-settings renderer");
        dialog.findChild<QPushButton*>("musicShowResults")->click();QCoreApplication::processEvents();}}

void normalization_project_roundtrip(MusicAnalysisDialog& dialog,NumericEditorGraphModel& model,const auto& registry,QUndoStack& undo,const auto& original,const auto& target,const auto& applied,const QTemporaryDir& directory,QString& error) {
    require(applied.has_value(),"analysis result reaches the explicit apply action");desktop_analysis::offline::ProfileDocument exported;exported.profile=*applied;
    const auto profilePath=directory.filePath("frequency-profile.json");require(desktop_analysis::offline::save_profile(profilePath,exported,error),"export actual trained profile");desktop_analysis::offline::ProfileDocument imported;
    require(desktop_analysis::offline::load_profile(profilePath,imported,error) && imported.profile.gains==applied->gains && imported.profile.mean_spectrum==applied->mean_spectrum && imported.profile.frequency_presence==applied->frequency_presence,"profile export/reload preserves actual means, occurrence and coefficients");
    const auto* summary=model.delegateModel<AudioNodeModel>(target.coefficients)->parameter_model()->field("coefficientSummary");require(summary && summary->primary,"card displays the explicit coefficient count instead of a misleading scalar one");
    const auto corrected=model.save();undo.undo();require(model.save()==original,"Undo restores the full original scheme");undo.redo();require(model.save()==corrected,"Redo restores coefficient array and operation");
    NumericEditorGraphModel restored(registry);restored.load(corrected);require(restored.save()==corrected,"coefficient curve persists with current project graph");
    auto graph=model.numeric_graph();desktop::NumericProjectDocument project;desktop::NumericDraftPresentation display;require(bool(desktop::create_numeric_project(graph,display,project)),"stage corrected project");
    const auto path=directory.filePath("corrected.json");require(bool(desktop::save_numeric_project(project,path)),"save corrected project");desktop::NumericProjectDocument loaded;NumericProcessingGraph reloaded;require(bool(desktop::load_numeric_project(path,loaded,reloaded)),"reload corrected project");
    require(same_processing_parameters(get(reloaded,target.coefficients).parameters,get(graph,target.coefficients).parameters),"saved project retains exact coefficients");
    require(dialog.findChild<QLabel*>("musicStatus")->text().contains("save",Qt::CaseInsensitive) || dialog.findChild<QLabel*>("musicStatus")->text().contains(QString::fromUtf8("сохран"),Qt::CaseInsensitive),"dialog confirms scheme application");
}

void normalization_dialog_scan(MusicAnalysisDialog& dialog,const auto& target,const QString& wavPath) {
    auto* choices=dialog.findChild<QComboBox*>("musicSpectrum");auto* compensation=dialog.findChild<QCheckBox*>("musicSpectrumCompensation");
    require(choices && choices->count()==1 && compensation && compensation->isChecked(),"scan stores calibrated peak amplitude independently of graph route");
    require(choices->currentData().toInt()==int(dsp_kernels::SpectrumProjection::PeakAmplitude),"canonical amplitude statistics");
    (void)target;
    auto* files=dialog.findChild<QListWidget*>("musicFiles");files->addItem(wavPath);files->addItem(wavPath);
    auto* run=dialog.findChild<QPushButton*>("musicRun");run->click();QElapsedTimer clock;clock.start();while(!dialog.findChild<QPushButton*>("musicSaveProfile")->isEnabled() && clock.elapsed()<15000){QCoreApplication::processEvents();QThread::msleep(2);}
    auto* applyButton=dialog.findChild<QPushButton*>("musicApplyProfile");require(applyButton && applyButton->isEnabled(),"actual batch dialog prepares applicable profile");
    auto* statistics=dialog.findChild<QTableView*>("musicFrequencyStatistics");require(statistics && statistics->model()->rowCount()==2048 && !statistics->model()->data(statistics->model()->index(1,2)).toString().isEmpty(),"scanner frequency measurements stay on its independent grid");
    require(!dialog.findChild<QLabel*>("musicMeanValue")->text().isEmpty(),"aggregate mean shown");
    auto* curve=dynamic_cast<NumericSeriesView*>(dialog.findChild<QWidget*>("musicGainCurve"));
    require(curve && curve->presentation_model()->axes().value("yTitle").toString()==ui::text("Gain, dB"),
            "linear stored coefficients have a clearly labeled dB preview");
    dialog.findChild<QPushButton*>("musicApplyStep")->click();applyButton->click();
}

QString normalization_wave_fixture(const QTemporaryDir& directory) {
QByteArray wav;const unsigned rate=48000,frames=12000;auto word=[&](unsigned v,unsigned n){for(unsigned b=0;b<n;++b)wav.append(char(v>>(8*b)));};
    wav.append("RIFF");word(36+2*frames,4);wav.append("WAVEfmt ");word(16,4);word(1,2);word(1,2);word(rate,4);word(2*rate,4);word(2,2);word(16,2);wav.append("data");word(2*frames,4);
    for(unsigned i=0;i<frames;++i){double sample=0;for(unsigned tone=0;tone<8;++tone)sample+=.035*std::sin(2*std::numbers::pi*(200+tone*173)*i/rate+.3*std::sin(double(i)/rate+tone));word(unsigned(std::int16_t(sample*32767)),2);}
    QFile file(directory.filePath("tones.wav"));require(file.open(QIODevice::WriteOnly)&&file.write(wav)==wav.size(),"write synthetic file");file.close();
return file.fileName();
}

void missing_profile_routes(const NumericProcessingGraph& initial,NumericEditorGraphModel& model,const auto& initialTargets) {
    if(initialTargets.empty()){
        std::fprintf(stderr,"profile routes before editor=%zu after editor=0\n",frequency_profile_targets(initial).size());
        for(const auto& n:model.numeric_graph().nodes)if(n.kind=="AmplitudePhase" || n.kind=="PcmResampler" || n.kind=="WindowFunction"){
            std::fprintf(stderr,"route node=%lld kind=%s\n",static_cast<long long>(n.id),n.kind.c_str());
            for(const auto& field:std::get<TransformProcessingParameters>(n.parameters).values){
                if(const auto* text=std::get_if<std::string>(&field.value))std::fprintf(stderr,"  %s=%s\n",field.key.c_str(),text->c_str());
                if(const auto* number=std::get_if<NumericLiteral>(&field.value))std::fprintf(stderr,"  %s=%.12g\n",field.key.c_str(),std::visit([](auto value){return double(value);},*number));
            }
        }
    }
}

void dialog_and_persistence(const NumericProcessingGraph& initial){
    auto registry=std::make_shared<QtNodes::NodeDelegateModelRegistry>();const auto catalog=numeric_editor::catalog();for(const auto& kind:catalog)registry->registerModel([kind]{return std::make_unique<AudioNodeModel>(kind);},numeric_editor::category(kind));
    NumericEditorGraphModel model(registry);QUndoStack undo;model.set_undo_stack(&undo);QString error;
    require(model.set_numeric_graph(initial,&error),"load current standard scheme");const auto original=model.save();
    const auto targets=[&]{return frequency_profile_targets(model.numeric_graph());};const auto initialTargets=targets();
    missing_profile_routes(initial,model,initialTargets);
    require(!initialTargets.empty(),"no compatible profile route in the imported graph");auto target=initialTargets.front();
    std::optional<dsp_kernels::FrequencyProfile> applied;
    auto apply=[&](const FrequencyProfileTarget& t,const dsp_kernels::FrequencyProfile& p,QString& error){ConfiguredProcessingNode fill,arithmetic;if(!frequency_profile_edits(model.numeric_graph(),t,p,fill,arithmetic,error))return false;if(!model.edit_nodes({fill,arithmetic},&error))return false;applied=p;return true;};
    QTemporaryDir directory;const auto wavPath=normalization_wave_fixture(directory);
    MusicAnalysisDialog dialog(targets,apply,analysis_services());dialog.show();QCoreApplication::processEvents();
    normalization_dialog_scan(dialog,target,wavPath);
    require(undo.count()==1 && model.save()!=original,"apply updates both coefficient and arithmetic nodes in one undo item");
    normalization_project_roundtrip(dialog,model,registry,undo,original,target,applied,directory,error);
    normalization_dialog_images(dialog);

}
}
void resampler_live_edit_regression(){
    auto registry=std::make_shared<QtNodes::NodeDelegateModelRegistry>();
    const auto catalog=numeric_editor::catalog();
    for(const auto& kind:catalog)
        registry->registerModel([kind]{return std::make_unique<AudioNodeModel>(kind);},numeric_editor::category(kind));
    NumericEditorGraphModel model(registry);QString error;
    require(model.set_numeric_graph(numeric_startup_graph(),&error),"Startup example loads in editor");
    auto original=model.numeric_graph();auto changed=get(original,332);
    // The measured coefficients stay at 32768 Hz. A locally valid rate edit
    // remains an incompatible draft; exporting it must fail atomically.
    put(changed,"output_rate",NumericLiteral{std::uint64_t{48000}});
    require(model.edit_node(QtNodes::NodeId(332),changed),
            "Locally valid sample-rate edit must remain available as a draft");
    const auto draft=model.numeric_graph();
    require(std::is_permutation(draft.edges.begin(),draft.edges.end(),original.edges.begin(),original.edges.end()),
            "Incompatible draft preserves graph edges for repair");
    auto executable=original;QByteArray before,after;
    require(bool(desktop::encode_numeric_graph(executable,before)),"Encode prior executable graph");
    require(!model.export_numeric_graph(executable,&error) && !error.isEmpty(),"Incompatible draft cannot execute");
    require(desktop::encode_numeric_graph(executable,after) && before==after,"Refused export preserves the prior executable graph");
    require(model.edit_node(QtNodes::NodeId(332),get(original,332)),"Restore the measured sample rate");
    original=model.numeric_graph();changed=get(original,332);
    put(changed,"mode",std::string{"existing_fir"});
    // The backend mode can change without affecting contracts. Check its
    // undoable round-trip separately, then restore the measured Speex backend.
    require(model.edit_node(QtNodes::NodeId(332),changed),
            "FIR backend can be selected without breaking data connections");
    original=model.numeric_graph();changed=get(original,332);
    put(changed,"mode",std::string{"speex"});
    require(model.edit_node(QtNodes::NodeId(332),changed),
            "Measured Speex backend can be restored without breaking links");
    require(model.numeric_graph().nodes.size()==numeric_startup_graph().nodes.size(),
            "Speex parameter edit preserves all nodes");
    const auto graph=model.numeric_graph();
    const auto source=std::find_if(graph.nodes.begin(),graph.nodes.end(),[](const auto& node){return node.id==332;});
    require(source!=graph.nodes.end(),"Resampler remains in edited example");
    std::size_t checked=0;
    for(const auto& edge:graph.edges)if(edge.from==332){
        const auto target=std::find_if(graph.nodes.begin(),graph.nodes.end(),[&](const auto& node){return node.id==edge.to;});
        require(target!=graph.nodes.end(),"Downstream node remains after reconfiguration");
        const auto out=std::find_if(source->outputs.begin(),source->outputs.end(),[&](const auto& port){return port.id==edge.output_port;});
        const auto in=std::find_if(target->inputs.begin(),target->inputs.end(),[&](const auto& port){return port.id==edge.input_port;});
        require(out!=source->outputs.end()&&in!=target->inputs.end(),"All resampler edges retain their ports");
        if(!domain::processing_ports_compatible(*out,*in))
            std::fprintf(stderr,"Speex edge mismatch %lld:%d -> %lld:%d\n",
                static_cast<long long>(edge.from),int(edge.output_port),static_cast<long long>(edge.to),int(edge.input_port));
        require(domain::processing_ports_compatible(*out,*in),"Changed sample-rate must propagate to connected inputs");
        ++checked;
    }
    require(checked>=3,"Resampler drives window, rate binding and line view");
}
int main(int argc,char** argv){QApplication app(argc,argv);
#ifdef _WIN32
    QFontDatabase::addApplicationFont(QDir(qEnvironmentVariable("WINDIR")).filePath("Fonts/segoeui.ttf"));app.setFont(QFont("Segoe UI",9));
#endif
    try{resampler_live_edit_regression();dialog_cadence_contract();explicit_preview_coordinates();loaded_frontend_baseline();typed_mismatch_messages();binary_coefficients();representations();profile_route_isolation();incompatible_routes();runtime_arrays(false);runtime_arrays(true);dialog_and_persistence(numeric_startup_graph());
    if(argc>=3 && QByteArray(argv[1])=="--source"){const auto path=QString::fromLocal8Bit(argv[2]);QFile file(path);require(file.open(QIODevice::ReadOnly),"open user config read-only");const auto original=file.readAll();file.close();desktop::NumericProjectDocument project;NumericProcessingGraph graph;
        require(bool(desktop::load_numeric_project(path,project,graph)),"read actual user graph");
        if(argc==4){desktop_analysis::offline::ProfileDocument loaded;QString error;require(desktop_analysis::offline::load_profile(QString::fromLocal8Bit(argv[3]),loaded,error),"read actual profile without scanning audio");
            const auto routes=frequency_profile_targets(graph);require(!routes.empty(),"actual graph route is visible independently of profile compatibility");
            for(const auto& route:routes)std::fprintf(stderr,"Actual route: %s\nCompatibility: %s\n",route.label.toUtf8().constData(),frequency_profile_incompatibility(route,loaded.profile).toUtf8().constData());}
        dialog_and_persistence(graph);require(file.open(QIODevice::ReadOnly)&&file.readAll()==original,"actual user config bytes remain unchanged");}
    std::puts("PASS normalization: four linear representations, measured batch statistics, numeric runtime, one-step Undo, project persistence");return 0;}catch(const std::exception& e){std::fprintf(stderr,"FAIL normalization: %s\n",e.what());return 1;}}
