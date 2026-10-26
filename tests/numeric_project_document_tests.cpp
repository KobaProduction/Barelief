#include <desktop/numeric_project_document.h>
#include "numeric_test_builders.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <bit>
#include <cstdio>
#include <limits>
#ifdef NUMERIC_CONFIG_MANAGER_TESTS
#include <core_app/config_manager.h>
#endif
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace desktop;
using namespace domain;
using namespace signal_contracts;
static bool check(bool valid,int line,const char* expression) {
    if(!valid)std::printf("FAIL %d %s\n",line,expression);
    return valid;
}
#define CHECK(x) if(!check(bool(x),__LINE__,#x)) return false
static ConverterProcessingParameters sample_converter_parameters() {
    ConverterProcessingParameters p;
    p.policy.scaling.gain.numerator=std::numeric_limits<std::uint64_t>::max();
    p.policy.scaling.offset.numerator=std::numeric_limits<std::int64_t>::min();
    p.policy.nan_replacement=std::bit_cast<float>(std::uint32_t{0x7fc00001});
    p.policy.negative_inf_replacement=std::bit_cast<double>(std::uint64_t{0x8000000000000000});
    return p;
}
static ProcessingPort sample_output(PortId id) {
    std::uint32_t channel=0;
    NumericContract c;c.dtype=static_cast<DType>(id);c.channel_order=std::span(&channel,1);c.representation.alignment=8;
    const auto require_owned=[](bool okay,const char*){if(!okay)std::abort();};
    return numeric_test_builders::port(id,PortDirection::Output,c,
        {64,1000000,16777216,16777216,1024,16777216},16777216,require_owned,"sample output contract");
}
static NumericProcessingGraph sample() {
    NumericProcessingGraph g;g.graph_id="main";
    ConfiguredProcessingNode n;n.id=7;n.kind="Converter";
    n.parameters=sample_converter_parameters();
    for(PortId id=0;id<10;++id)n.outputs.push_back(sample_output(id));
    g.nodes.push_back(n); return g;
}
static bool malformed_position_document(const QByteArray& extension,NumericProjectDocument& out) {
    QByteArray graph;
    CHECK(encode_numeric_graph(sample(),graph));
    graph.replace("\"kind\":\"Converter\"","\"kind\":\"Converter\",\"position\":{\"x\":\"invalid\"},"+extension);
    CHECK(read_numeric_project("{\"schema_version\":4,\"scene_graph\":"+graph+"}",out));
    return true;
}
static bool position_error_preserves_publication(const QByteArray& extension) {
    NumericProjectDocument source;
    CHECK(malformed_position_document(extension,source));
    NumericDraftPresentation out;
    out.editor_view=QJsonObject{{"sentinel",1}};
    CHECK(!read_numeric_presentation(source,out));
    CHECK(out.editor_view==QJsonObject({{"sentinel",1}}) && out.positions.isEmpty());
    return true;
}
static bool malformed_position_projection() {
    return position_error_preserves_publication("\"presentation\":{\"display_name\":\"valid\"}")
        && position_error_preserves_publication("\"adapter\":{\"host\":\"localhost\"}");
}
class ExactStorageScenario {
    NumericProcessingGraph original=sample(),copy;
    QByteArray snapshot;
    NumericProjectDocument baseline,edited;
    QTemporaryDir directory;
    QString path;
    bool integer_lexemes() const {
        CHECK(snapshot.contains("18446744073709551615"));
        CHECK(snapshot.contains("-9223372036854775808"));
        return true;
    }
    bool floating_bits() const {
        CHECK(snapshot.contains("7fc00001"));
        CHECK(snapshot.contains("8000000000000000"));
        return true;
    }
    bool encode_graph() {
        CHECK(encode_numeric_graph(original,snapshot));
        return integer_lexemes() && floating_bits();
    }
    bool decode_graph() {
        CHECK(decode_numeric_graph(snapshot,copy));
        CHECK(same_processing_parameters(original.nodes[0].parameters,copy.nodes[0].parameters));
        return port_contracts_identical();
    }
    bool port_contracts_identical() const {
        CHECK(copy.nodes[0].outputs.size()==10);
        for(std::size_t i=0;i<10;++i)
            CHECK(same_contract(original.nodes[0].outputs[i].contract.view(),copy.nodes[0].outputs[i].contract.view()));
        return true;
    }
    bool paste_original(ConfiguredProcessingNode& pasted) const {
        QByteArray node;
        CHECK(encode_numeric_node(original.nodes[0],node));
        CHECK(decode_numeric_node(node,pasted));
        return true;
    }
    bool clipboard_restore() const {
        ConfiguredProcessingNode pasted;
        CHECK(paste_original(pasted));
        const auto undo=pasted;
        std::get<ConverterProcessingParameters>(pasted.parameters).policy.scaling.gain.numerator=std::uint64_t{9};
        pasted=undo;
        CHECK(same_processing_parameters(original.nodes[0].parameters,pasted.parameters));
        return true;
    }
    bool edit_opaque_source() {
        // Independent opaque source oracle is inserted manually, not by the writer.
        snapshot.replace("\"parameters\":{","\"parameters\":{\"unknown\":123456789012345678901234567890,");
        const QByteArray project="{\"schema_version\":4,\"scene_graph\":"+snapshot+",\"ui\":{\"n\":9007199254740993},\"presets\":[{\"version\":1,\"id\":\"saved\",\"unknown\":1e9999}],\"pipeline_graph\":"+snapshot+",\"app\":-0}";
        CHECK(read_numeric_project(project,baseline));
        CHECK(read_numeric_draft(baseline,copy));
        std::get<ConverterProcessingParameters>(copy.nodes[0].parameters).policy.scaling.offset.exponent2=-123;
        CHECK(stage_numeric_draft(baseline,copy,edited));
        return true;
    }
    bool preserved_numeric_lexemes() const {
        const auto raw=edited.source.bytes();
        CHECK(raw.find("123456789012345678901234567890")!=raw.npos);
        CHECK(raw.find("9007199254740993")!=raw.npos);
        CHECK(raw.find("1e9999")!=raw.npos);
        return true;
    }
    bool preserved_negative_zero() const {
        const auto raw=edited.source.bytes();
        CHECK(raw.find("\"app\":-0")!=raw.npos);
        return true;
    }
    bool save_source() {
        CHECK(directory.isValid());
        path=directory.filePath("sub/config.json");
        CHECK(save_numeric_project(edited,path));
        return true;
    }
    bool reload_source() const {
        NumericProjectDocument reloaded;
        NumericProcessingGraph read;
        CHECK(load_numeric_project(path,reloaded,read));
        CHECK(same_processing_parameters(copy.nodes[0].parameters,read.nodes[0].parameters));
        CHECK(reloaded.active_path==edited.active_path);
        return true;
    }
    bool saved_bytes() const {
        QFile disk(path);
        CHECK(disk.open(QIODevice::ReadOnly));
        CHECK(disk.readAll()==edited.disk_baseline);
        return true;
    }
    bool write_external_change() const {
        QFile disk(path);
        CHECK(disk.open(QIODevice::WriteOnly));
        CHECK(disk.write("{\"schema_version\":999}")>0);
        disk.close();
        return true;
    }
    bool refuse_external_change() {
        CHECK(!save_numeric_project(edited));
        QFile disk(path);
        CHECK(disk.open(QIODevice::ReadOnly));
        CHECK(disk.readAll()=="{\"schema_version\":999}");
        return true;
    }
public:
    bool run() {
        return encode_graph() && decode_graph() && clipboard_restore() && edit_opaque_source()
            && preserved_numeric_lexemes() && preserved_negative_zero() && save_source()
            && reload_source() && saved_bytes() && write_external_change() && refuse_external_change();
    }
};
static bool exact_clipboard_and_file() {return ExactStorageScenario{}.run();}
class ReplacementConsentScenario {
    QTemporaryDir directory;
    QString path;
    NumericProjectDocument candidate;
    NumericSaveConsent consent;
    bool write(const QByteArray& bytes) const {
        QFile file(path);
        return file.open(QIODevice::WriteOnly) && file.write(bytes)==bytes.size();
    }
    QByteArray read() const {
        QFile file(path);
        return file.open(QIODevice::ReadOnly)?file.readAll():QByteArray{};
    }
    bool initialize() {
        CHECK(directory.isValid());
        path=directory.filePath("overwrite.json");
        CHECK(create_numeric_project(sample(),{},candidate));
        return true;
    }
    bool refuse_broken_destination() {
        CHECK(write("broken JSON"));
        CHECK(!save_numeric_project(candidate,path));
        CHECK(read()=="broken JSON");
        return true;
    }
    bool inspect_broken_destination() {
        CHECK(inspect_numeric_save_destination(path,consent));
        CHECK(consent.existed && consent.bytes=="broken JSON");
        return true;
    }
    bool confirm_broken_destination() {
        CHECK(save_numeric_project(candidate,path,&consent));
        NumericProjectDocument loaded;
        NumericProcessingGraph graph;
        CHECK(load_numeric_project(path,loaded,graph));
        return true;
    }
    bool change_after_confirmation() {
        CHECK(write("{\"schema_version\":999}"));
        CHECK(inspect_numeric_save_destination(path,consent));
        CHECK(write("changed during confirmation"));
        return true;
    }
    bool reject_changed_confirmation() {
        const auto before=candidate.disk_baseline;
        CHECK(!save_numeric_project(candidate,path,&consent));
        CHECK(read()=="changed during confirmation" && candidate.disk_baseline==before);
        return true;
    }
    bool reconfirm_destination() {
        CHECK(inspect_numeric_save_destination(path,consent));
        CHECK(save_numeric_project(candidate,path,&consent));
        NumericProjectDocument loaded;
        NumericProcessingGraph graph;
        CHECK(load_numeric_project(path,loaded,graph));
        return true;
    }
    bool reject_other_destination() {
        const auto other=directory.filePath("other.json");
        CHECK(!save_numeric_project(candidate,other,&consent));
        CHECK(!QFile::exists(other));
        return true;
    }
public:
    bool run() {
        return initialize() && refuse_broken_destination() && inspect_broken_destination()
            && confirm_broken_destination() && change_after_confirmation() && reject_changed_confirmation()
            && reconfirm_destination() && reject_other_destination();
    }
};
static bool confirmed_replacement() {return ReplacementConsentScenario{}.run();}
static bool binding_snapshot_fields(const NumericProcessingGraph& graph,QByteArray& bytes) {
    CHECK(encode_numeric_graph(graph,bytes));
    CHECK(bytes.contains("configuration_bindings"));
    CHECK(!bytes.contains("clock_node"));
    return true;
}
static bool binding_storage_roundtrip(const NumericProcessingGraph& graph,const QByteArray& bytes) {
    NumericProcessingGraph restored;
    CHECK(decode_numeric_graph(bytes,restored));
    CHECK(restored.configuration_bindings==graph.configuration_bindings);
    return true;
}
static bool unknown_binding_role_refused(const NumericProcessingGraph& graph,const QByteArray& bytes) {
    auto malformed=bytes;
    malformed.replace("frequency_grid","future_role");
    auto restored=graph;
    CHECK(!decode_numeric_graph(malformed,restored));
    CHECK(restored.configuration_bindings==graph.configuration_bindings);
    return true;
}
static bool explicit_binding_storage() {
    auto graph=sample();
    graph.configuration_bindings={{7,8,ConfigurationRole::FrequencyGrid,ConfigurationRole::FrequencyGrid}};
    QByteArray bytes;
    return binding_snapshot_fields(graph,bytes) && binding_storage_roundtrip(graph,bytes)
        && unknown_binding_role_refused(graph,bytes);
}
static bool rejected_project_inputs(NumericProjectDocument& out,const std::string& before) {
    for(auto bad:{"{\"schema_version\":1}","{\"schema_version\":2}","{\"schema_version\":4,\"schema_version\":4}","{\"schema_version\":3}","{\"nodes\":[],\"connections\":[]}","{\"schema_version\":4,\"ui\":{\"pipeline_presets\":[{\"version\":1,\"id\":\"future\",\"graph\":{\"version\":5,\"processing_version\":4}}]}}"}){
        CHECK(!read_numeric_project(bad,out)); CHECK(out.source.bytes()==before);
    }
    return true;
}
static bool duplicate_literal_refused(const NumericProcessingGraph& graph) {
    QByteArray bytes; CHECK(encode_numeric_node(graph.nodes[0],bytes)); auto saved=graph.nodes[0];
    bytes.replace("\"uint64\"","\"uint64\",\"dtype\":\"uint64\""); CHECK(!decode_numeric_node(bytes,saved)); CHECK(same_processing_parameters(saved.parameters,graph.nodes[0].parameters));
    return true;
}
static bool write_future_project(const QString& path) {
    QFile file(path);
    CHECK(file.open(QIODevice::WriteOnly));
    file.write("{\"schema_version\":99}");
    file.close();
    return true;
}
static bool future_destination_unchanged(NumericProjectDocument& document,const QString& path) {
    CHECK(!save_numeric_project(document,path));
    QFile file(path);
    CHECK(file.open(QIODevice::ReadOnly));
    CHECK(file.readAll()=="{\"schema_version\":99}");
    return true;
}
static bool protect_future_destination(NumericProjectDocument& document) {
    QTemporaryDir directory;
    const auto path=directory.filePath("future.json");
    return write_future_project(path) && future_destination_unchanged(document,path);
}
static bool refusal_and_output_rollback() {
    NumericProjectDocument staged,out;
    CHECK(read_numeric_project("{\"schema_version\":4,\"unknown\":18446744073709551615}",out));
    const auto before=std::string(out.source.bytes());
    if(!rejected_project_inputs(out,before))return false;
    const auto graph=sample();
    CHECK(stage_numeric_draft(out,graph,staged));
    return duplicate_literal_refused(graph) && protect_future_destination(staged);
}
static bool unsupported_schema_diagnostic(NumericProjectDocument& retained,const std::string& unchanged) {
    const auto result=read_numeric_project("{\"schema_version\":3}",retained);
    CHECK(!result && result.path=="schema_version");
    CHECK(result.error.contains("found 3") && result.error.contains("expected 4") && result.error.contains("graph 5, processing 4"));
    CHECK(retained.source.bytes()==unchanged);
    return true;
}
static bool unsupported_graph_diagnostic(const NumericDocumentResult& result,const NumericProjectDocument& retained,const std::string& unchanged) {
    CHECK(!result && result.path=="project.scene_graph");
    CHECK(result.error.contains("found document 4, graph 4, processing 3") && result.error.contains("expected 4/5/4"));
    CHECK(retained.source.bytes()==unchanged);
    return true;
}
static bool unsupported_graph_version(NumericProjectDocument& retained,const std::string& unchanged) {
    QByteArray graph;CHECK(encode_numeric_graph(sample(),graph));
    graph.replace("\"version\":5","\"version\":4");graph.replace("\"processing_version\":4","\"processing_version\":3");
    const auto result=read_numeric_project("{\"schema_version\":4,\"scene_graph\":"+graph+"}",retained);
    return unsupported_graph_diagnostic(result,retained,unchanged);
}
static bool unsupported_version_diagnostics() {
    NumericProjectDocument retained;
    CHECK(read_numeric_project("{\"schema_version\":4,\"opaque\":18446744073709551616}",retained));
    const auto unchanged=std::string(retained.source.bytes());
    return unsupported_schema_diagnostic(retained,unchanged) && unsupported_graph_version(retained,unchanged);
}
static void append_parameter_node(NumericProcessingGraph& graph,NodeId id,const std::string& kind,
    ProcessingParameters parameters,NodeId clock=-1) {
    ConfiguredProcessingNode node;
    node.id=id;
    node.kind=kind;
    node.parameters=std::move(parameters);
    node.clock_node=clock;
    graph.nodes.push_back(std::move(node));
}
static ReduceProcessingParameters selected_channel_reduction() {
    ReduceProcessingParameters reduction;
    reduction.selected_channels={0,4294967295u};
    reduction.channels=ReductionChannels::SelectedChannels;
    return reduction;
}
static SignalGeneratorProcessingParameters exact_tones() {
    SignalGeneratorProcessingParameters tones;
    tones.oscillators={{9007199254740993ULL,true,440.25,0.125,-0.5,0.25,0.75},
        {UINT64_MAX,false,220.5,0.25,1.5,0,1}};
    return tones;
}
static NumericProcessingGraph parameter_alternatives() {
    auto graph=sample();
    append_parameter_node(graph,8,"Spectrogram",HistoryProcessingParameters{18446744073709551615ULL});
    append_parameter_node(graph,9,"Gate",RouteProcessingParameters{2,1,false});
    append_parameter_node(graph,10,"ScalarReduce",selected_channel_reduction());
    append_parameter_node(graph,11,"TestSignal",EndpointProcessingParameters{{{"amplitude",NumericLiteral{18446744073709551615ULL}}}});
    append_parameter_node(graph,12,"SpectrumGain",TransformProcessingParameters{{{"gain",NumericLiteral{std::numeric_limits<std::int64_t>::min()}}}});
    append_parameter_node(graph,13,"SignalGenerator",exact_tones(),14);
    append_parameter_node(graph,14,"SampleClock",SampleClockProcessingParameters{44100,"local-exact"});
    append_parameter_node(graph,15,"Mixer",MixerProcessingParameters{MixerMode::WeightedRMS,{{3,0.25},{9,0.75}},125},14);
    return graph;
}
static bool parameter_alternatives_equal(const NumericProcessingGraph& expected,const NumericProcessingGraph& restored) {
    CHECK(expected.nodes.size()==restored.nodes.size());
    for(std::size_t i=0;i<expected.nodes.size();++i) {
        CHECK(same_processing_parameters(expected.nodes[i].parameters,restored.nodes[i].parameters));
        CHECK(restored.nodes[i].clock_node==-1);
    }
    return true;
}
static bool parameter_alternatives_roundtrip(const NumericProcessingGraph& graph) {
    QByteArray encoded;
    CHECK(encode_numeric_graph(graph,encoded));
    NumericProcessingGraph restored;
    CHECK(decode_numeric_graph(encoded,restored));
    return parameter_alternatives_equal(graph,restored);
}
static bool file_bytes_equal(const QString& path,const QByteArray& expected) {
    QFile file(path);
    CHECK(file.open(QIODevice::ReadOnly));
    CHECK(file.readAll()==expected);
    return true;
}
#ifdef _WIN32
static bool locked_save_refused(NumericProjectDocument& document,const QString& path) {
    const auto wide=path.toStdWString();
    const auto lock=CreateFileW(wide.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,
        nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    CHECK(lock!=INVALID_HANDLE_VALUE);
    const auto result=save_numeric_project(document);
    CloseHandle(lock);
    CHECK(!result);
    return true;
}
static bool commit_failure_preserves_document(NumericProjectDocument& document,const QString& path) {
    const auto baseline=document.disk_baseline;
    if(!locked_save_refused(document,path))return false;
    CHECK(document.disk_baseline==baseline);
    CHECK(document.active_path==path);
    if(!file_bytes_equal(path,baseline))return false;
    CHECK(save_numeric_project(document));
    return true;
}
#endif
static bool parameter_alternatives_commit(const NumericProcessingGraph& graph) {
    NumericProjectDocument source,staged;
    CHECK(read_numeric_project("{\"schema_version\":4,\"ui\":{\"unknown\":9007199254740993}}",source));
    CHECK(stage_numeric_draft(source,graph,staged));
    QTemporaryDir directory;
    const auto path=directory.filePath("config.json");
    CHECK(save_numeric_project(staged,path));
#ifdef _WIN32
    return commit_failure_preserves_document(staged,path);
#else
    return true;
#endif
}
static bool alternatives_and_commit_failure() {
    const auto graph=parameter_alternatives();
    return parameter_alternatives_roundtrip(graph) && parameter_alternatives_commit(graph);
}
class DraftAppliedScenario {
    NumericProjectDocument document,disk;
    NumericProcessingGraph draft=sample(),loaded;
    std::optional<NumericProcessingGraph> applied=sample();
    NumericDraftPresentation change;
    QTemporaryDir directory;
    QString path,active_path;
    QByteArray baseline;
    std::string source_before;
    bool initialize() {
        CHECK(read_numeric_project("{\"schema_version\":4}",document));
        CHECK(stage_numeric_draft(document,draft,document));
        return true;
    }
    bool initially_unapplied() {
        CHECK(read_numeric_applied(document,applied));
        CHECK(!applied);
        return true;
    }
    bool apply_draft() {
        change.applied_graph=draft;
        CHECK(stage_numeric_presentation(document,change,document));
        CHECK(read_numeric_applied(document,applied));
        CHECK(applied && applied->nodes[0].enabled);
        return true;
    }
    bool edit_draft() {
        draft.nodes[0].enabled=false;
        CHECK(stage_numeric_draft(document,draft,document));
        CHECK(read_numeric_draft(document,loaded));
        CHECK(!loaded.nodes[0].enabled);
        return true;
    }
    bool applied_unchanged() {
        CHECK(read_numeric_applied(document,applied));
        CHECK(applied && applied->nodes[0].enabled);
        return true;
    }
    bool save_and_reload() {
        CHECK(directory.isValid());
        path=directory.filePath("separate.json");
        CHECK(save_numeric_project(document,path));
        CHECK(load_numeric_project(path,disk,loaded));
        return true;
    }
    bool reloaded_draft_and_applied() {
        CHECK(!loaded.nodes[0].enabled);
        CHECK(read_numeric_applied(disk,applied));
        CHECK(applied && applied->nodes[0].enabled);
        return true;
    }
    bool remember_disk() {
        CHECK(same_processing_parameters(loaded.nodes[0].parameters,applied->nodes[0].parameters));
        source_before=std::string(disk.source.bytes());
        baseline=disk.disk_baseline;
        active_path=disk.active_path;
        return true;
    }
    bool publication_unchanged() const {
        CHECK(disk.disk_baseline==baseline && disk.active_path==active_path && disk.source.bytes()==source_before);
        return true;
    }
    bool combined_edit_refused() {
        change.applied_graph=draft;
        change.positions["999"]=QJsonObject{{"x",4}};
        CHECK(!stage_numeric_presentation(disk,change,disk));
        return publication_unchanged();
    }
    bool invalid_version_refused() {
        change.positions={};
        change.applied_graph->versions.processing=3;
        CHECK(!stage_numeric_presentation(disk,change,disk));
        CHECK(disk.source.bytes()==source_before);
        return true;
    }
    bool malformed_read_preserves_applied() {
        NumericProjectDocument malformed;
        CHECK(project_storage::parse_project_source("{\"schema_version\":4,\"pipeline_graph\":{\"version\":4}}",malformed.source));
        CHECK(!read_numeric_applied(malformed,applied));
        CHECK(applied && applied->nodes[0].enabled);
        return true;
    }
    bool missing_destination_refused() {
        CHECK(QFile::remove(path));
        CHECK(!save_numeric_project(disk));
        CHECK(!QFile::exists(path));
        return publication_unchanged();
    }
    bool independent_draft() {
        return initialize() && initially_unapplied() && apply_draft() && edit_draft() && applied_unchanged();
    }
    bool persisted_snapshot() {
        return save_and_reload() && reloaded_draft_and_applied() && remember_disk();
    }
public:
    bool run() {
        return independent_draft() && persisted_snapshot() && combined_edit_refused()
            && invalid_version_refused() && malformed_read_preserves_applied() && missing_destination_refused();
    }
};
static bool draft_applied_and_atomic_publication() {
    DraftAppliedScenario scenario;
    return scenario.run();
}
static NumericProcessingGraph connected_sample(PortId output_port,std::uint8_t conversion);
class EditorSnapshotScenario {
    NumericProcessingGraph graph=connected_sample(9,3),decoded;
    QByteArray canonical,first_node,second_node,encoded;
    QJsonObject visual;
    static QJsonObject visual_node(int id,const QByteArray& typed) {
        return {{"id",id},{"internal-data",QJsonObject{{"model-name","Converter"},{"numeric_processing",QString::fromUtf8(typed)}}}};
    }
    bool typed_snapshot() {
        CHECK(encode_numeric_graph(graph,canonical));
        CHECK(decode_numeric_editor_graph(canonical,decoded));
        CHECK(decoded.edges==graph.edges);
        return true;
    }
    bool encode_nodes() {
        CHECK(encode_numeric_node(graph.nodes[0],first_node));
        CHECK(encode_numeric_node(graph.nodes[1],second_node));
        return true;
    }
    bool visual_snapshot() {
        visual={{"version",5},{"processing_version",4},{"graph_id","main"},
            {"nodes",QJsonArray{visual_node(7,first_node),visual_node(19,second_node)}},
            {"connections",QJsonArray{QJsonObject{{"outNodeId",7},{"outPortIndex",9},{"inNodeId",19},{"inPortIndex",0},{"conversion",3}}}}};
        encoded=QJsonDocument(visual).toJson(QJsonDocument::Compact);
        CHECK(decode_numeric_editor_graph(encoded,decoded));
        CHECK(decoded.graph_id=="main" && decoded.nodes.size()==2 && decoded.edges==graph.edges);
        return true;
    }
    bool integer_policy_exact() const {
        const auto& policy=std::get<ConverterProcessingParameters>(decoded.nodes[0].parameters).policy;
        CHECK(std::get<std::uint64_t>(policy.scaling.gain.numerator)==18446744073709551615ULL);
        CHECK(std::get<std::int64_t>(policy.scaling.offset.numerator)==std::numeric_limits<std::int64_t>::min());
        return true;
    }
    bool float_policy_exact() const {
        const auto& policy=std::get<ConverterProcessingParameters>(decoded.nodes[0].parameters).policy;
        CHECK(std::bit_cast<std::uint32_t>(std::get<float>(policy.nan_replacement))==0x7fc00001u);
        CHECK(std::bit_cast<std::uint64_t>(std::get<double>(policy.negative_inf_replacement))==0x8000000000000000ULL);
        return true;
    }
    bool unchanged() const {
        return decoded.graph_id=="main" && decoded.nodes.size()==2 && decoded.edges==graph.edges
            && same_processing_parameters(decoded.nodes[0].parameters,graph.nodes[0].parameters);
    }
    bool refused(const QJsonObject& bad) {
        CHECK(!decode_numeric_editor_graph(QJsonDocument(bad).toJson(),decoded));
        CHECK(unchanged());
        return true;
    }
    bool versions_refused() {
        for(const auto field:{"version","processing_version"}) {
            auto bad=visual;
            bad[field]=99;
            if(!refused(bad))return false;
        }
        return true;
    }
    bool graph_identity_refused() {
        auto bad=visual;
        bad["graph_id"]="";
        return refused(bad);
    }
    bool refuse_nodes(const QJsonArray& nodes) {
        auto bad=visual;
        bad["nodes"]=nodes;
        return refused(bad);
    }
    bool node_id_mismatch() {
        auto nodes=visual["nodes"].toArray();
        nodes[0]=visual_node(8,first_node);
        return refuse_nodes(nodes);
    }
    bool duplicate_node_id() {
        auto nodes=visual["nodes"].toArray();
        nodes[1]=nodes[0];
        return refuse_nodes(nodes);
    }
    bool mixed_node_formats() {
        auto nodes=visual["nodes"].toArray();
        nodes[1]=QJsonDocument::fromJson(canonical)["nodes"].toArray()[1];
        return refuse_nodes(nodes);
    }
    bool duplicate_edge() {
        auto bad=visual;
        auto edges=bad["connections"].toArray();
        edges.append(edges[0]);
        bad["connections"]=edges;
        return refused(bad);
    }
    bool visual_snapshot_not_storage() const {
        NumericProjectDocument document;
        CHECK(read_numeric_project("{\"schema_version\":4}",document));
        CHECK(!read_numeric_project("{\"schema_version\":4,\"scene_graph\":"+encoded+"}",document));
        CHECK(document.source.bytes()=="{\"schema_version\":4}");
        return true;
    }
    bool visual_refusals() {
        return versions_refused() && graph_identity_refused() && node_id_mismatch()
            && duplicate_node_id() && mixed_node_formats() && duplicate_edge();
    }
public:
    bool run() {
        return typed_snapshot() && encode_nodes() && visual_snapshot() && integer_policy_exact()
            && float_policy_exact() && visual_refusals() && visual_snapshot_not_storage();
    }
};
static bool editor_snapshot_boundary() {
    EditorSnapshotScenario scenario;
    return scenario.run();
}
static NumericProcessingGraph connected_sample(PortId output_port=0,std::uint8_t conversion=0) {
    auto graph=sample();
    auto input=graph.nodes[0].outputs[0];
    input.direction=PortDirection::Input;
    graph.nodes[0].inputs.push_back(input);
    auto second=graph.nodes[0];
    second.id=19;
    graph.nodes.push_back(std::move(second));
    graph.edges.push_back({7,19,output_port,0,conversion});
    return graph;
}
class LayoutIdentityScenario {
    NumericProcessingGraph graph=connected_sample();
    NumericProjectDocument document;
    NumericDraftPresentation restored;
    bool initialize() {
        QByteArray snapshot;
        CHECK(encode_numeric_graph(graph,snapshot));
        snapshot.replace("\"kind\":\"Converter\"","\"kind\":\"Converter\",\"position\":{\"x\":12,\"y\":34},\"local_node_extension\":1e9999");
        snapshot.replace("\"out_id\":7","\"local_edge_extension\":9007199254740993,\"out_id\":7");
        CHECK(snapshot.contains("local_edge_extension"));
        CHECK(read_numeric_project("{\"schema_version\":4,\"scene_graph\":"+snapshot+"}",document));
        return true;
    }
    bool restore_positions() {
        CHECK(read_numeric_presentation(document,restored));
        CHECK(restored.positions.size()==2);
        return true;
    }
    bool foreign_identity_refused() {
        const auto before=std::string(document.source.bytes());
        auto foreign=graph;
        foreign.graph_id="foreign";
        CHECK(!stage_numeric_draft(document,foreign,document));
        CHECK(document.source.bytes()==before);
        return true;
    }
    bool replace_node_kind() {
        graph.nodes[0].kind="Spectrogram";
        graph.nodes[0].parameters=HistoryProcessingParameters{8};
        CHECK(stage_numeric_draft(document,graph,document));
        CHECK(read_numeric_presentation(document,restored));
        return true;
    }
    bool retained_identity_extensions() const {
        CHECK(!restored.positions.contains("7") && restored.positions.contains("19"));
        CHECK(document.source.bytes().find("local_edge_extension")!=std::string_view::npos);
        return true;
    }
    bool replace_edge_port() {
        graph.edges[0].output_port=1;
        CHECK(stage_numeric_draft(document,graph,document));
        CHECK(document.source.bytes().find("local_edge_extension")==std::string_view::npos);
        return true;
    }
    bool remove_node() {
        graph.nodes.erase(graph.nodes.begin()+1);
        graph.edges.clear();
        CHECK(stage_numeric_draft(document,graph,document));
        CHECK(read_numeric_presentation(document,restored));
        return true;
    }
    bool removed_identity_extensions() const {
        CHECK(restored.positions.isEmpty());
        CHECK(document.source.bytes().find("local_node_extension")==std::string_view::npos);
        return true;
    }
public:
    bool run() {
        return initialize() && restore_positions() && foreign_identity_refused() && replace_node_kind()
            && retained_identity_extensions() && replace_edge_port() && remove_node() && removed_identity_extensions();
    }
};
static bool retained_layout_identity_oracles() {
    LayoutIdentityScenario scenario;
    return scenario.run();
}
class SettingsArrayScenario {
    NumericProjectDocument document;
    NumericProcessingGraph graph=sample();
    NumericDraftPresentation change;
    bool initialize() {
        CHECK(read_numeric_project(R"({"schema_version":4,"ui":{"camera":{"position":[1.2300E+000,2,-0.000E+000],"opaque":1e9999},"graph_rendering":{"items":[{"id":"a","value":1.2300E+000,"opaque":7},{"id":"b","value":2,"opaque":9}]},"unsafe":[9007199254740993,2]}})",document));
        CHECK(stage_numeric_draft(document,graph,document));
        return true;
    }
    bool edit_arrays() {
        change.application_settings=QJsonObject{{"ui",QJsonObject{
            {"camera",QJsonObject{{"position",QJsonArray{1.23,3,0}}}},
            {"graph_rendering",QJsonObject{{"items",QJsonArray{QJsonObject{{"id","b"},{"value",4}},QJsonObject{{"id","a"},{"value",1.23}}}}}}}}};
        CHECK(stage_numeric_presentation(document,change,document));
        return true;
    }
    bool exact_opaque_values() const {
        const auto raw=document.source.bytes();
        CHECK(raw.find("[1.2300E+000,3,-0.000E+000]")!=raw.npos);
        CHECK(raw.find("\"opaque\":1e9999")!=raw.npos && raw.find("\"opaque\":7")!=raw.npos && raw.find("\"opaque\":9")!=raw.npos);
        return true;
    }
    bool identity_order_preserved() const {
        QJsonObject settings;
        CHECK(read_numeric_application_settings(document,settings));
        const auto items=settings["ui"].toObject()["graph_rendering"].toObject()["items"].toArray();
        CHECK(items[0].toObject()["id"]=="b" && items[0].toObject()["value"]==4);
        CHECK(items[1].toObject()["id"]=="a");
        return true;
    }
    bool unsafe_array_refused() {
        const auto before=std::string(document.source.bytes());
        change.application_settings=QJsonObject{{"ui",QJsonObject{{"theme","light"},{"unsafe",QJsonArray{0,2}}}}};
        CHECK(!stage_numeric_presentation(document,change,document));
        CHECK(document.source.bytes()==before);
        return true;
    }
#ifdef NUMERIC_CONFIG_MANAGER_TESTS
    bool manager_edit(ConfigManager& manager,FileSaveApplicationConfig& config) {
        CHECK(manager.readNumericSettings(document,config));
        config.ui.camera["position"]=QJsonArray{1.23,5,0};
        CHECK(manager.numericSettingsDelta(document,config,change.application_settings));
        CHECK(manager.saveNumeric(document,graph,change));
        return true;
    }
    bool manager_reload(ConfigManager& manager,FileSaveApplicationConfig& config) {
        CHECK(manager.loadNumeric(document,graph));
        CHECK(manager.readNumericSettings(document,config));
        CHECK(config.ui.camera["position"].toArray()[1]==5);
        return true;
    }
    bool manager_exact_lexemes() const {
        CHECK(document.source.bytes().find("[1.2300E+000,5,-0.000E+000]")!=std::string_view::npos);
        return true;
    }
    bool manager_roundtrip() {
        QTemporaryDir directory;
        CHECK(directory.isValid());
        ConfigManager manager(directory.filePath("vectors.json"));
        CHECK(manager.saveNumeric(document,graph));
        FileSaveApplicationConfig config;
        return manager_edit(manager,config) && manager_reload(manager,config) && manager_exact_lexemes();
    }
#endif
public:
    bool run() {
        if(!(initialize() && edit_arrays() && exact_opaque_values() && identity_order_preserved() && unsafe_array_refused()))return false;
#ifdef NUMERIC_CONFIG_MANAGER_TESTS
        return manager_roundtrip();
#else
        return true;
#endif
    }
};
static bool settings_arrays_and_opaque_refusal() {
    SettingsArrayScenario scenario;
    return scenario.run();
}
static bool snapshot_graphs_equal(const NumericProcessingGraph&,const NumericProcessingGraph&);
static bool inject_future_presentation(NumericProjectDocument&);
static bool future_presentation_preserved(const NumericProjectDocument&);
class NodeLayoutScenario {
    const NumericProcessingGraph& graph;
    const NumericProjectDocument& source;
    NumericProjectDocument document;
    NumericDraftPresentation presentation,restored;
    QJsonObject saved_positions;
    const QJsonObject labels{{"input0","Вход"},{"output65534","Моя частота"}};
    const QJsonArray input_order{1,0},output_order{65534,0};
    bool initialize() {
        CHECK(clone_numeric_project(source,document));
        presentation.node_presentations["7"]=QJsonObject{{"input_port_order",input_order},
            {"output_port_order",output_order},{"port_labels",labels}};
        CHECK(stage_numeric_presentation(document,presentation,document));
        CHECK(stage_numeric_draft(document,graph,document));
        return true;
    }
    bool verify_layout() {
        CHECK(read_numeric_presentation(document,restored));
        CHECK(restored.positions==saved_positions);
        const auto visual=restored.node_presentations["7"].toObject();
        CHECK(visual["input_port_order"]==input_order && visual["output_port_order"]==output_order && visual["port_labels"]==labels);
        return true;
    }
    bool clear_layout() {
        presentation.node_presentations["7"]=QJsonObject{{"input_port_order",QJsonArray{}},
            {"output_port_order",QJsonArray{}},{"port_labels",QJsonObject{}}};
        CHECK(stage_numeric_presentation(document,presentation,document));
        CHECK(read_numeric_presentation(document,restored));
        CHECK(restored.node_presentations["7"].toObject()["port_labels"].toObject().isEmpty());
        return true;
    }
public:
    NodeLayoutScenario(const NumericProcessingGraph& g,const NumericProjectDocument& d,const NumericDraftPresentation& p,const QJsonObject& positions)
        :graph(g),source(d),presentation(p),saved_positions(positions){}
    bool run(){return initialize() && verify_layout() && clear_layout();}
};
class StartupProjectScenario {
    const NumericProcessingGraph& graph;
    const NumericProjectDocument& loaded;
    NumericProjectDocument fresh;
    QJsonObject settings;
    QJsonArray presets;
    bool initialize() {
        CHECK(create_numeric_startup_project(graph,fresh));
        CHECK(fresh.active_path.isEmpty() && fresh.disk_baseline.isEmpty());
        return future_presentation_preserved(loaded);
    }
    bool read_settings() {
        CHECK(read_numeric_application_settings(fresh,settings));
        presets=settings["ui"].toObject()["pipeline_presets"].toArray();
        return true;
    }
    bool visible_defaults() const {
        for(const auto key:{"show_raw_samples","show_amplitudes","show_test_amplitudes","show_spectrogram"})
            CHECK(settings["ui"].toObject()[key]==true);
        return true;
    }
    bool preset_identity() const {
        CHECK(presets.size()==1 && presets[0].toObject()["id"]=="builtin-test-signal" && presets[0].toObject()["version"]==1);
        NumericProcessingGraph decoded;
        CHECK(decode_numeric_editor_graph(QJsonDocument(presets[0].toObject()["graph"].toObject()).toJson(),decoded));
        CHECK(decoded.graph_id==graph.graph_id);
        return true;
    }
    bool unapplied_candidate() const {
        std::optional<NumericProcessingGraph> applied=graph;
        CHECK(read_numeric_applied(fresh,applied));
        CHECK(!applied);
        return true;
    }
    bool invalid_graph_refused() {
        const auto before=std::string(fresh.source.bytes());
        auto invalid=graph;
        invalid.versions.graph=99;
        CHECK(!create_numeric_startup_project(invalid,fresh));
        CHECK(fresh.source.bytes()==before);
        return true;
    }
public:
    StartupProjectScenario(const NumericProcessingGraph& g,const NumericProjectDocument& d):graph(g),loaded(d){}
    bool run() {
        return initialize() && read_settings() && visible_defaults() && preset_identity()
            && unapplied_candidate() && invalid_graph_refused();
    }
};
class FreshProjectScenario {
    const NumericProcessingGraph graph=sample();
    NumericProcessingGraph decoded;
    NumericProjectDocument document;
    NumericDraftPresentation presentation,restored;
    const QJsonObject flags{{"inline_parameters",QJsonArray{"enabled","converter.overflow"}},
        {"stream_names",QJsonArray{"Generators","Loopback","Microphone"}}};
    bool initialize() {
        presentation.positions["7"]=QJsonObject{{"x",31},{"y",47}};
        presentation.node_presentations["7"]=flags;
        CHECK(create_numeric_project(graph,presentation,document));
        CHECK(document.active_path.isEmpty() && document.disk_baseline.isEmpty());
        return true;
    }
    bool initially_unapplied() const {
        std::optional<NumericProcessingGraph> applied=graph;
        CHECK(read_numeric_applied(document,applied));
        CHECK(!applied);
        return true;
    }
    bool initial_projection() {
        CHECK(read_numeric_presentation(document,restored));
        CHECK(restored.positions==presentation.positions && restored.node_presentations==presentation.node_presentations);
        return true;
    }
    bool initial_draft() {
        CHECK(read_numeric_draft(document,decoded));
        CHECK(same_processing_parameters(graph.nodes[0].parameters,decoded.nodes[0].parameters));
        return snapshot_graphs_equal(graph,decoded);
    }
    bool graph_excludes_ui() const {
        QByteArray snapshot;
        CHECK(encode_numeric_graph(decoded,snapshot));
        CHECK(!snapshot.contains("inline_parameters"));
        return true;
    }
    bool edit_flags() {
        if(!inject_future_presentation(document))return false;
        presentation.positions={};
        presentation.node_presentations["7"]=QJsonObject{{"inline_parameters",QJsonArray{"enabled"}},{"stream_names",flags["stream_names"]}};
        CHECK(stage_numeric_presentation(document,presentation,document));
        return future_presentation_preserved(document);
    }
    bool draft_retains_flags() {
        CHECK(stage_numeric_draft(document,graph,document));
        CHECK(read_numeric_presentation(document,restored));
        CHECK(restored.node_presentations==presentation.node_presentations);
        return true;
    }
    bool independent_layout() const {
        NodeLayoutScenario scenario(graph,document,presentation,restored.positions);
        return scenario.run();
    }
    bool invalid_inline_flags() {
        const auto before=std::string(document.source.bytes());
        for(const auto& invalid:{QJsonArray{"enabled","enabled"},QJsonArray{42},QJsonArray{""}}) {
            presentation.node_presentations["7"]=QJsonObject{{"inline_parameters",invalid}};
            CHECK(!stage_numeric_presentation(document,presentation,document));
            CHECK(document.source.bytes()==before);
        }
        return true;
    }
    bool unknown_node_refused() {
        const auto before=std::string(document.source.bytes());
        presentation.node_presentations={};
        presentation.node_presentations["999"]=flags;
        CHECK(!stage_numeric_presentation(document,presentation,document));
        CHECK(document.source.bytes()==before);
        return true;
    }
    bool store_flags() {
#ifdef NUMERIC_CONFIG_MANAGER_TESTS
        QTemporaryDir directory;
        CHECK(directory.isValid());
        ConfigManager manager(directory.filePath("flags.json"));
        CHECK(manager.saveNumeric(document,graph,presentation));
        CHECK(manager.loadNumeric(document,decoded));
#else
        CHECK(stage_numeric_presentation(document,presentation,document));
#endif
        return true;
    }
    bool empty_inline_flags() {
        presentation.node_presentations={};
        presentation.node_presentations["7"]=QJsonObject{{"inline_parameters",QJsonArray{}},{"stream_names",flags["stream_names"]}};
        return store_flags();
    }
    bool restored_flags() {
        CHECK(read_numeric_presentation(document,restored));
        CHECK(restored.node_presentations==presentation.node_presentations);
        return future_presentation_preserved(document);
    }
    bool startup_independence() const {
        StartupProjectScenario scenario(graph,document);
        return scenario.run();
    }
    bool initial_snapshot() {
        return initialize() && initially_unapplied() && initial_projection() && initial_draft() && graph_excludes_ui();
    }
    bool presentation_edits() {
        return edit_flags() && draft_retains_flags() && independent_layout() && invalid_inline_flags()
            && unknown_node_refused() && empty_inline_flags() && restored_flags();
    }
public:
    bool run(){return initial_snapshot() && presentation_edits() && startup_independence();}
};
static bool fresh_project_and_node_presentation() {
    FreshProjectScenario scenario;
    return scenario.run();
}
static bool snapshot_graphs_equal(const NumericProcessingGraph& expected,const NumericProcessingGraph& actual) {
    QByteArray before,after;
    CHECK(encode_numeric_graph(expected,before));
    CHECK(encode_numeric_graph(actual,after));
    CHECK(before==after);
    return true;
}
static bool inject_future_presentation(NumericProjectDocument& document) {
    auto raw=QByteArray(document.source.bytes().data(),qsizetype(document.source.bytes().size()));
    raw.replace("\"presentation\":{","\"presentation\":{\"future\":18446744073709551616,");
    CHECK(read_numeric_project(raw,document));
    return true;
}
static bool future_presentation_preserved(const NumericProjectDocument& document) {
    CHECK(document.source.bytes().find("18446744073709551616")!=std::string_view::npos);
    return true;
}
class NodeDisplayNameScenario {
    const NumericProcessingGraph graph=sample();
    NumericProjectDocument document,reloaded;
    NumericProcessingGraph draft;
    NumericDraftPresentation delta,projected;
    QTemporaryDir directory;
    QString path;
    const QString name=QString::fromUtf8("Мой преобразователь");
    QJsonObject view() const {return projected.node_presentations["7"].toObject();}
    bool stage_and_project(NumericProjectDocument& source) {
        CHECK(stage_numeric_presentation(source,delta,source));
        CHECK(read_numeric_presentation(source,projected));
        return true;
    }
    bool initialize() {
        delta.node_presentations["7"]=QJsonObject{{"inline_parameters",QJsonArray{"converter.overflow"}}};
        CHECK(create_numeric_project(graph,delta,document));
        return inject_future_presentation(document);
    }
    bool set_name() {
        delta.node_presentations["7"]=QJsonObject{{"display_name",name}};
        return stage_and_project(document);
    }
    bool named_projection() const {
        CHECK(view().value("display_name")==name);
        CHECK(view().value("inline_parameters").toArray()==QJsonArray{"converter.overflow"});
        return future_presentation_preserved(document);
    }
    bool processing_unchanged() {
        CHECK(read_numeric_draft(document,draft));
        return snapshot_graphs_equal(graph,draft);
    }
    bool maximum_name() {
        delta.node_presentations["7"]=QJsonObject{{"display_name",QString(256,QChar(0x0416))}};
        if(!stage_and_project(document))return false;
        CHECK(view().value("display_name").toString()==QString(256,QChar(0x0416)));
        return true;
    }
    bool invalid_names_refused() {
        const auto before=std::string(document.source.bytes());
        for(const QJsonValue& invalid:{QJsonValue(42),QJsonValue(QString(257,'x')),QJsonValue(QString("bad")+QChar(1))}) {
            delta.node_presentations["7"]=QJsonObject{{"display_name",invalid}};
            CHECK(!stage_numeric_presentation(document,delta,document));
            CHECK(document.source.bytes()==before);
        }
        return true;
    }
    bool scalar_labels() {
        QJsonObject labels{
            {"0",QJsonObject{{"display_name","Count"},{"enabled",false}}},
            {"3",QJsonObject{{"display_name","Peak"},{"enabled",true}}}};
        for(unsigned id=0;id<12;++id)if(!labels.contains(QString::number(id)))
            labels[QString::number(id)]=QJsonObject{{"display_name",QString("Metric %1").arg(id)},{"enabled",true}};
        delta.node_presentations["7"]=QJsonObject{{"scalar_inputs",labels}};
        if(!stage_and_project(document))return false;
        CHECK(view().value("scalar_inputs").toObject().size()==12);
        CHECK(view().value("scalar_inputs").toObject().value("0").toObject().value("enabled")==false);
        return true;
    }
    bool replace_scalar_labels() {
        delta.node_presentations["7"]=QJsonObject{{"scalar_inputs",QJsonObject{{"3",QJsonObject{{"display_name","Peak"},{"enabled",true}}}}}};
        if(!stage_and_project(document))return false;
        CHECK(!view().value("scalar_inputs").toObject().contains("0"));
        return true;
    }
    bool empty_name() {
        delta.node_presentations["7"]=QJsonObject{{"display_name",""}};
        if(!stage_and_project(document))return false;
        CHECK(view().value("display_name").isString() && view().value("display_name").toString().isEmpty());
        return true;
    }
    bool save_and_reload() {
        CHECK(directory.isValid());
        path=directory.filePath("named.json");
        CHECK(save_numeric_project(document,path));
        CHECK(load_numeric_project(path,reloaded,draft));
        return true;
    }
    bool reloaded_name() {
        CHECK(read_numeric_presentation(reloaded,projected));
        CHECK(view().value("display_name").toString().isEmpty());
        return future_presentation_preserved(reloaded);
    }
    bool save_dc_flag(bool shown) {
        delta.node_presentations["7"]=QJsonObject{{"show_dc_outputs",shown}};
        CHECK(stage_numeric_presentation(reloaded,delta,reloaded));
        CHECK(save_numeric_project(reloaded,path));
        return true;
    }
    bool read_dc_flag(bool shown,NumericProjectDocument& copy) {
        CHECK(load_numeric_project(path,copy,draft));
        CHECK(read_numeric_presentation(copy,projected));
        CHECK(view().value("show_dc_outputs")==shown);
        return future_presentation_preserved(copy);
    }
    bool dc_roundtrip() {
        for(bool shown:{true,false}) {
            if(!save_dc_flag(shown))return false;
            NumericProjectDocument copy;
            if(!read_dc_flag(shown,copy))return false;
        }
        return true;
    }
    bool invalid_dc_flags() {
        const auto before=std::string(reloaded.source.bytes());
        for(const QJsonValue& invalid:{QJsonValue(1),QJsonValue("true"),QJsonValue(QJsonArray{})}) {
            delta.node_presentations["7"]=QJsonObject{{"show_dc_outputs",invalid}};
            CHECK(!stage_numeric_presentation(reloaded,delta,reloaded));
            CHECK(reloaded.source.bytes()==before);
        }
        return true;
    }
    bool names_and_fields() {
        return initialize() && set_name() && named_projection() && processing_unchanged()
            && maximum_name() && invalid_names_refused() && scalar_labels() && replace_scalar_labels() && empty_name();
    }
public:
    bool run() {
        return names_and_fields() && save_and_reload() && reloaded_name() && dc_roundtrip() && invalid_dc_flags();
    }
};
static bool node_display_name_persistence() {
    NodeDisplayNameScenario scenario;
    return scenario.run();
}
class PresetDisplayScenario {
    const NumericProcessingGraph graph=sample();
    QByteArray snapshot;
    const QByteArray display=R"({"version":1,"show_raw_samples":true,"show_amplitudes":false,"show_spectrogram":false,"show_data_flow":true,"graph_views":{"fft":{"open":true,"mode":"3d","opaque":9007199254740993}},"graph_rendering":{"version":1,"defaults":{"axis_precision":4},"future":1e9999},"workspace":{"version":1,"layout_version":5,"backend":"qt-ads","ads_state":"saved-tabs","geometry":"saved-geometry","visibility":{"fft":true,"dataFlow":true},"opaque":18446744073709551616},"opaque":1e9999})";
    NumericProjectDocument source;
    QJsonObject projected,settings,preset,restored;
    QJsonArray presets;
    NumericDraftPresentation delta;
    bool initialize_projection() {
        CHECK(encode_numeric_graph(graph,snapshot));
        CHECK(decode_numeric_preset_presentation(display,projected));
        CHECK(projected["show_raw_samples"]==true && projected["show_amplitudes"]==false);
        return true;
    }
    bool projection_excludes_opaque() const {
        CHECK(!projected.contains("opaque"));
        CHECK(!projected["graph_views"].toObject()["fft"].toObject().contains("opaque"));
        CHECK(projected["workspace"].toObject()["ads_state"]=="saved-tabs");
        return true;
    }
    bool initialize_project() {
        const auto raw="{\"schema_version\":4,\"scene_graph\":"+snapshot+",\"pipeline_graph\":"+snapshot
            +",\"ui\":{\"pipeline_presets\":[{\"version\":1,\"id\":\"scene\",\"name\":\"Scene\",\"graph\":"+snapshot+",\"presentation\":"+display+"}]}}";
        CHECK(read_numeric_project(raw,source));
        CHECK(read_numeric_application_settings(source,settings));
        presets=settings["ui"].toObject()["pipeline_presets"].toArray();
        CHECK(presets.size()==1);
        return true;
    }
    void clear_preset_display() {
        preset=presets[0].toObject();
        auto saved=preset["presentation"].toObject();
        saved["show_raw_samples"]=false;
        saved["graph_views"]=QJsonObject{};
        saved["graph_rendering"]=QJsonObject{};
        saved["workspace"]=QJsonObject{};
        preset["presentation"]=saved;
        presets[0]=preset;
        delta.application_settings=QJsonObject{{"ui",QJsonObject{{"pipeline_presets",presets}}}};
    }
    bool stage_preset() {
        clear_preset_display();
        CHECK(stage_numeric_presentation(source,delta,source));
        CHECK(read_numeric_application_settings(source,settings));
        restored=settings["ui"].toObject()["pipeline_presets"].toArray()[0].toObject()["presentation"].toObject();
        return true;
    }
    bool cleared_display() const {
        CHECK(restored["show_raw_samples"]==false && !restored["graph_views"].toObject().contains("fft"));
        CHECK(!restored["graph_rendering"].toObject().contains("defaults") && !restored["workspace"].toObject().contains("ads_state"));
        return true;
    }
    bool retained_opaque_source() const {
        for(const auto lexeme:{"1e9999","18446744073709551616"})
            CHECK(source.source.bytes().find(lexeme)!=std::string_view::npos);
        return true;
    }
    bool retained_applied_graph() const {
        std::optional<NumericProcessingGraph> applied;
        CHECK(read_numeric_applied(source,applied));
        CHECK(applied && same_processing_parameters(applied->nodes[0].parameters,graph.nodes[0].parameters));
        return true;
    }
    bool write_project(const QString& path) {
        CHECK(save_numeric_project(source,path));
        return true;
    }
    bool reload_project(const QString& path) {
        NumericProjectDocument reloaded;
        NumericProcessingGraph draft;
        CHECK(load_numeric_project(path,reloaded,draft));
        CHECK(read_numeric_application_settings(reloaded,settings));
        CHECK(settings["ui"].toObject()["pipeline_presets"].toArray()[0].toObject()["presentation"]==restored);
        return true;
    }
    bool file_roundtrip() {
        QTemporaryDir directory;
        CHECK(directory.isValid());
        const auto path=directory.filePath("display.json");
        return write_project(path) && reload_project(path);
    }
    bool invalid_projections() {
        const auto before=projected;
        for(const auto& invalid:{QByteArray("{\"version\":99}"),QByteArray("{\"version\":1,\"version\":1}"),
            QByteArray("{\"version\":1,\"show_spectrogram\":0}"),QByteArray("{\"version\":1,\"workspace\":[]}"),
            QByteArray("{\"version\":1,\"workspace\":{\"layout_version\":1.5}}"),
            QByteArray("{\"version\":1,\"workspace\":{\"visibility\":{\"fft\":0}}}"),
            QByteArray("{\"version\":1,\"graph_views\":{\"fft\":{\"open\":1}}}")}) {
            CHECK(!decode_numeric_preset_presentation(invalid,projected));
            CHECK(projected==before);
        }
        return true;
    }
    bool invalid_preset_refused() {
        const auto before=std::string(source.source.bytes());
        preset["presentation"]=QJsonObject{{"version",99}};
        presets[0]=preset;
        delta.application_settings=QJsonObject{{"ui",QJsonObject{{"pipeline_presets",presets}}}};
        CHECK(!stage_numeric_presentation(source,delta,source));
        CHECK(source.source.bytes()==before);
        return true;
    }
public:
    bool run() {
        return initialize_projection() && projection_excludes_opaque() && initialize_project() && stage_preset()
            && cleared_display() && retained_opaque_source() && retained_applied_graph()
            && file_roundtrip() && invalid_projections() && invalid_preset_refused();
    }
};
static bool preset_display_snapshot_roundtrip() {
    PresetDisplayScenario scenario;
    return scenario.run();
}
class NodeAdapterScenario {
    const NumericProcessingGraph graph=sample();
    const QByteArray raw=R"({"provider_uuid":"11111111-1111-4111-8111-111111111111","stream_id":"mel-main","host":"stale-host","port":8213,"device_identity":"numeric-desktop","remote_endpoint":{"graph_id":"publisher","node_id":19,"direction":"output","port_index":0,"opaque":9007199254740993},"opaque":1e9999})";
    const QJsonObject direct{{"host","127.0.0.1"},{"port",8214},{"remote_endpoint",QJsonObject{{"graph_id","direct"},{"node_id",4},{"direction","input"},{"port_index",1}}}};
    QJsonObject adapter;
    NumericDraftPresentation change,restored;
    NumericProjectDocument document;
    NumericProcessingGraph processing;
    QByteArray before,after;
    std::string unchanged;
    bool decode_adapter() {
        CHECK(decode_numeric_node_adapter(raw,adapter));
        CHECK(!adapter.contains("opaque")&&!adapter["remote_endpoint"].toObject().contains("opaque"));return true;
    }
    bool create_project() {
        change.node_adapters["7"]=adapter;CHECK(create_numeric_project(graph,change,document));
        auto source=QByteArray(document.source.bytes().data(),qsizetype(document.source.bytes().size()));
        source.replace(QJsonDocument(adapter).toJson(QJsonDocument::Compact),raw);
        CHECK(read_numeric_project(source,document));return true;
    }
    bool read_adapter() {
        CHECK(read_numeric_presentation(document,restored));CHECK(restored.node_adapters==change.node_adapters);return true;
    }
    bool read_processing() {
        CHECK(read_numeric_draft(document,processing));CHECK(encode_numeric_graph(graph,before));return true;
    }
    bool processing_excludes_adapter() {
        CHECK(encode_numeric_graph(processing,after));CHECK(before==after&&!after.contains("provider_uuid"));return true;
    }
    bool direct_replacement() {
        // A full replacement clears provider priority, not just its host.
        change.node_adapters["7"]=direct;CHECK(stage_numeric_presentation(document,change,document));
        CHECK(read_numeric_presentation(document,restored));CHECK(restored.node_adapters["7"]==direct);return true;
    }
    bool opaque_fields_retained() const {
        CHECK(document.source.bytes().find("1e9999")!=std::string_view::npos&&document.source.bytes().find("9007199254740993")!=std::string_view::npos);
        return true;
    }
    bool provider_priority_removed() {
        CHECK(document.source.bytes().find("provider_uuid")==std::string_view::npos);
        unchanged=std::string(document.source.bytes());return opaque_fields_retained();
    }
    bool invalid_adapter(const QByteArray& bytes,const QJsonObject& baseline) {
        CHECK(!decode_numeric_node_adapter(bytes,adapter));CHECK(adapter==baseline);return true;
    }
    bool invalid_adapter_inputs() {
        const auto baseline=adapter;
        for(const auto& bytes:{QByteArray("{\"provider_uuid\":\"bad\"}"),QByteArray("{\"host\":\"\"}"),QByteArray("{\"port\":0}"),QByteArray("{\"port\":65536}"),QByteArray("{\"port\":1,\"port\":2}"),QByteArray("{\"remote_endpoint\":{\"node_id\":2147483648}}"),QByteArray("{\"remote_endpoint\":{\"direction\":\"invalid\"}}")})
            if(!invalid_adapter(bytes,baseline))return false;
        return true;
    }
    bool invalid_delta(const QJsonObject& adapter) {
        change.node_adapters["7"]=adapter;CHECK(!stage_numeric_presentation(document,change,document));
        CHECK(document.source.bytes()==unchanged);return true;
    }
    bool partial_replacement() {
        change.node_adapters["7"]=QJsonObject{{"host","partial-peer"}};CHECK(stage_numeric_presentation(document,change,document));
        CHECK(read_numeric_presentation(document,restored));CHECK(restored.node_adapters["7"]==change.node_adapters["7"]);return true;
    }
    bool empty_replacement() {
        change.node_adapters["7"]=QJsonObject{};CHECK(stage_numeric_presentation(document,change,document));
        CHECK(read_numeric_presentation(document,restored));CHECK(restored.node_adapters["7"].toObject().isEmpty());return true;
    }
#ifdef NUMERIC_CONFIG_MANAGER_TESTS
    QTemporaryDir directory;
    ConfigManager manager{directory.filePath("routing.json")};
    bool manager_save() {
        CHECK(directory.isValid());CHECK(manager.saveNumeric(document,graph));
        change.node_adapters["7"]=direct;CHECK(manager.saveNumeric(document,graph,change));return true;
    }
    bool manager_reload() {
        CHECK(manager.loadNumeric(document,processing));CHECK(read_numeric_presentation(document,restored));
        CHECK(restored.node_adapters["7"]==direct);return true;
    }
#endif
public:
    bool run() {
        const bool complete=decode_adapter()&&create_project()&&read_adapter()&&read_processing()&&processing_excludes_adapter()
            &&direct_replacement()&&provider_priority_removed()&&invalid_adapter_inputs()
            &&invalid_delta(QJsonObject{{"port",0}})&&invalid_delta(QJsonObject{{"host","peer"},{"unregistered",true}})
            &&partial_replacement()&&empty_replacement()&&opaque_fields_retained();
#ifdef NUMERIC_CONFIG_MANAGER_TESTS
        return complete&&manager_save()&&manager_reload();
#else
        return complete;
#endif
    }
};
static bool node_adapter_storage_and_projection() {NodeAdapterScenario scenario;return scenario.run();}
class PresentationVerticalScenario {
    NumericProjectDocument settings_only,base,typed,layout,disk;
    std::uint32_t edition=99;
    NumericProcessingGraph graph=sample(),restored_graph;
    NumericDraftPresentation change,read,preferences;
    QJsonObject settings;
    QTemporaryDir directory;
    QString path=directory.filePath("layout.json");
    bool settings_without_graph() {
        CHECK(read_numeric_project("{\"schema_version\":4,\"ui\":{\"theme\":\"dark\"}}",settings_only));
        CHECK(!read_project_graph_edition(settings_only,edition));CHECK(edition==99);return true;
    }
    bool initialize_typed_project() {
        CHECK(read_numeric_project(R"({"schema_version":4,"ui":{"unknown":9007199254740993,"editor_view":{"version":1,"scale":1.2300E+000,"opaque":1e9999}}})",base));
        CHECK(stage_numeric_draft(base,graph,typed));
        change.positions["7"]=QJsonObject{{"x",17.5},{"y",-28.25}};
        change.editor_view=QJsonObject{{"version",1},{"scale",1.5},{"center_x",11.0},{"center_y",12.0}};return true;
    }
    bool stage_layout() {
        CHECK(stage_numeric_presentation(typed,change,layout));CHECK(read_numeric_presentation(layout,read));CHECK(read.positions==change.positions);return true;
    }
    bool layout_and_opaque_retained() const {
        CHECK(read.editor_view==change.editor_view);CHECK(layout.source.bytes().find("9007199254740993")!=std::string_view::npos);
        CHECK(layout.source.bytes().find("1e9999")!=std::string_view::npos);return true;
    }
    bool invalid_layout_refused() {
        NumericDraftPresentation invalid;invalid.positions["7"]=QJsonObject{{"hidden-field",7}};
        const auto before=std::string(layout.source.bytes());CHECK(!stage_numeric_presentation(layout,invalid,layout));
        CHECK(layout.source.bytes()==before);return true;
    }
    bool stage_preferences() {
        preferences.application_settings=QJsonObject{{"ui",QJsonObject{{"theme","dark"},{"camera",QJsonObject{{"zoom",2.0}}}}}};
        CHECK(stage_numeric_presentation(layout,preferences,layout));CHECK(read_numeric_application_settings(layout,settings));return true;
    }
    bool preferences_and_opaque_retained() const {
        CHECK(settings["ui"].toObject()["theme"]=="dark");CHECK(settings["ui"].toObject()["camera"].toObject()["zoom"]==2.0);
        CHECK(layout.source.bytes().find("1e9999")!=std::string_view::npos);return true;
    }
    bool unregistered_preference_refused() {
        preferences.application_settings=QJsonObject{{"ui",QJsonObject{{"unknown",0}}}};const auto before=std::string(layout.source.bytes());
        CHECK(!stage_numeric_presentation(layout,preferences,layout));CHECK(layout.source.bytes()==before);return true;
    }
    bool file_roundtrip() {
        CHECK(save_numeric_project(layout,path));CHECK(load_numeric_project(path,disk,restored_graph));return true;
    }
    bool file_layout_restored() {
        CHECK(read_numeric_presentation(disk,read));CHECK(read.positions==change.positions);return true;
    }
#ifdef NUMERIC_CONFIG_MANAGER_TESTS
    ConfigManager manager{path};
    FileSaveApplicationConfig config;
    QJsonObject delta;
    bool manager_edit_layout() {
        change.positions["7"]=QJsonObject{{"x",29.0},{"y",41.0}};CHECK(manager.saveNumeric(disk,restored_graph,change));
        CHECK(manager.loadNumeric(disk,restored_graph));CHECK(read_numeric_presentation(disk,read));return true;
    }
    bool manager_settings_loaded() {
        CHECK(read.positions==change.positions);CHECK(manager.readNumericSettings(disk,config));CHECK(config.ui.theme=="dark");return true;
    }
    bool settings_delta() {
        CHECK(manager.numericSettingsDelta(disk,config,delta));CHECK(delta.isEmpty());config.ui.theme="light";
        CHECK(manager.numericSettingsDelta(disk,config,delta));return true;
    }
    bool save_settings_delta() {
        change.application_settings=delta;CHECK(manager.saveNumeric(disk,restored_graph,change));
        CHECK(manager.readNumericSettings(disk,config));CHECK(config.ui.theme=="light");return true;
    }
    bool save_preset_name(const QString& name) {
        config.ui.pipeline_presets=QJsonArray{QJsonObject{{"version",1},{"id","stable"},{"name",name}}};
        CHECK(manager.numericSettingsDelta(disk,config,delta));change.application_settings=delta;
        CHECK(manager.saveNumeric(disk,restored_graph,change));return true;
    }
    bool preset_name_restored() {
        CHECK(manager.readNumericSettings(disk,config));CHECK(config.ui.pipeline_presets[0].toObject()["name"]=="changed");
        change.application_settings={};return true;
    }
    bool replace_existing_destination() {
        const auto other=directory.filePath("existing.json");QFile target(other);CHECK(target.open(QIODevice::WriteOnly));
        target.write("{\"schema_version\":4,\"old\":1}");target.close();
        CHECK(manager.saveNumericAs(disk,restored_graph,change,other));CHECK(manager.getConfigPath()==other);return true;
    }
    bool manager_roundtrip() {
        return manager_edit_layout()&&manager_settings_loaded()&&settings_delta()&&save_settings_delta()
            &&save_preset_name("explicit")&&save_preset_name("changed")&&preset_name_restored()&&replace_existing_destination();
    }
#endif
public:
    bool run() {
        const bool complete=settings_without_graph()&&initialize_typed_project()&&stage_layout()&&layout_and_opaque_retained()
            &&invalid_layout_refused()&&stage_preferences()&&preferences_and_opaque_retained()&&unregistered_preference_refused()
            &&file_roundtrip()&&file_layout_restored();
#ifdef NUMERIC_CONFIG_MANAGER_TESTS
        return complete&&manager_roundtrip();
#else
        return complete;
#endif
    }
};
static bool presentation_vertical() {PresentationVerticalScenario scenario;return scenario.run();}
#ifdef NUMERIC_CONFIG_MANAGER_TESTS
class PresetVerticalScenario {
    QTemporaryDir directory;
    const QString path=directory.filePath("presets.json"),target=directory.filePath("presets-as.json");
    ConfigManager manager{path};
    QString error;
    NumericProcessingGraph graph=sample();
    NumericProjectDocument document;
    QByteArray typed,opaque_graph,qt_bytes,source,edited_snapshot,disk_before;
    QJsonObject saved_graph,first,retained,reloaded_node;
    const QJsonObject preset_flags{{"inline_parameters",QJsonArray{"enabled"}}};
    FileSaveApplicationConfig settings;
    NumericDraftPresentation change;
    std::string unchanged;
    bool initialize_document() {
        CHECK(directory.isValid());CHECK(read_numeric_project("{\"schema_version\":4,\"ui\":{\"opaque\":1e9999}}",document));
        CHECK(manager.saveNumeric(document,graph,&error));return true;
    }
    bool initialize_saved_graph() {
        CHECK(encode_numeric_node(graph.nodes[0],typed));
        saved_graph=QJsonObject{{"version",5},{"processing_version",4},{"graph_id","main"},{"nodes",QJsonArray{QJsonObject{{"id",7},{"internal-data",QJsonObject{{"model-name","Converter"},{"numeric_processing",QString::fromUtf8(typed)},{"presentation",preset_flags}}},{"position",QJsonObject{{"x",11.0},{"y",12.0}}}}}},{"connections",QJsonArray{}}};
        return true;
    }
    QJsonObject preset(const QString& id,const QString& name) const {
        return QJsonObject{{"version",1},{"id",id},{"name",name},{"graph",saved_graph},{"view",QJsonObject{{"version",1},{"scale",1.0}}}};
    }
    bool save_settings() {
        CHECK(manager.numericSettingsDelta(document,settings,change.application_settings,&error));
        CHECK(manager.saveNumeric(document,graph,change,&error));return true;
    }
    bool create_initial_presets() {
        CHECK(manager.readNumericSettings(document,settings,&error));settings.ui.pipeline_presets=QJsonArray{preset("first","First")};
        if(!save_settings())return false;
        settings.ui.pipeline_presets.append(preset("second","Second"));return save_settings();
    }
    bool reload_two_presets() {
        CHECK(manager.loadNumeric(document,graph,&error));CHECK(manager.readNumericSettings(document,settings,&error));
        CHECK(settings.ui.pipeline_presets.size()==2);return true;
    }
    bool opaque_typed_graph() {
        CHECK(encode_numeric_graph(graph,opaque_graph));opaque_graph.insert(1,"\"graph_opaque\":1e9999,");
        opaque_graph.replace("\"kind\":\"Converter\"","\"kind\":\"Converter\",\"node_opaque\":9007199254740993");return true;
    }
    void opaque_qt_graph() {
        auto opaque_typed=typed;opaque_typed.insert(1,"\"inner_opaque\":18446744073709551616,");auto qt_graph=saved_graph;
        auto nodes=qt_graph["nodes"].toArray();auto node=nodes[0].toObject();auto internal=node["internal-data"].toObject();
        internal["numeric_processing"]=QString::fromUtf8(opaque_typed);node["internal-data"]=internal;nodes[0]=node;qt_graph["nodes"]=nodes;
        qt_bytes=QJsonDocument(qt_graph).toJson(QJsonDocument::Compact);qt_bytes.insert(1,"\"qt_opaque\":-0.000E+000,");
        qt_bytes.replace("\"position\":{","\"position\":{\"position_opaque\":1e9999,");
    }
    bool opaque_project_source() {
        opaque_qt_graph();QByteArray scene;CHECK(encode_numeric_graph(graph,scene));
        auto applied=scene;applied.insert(1,"\"applied_opaque\":1e9999,");
        source="{\"schema_version\":4,\"scene_graph\":"+scene+",\"pipeline_graph\":"+applied+",\"ui\":{\"opaque\":1e9999,\"camera\":{\"x\":1.2300E+000},\"pipeline_presets\":[{\"version\":1,\"id\":\"first\",\"name\":\"First\",\"preset_opaque\":9007199254740993,\"graph\":"+qt_bytes+",\"view\":{\"version\":1,\"scale\":1.2300E+000,\"view_opaque\":1e9999}},{\"version\":1,\"id\":\"typed\",\"name\":\"Typed\",\"graph\":"+opaque_graph+"}]}}";
        return true;
    }
    bool write_opaque_fixture() {
        QFile fixture(path);CHECK(fixture.open(QIODevice::WriteOnly));CHECK(fixture.write(source)==source.size());fixture.close();return true;
    }
    bool disable_typed_node() {
        auto node=graph.nodes[0];node.enabled=false;CHECK(encode_numeric_node(node,edited_snapshot));return true;
    }
    void edit_first_preset() {
        first=settings.ui.pipeline_presets[0].toObject();first["name"]="Renamed";auto edited=first["graph"].toObject();
        auto nodes=edited["nodes"].toArray();auto node=nodes[0].toObject();node["position"]=QJsonObject{{"x",99.0},{"y",88.0}};
        auto internal=node["internal-data"].toObject();internal["numeric_processing"]=QString::fromUtf8(edited_snapshot);
        node["internal-data"]=internal;nodes[0]=node;edited["nodes"]=nodes;first["graph"]=edited;
    }
    bool reorder_and_append() {
        edit_first_preset();settings.ui.pipeline_presets=QJsonArray{settings.ui.pipeline_presets[1],first,preset("third","Third")};return save_settings();
    }
    bool raw_numeric_tokens() const {
        const auto raw=QByteArray(document.source.bytes().data(),qsizetype(document.source.bytes().size()));
        for(auto token:{"9007199254740993","1e9999","18446744073709551616","-0.000E+000","1.2300E+000","18446744073709551615"})CHECK(raw.contains(token));
        return true;
    }
    bool raw_graph_depths() const {
        const auto raw=document.source.bytes();CHECK(raw.find("\"applied_opaque\":1e9999")!=raw.npos);
        CHECK(raw.find("\"preset_opaque\":9007199254740993")!=raw.npos);CHECK(raw.find("\"node_opaque\":9007199254740993")!=raw.npos);return true;
    }
    bool raw_presentation_depths() const {
        const auto raw=document.source.bytes();CHECK(raw.find("\"position_opaque\":1e9999")!=raw.npos);
        CHECK(raw.find("\"view_opaque\":1e9999")!=raw.npos);return true;
    }
    bool reload_three_presets() {
        CHECK(manager.loadNumeric(document,graph,&error));CHECK(manager.readNumericSettings(document,settings,&error));
        CHECK(settings.ui.pipeline_presets.size()==3);return true;
    }
    bool preset_order_and_names() const {
        CHECK(settings.ui.pipeline_presets[0].toObject()["id"]=="typed");CHECK(settings.ui.pipeline_presets[1].toObject()["name"]=="Renamed");return true;
    }
    bool decode_retained_node(ConfiguredProcessingNode& decoded) {
        retained=settings.ui.pipeline_presets[1].toObject();reloaded_node=retained["graph"].toObject()["nodes"].toArray()[0].toObject();
        CHECK(decode_numeric_node(reloaded_node["internal-data"].toObject()["numeric_processing"].toString().toUtf8(),decoded));
        CHECK(!decoded.enabled);CHECK(std::get<std::uint64_t>(std::get<ConverterProcessingParameters>(decoded.parameters).policy.scaling.gain.numerator)==std::numeric_limits<std::uint64_t>::max());return true;
    }
    bool exact_retained_policy(const ConfiguredProcessingNode& decoded) const {
        const auto& policy=std::get<ConverterProcessingParameters>(decoded.parameters).policy;
        CHECK(std::get<std::int64_t>(policy.scaling.offset.numerator)==std::numeric_limits<std::int64_t>::min());
        CHECK(std::bit_cast<std::uint32_t>(std::get<float>(policy.nan_replacement))==0x7fc00001u);
        CHECK(std::bit_cast<std::uint64_t>(std::get<double>(policy.negative_inf_replacement))==0x8000000000000000ULL);return true;
    }
    bool retained_node_oracles() {
        ConfiguredProcessingNode decoded;if(!decode_retained_node(decoded)||!exact_retained_policy(decoded))return false;
        CHECK(reloaded_node["internal-data"].toObject()["presentation"]==preset_flags);return true;
    }
    bool remove_third_and_save_as() {
        settings.ui.pipeline_presets.removeAt(2);if(!save_settings())return false;
        CHECK(manager.saveNumericAs(document,graph,change,target,&error));return reload_two_presets();
    }
    bool remember_baseline() {
        unchanged=std::string(document.source.bytes());disk_before=document.disk_baseline;
        return true;
    }
    bool duplicate_preset_refused() {
        auto invalid=settings.ui.pipeline_presets;invalid.append(invalid[0]);change.application_settings=QJsonObject{{"ui",QJsonObject{{"pipeline_presets",invalid}}}};
        CHECK(!manager.saveNumeric(document,graph,change,&error));CHECK(document.source.bytes()==unchanged);
        CHECK(document.disk_baseline==disk_before);return true;
    }
    bool future_preset_refused() {
        auto invalid=settings.ui.pipeline_presets;auto future=invalid[0].toObject();future["version"]=2;invalid[0]=future;
        change.application_settings=QJsonObject{{"ui",QJsonObject{{"pipeline_presets",invalid}}}};
        CHECK(!manager.saveNumeric(document,graph,change,&error));CHECK(document.source.bytes()==unchanged);return true;
    }
    QJsonObject malformed_typed_preset() const {
        auto malformed=retained;auto edited=malformed["graph"].toObject();auto nodes=edited["nodes"].toArray();
        auto node=nodes[0].toObject();auto internal=node["internal-data"].toObject();internal["numeric_processing"]="{}";
        node["internal-data"]=internal;nodes[0]=node;edited["nodes"]=nodes;malformed["graph"]=edited;return malformed;
    }
    bool malformed_preset_refused() {
        change.application_settings=QJsonObject{{"ui",QJsonObject{{"pipeline_presets",QJsonArray{malformed_typed_preset()}}}}};
        CHECK(!manager.saveNumeric(document,graph,change,&error));CHECK(document.source.bytes()==unchanged);return true;
    }
    bool stage_external_conflict() {
        settings.ui.pipeline_presets.append(preset("fourth","Fourth"));
        CHECK(manager.numericSettingsDelta(document,settings,change.application_settings,&error));
        QFile external(target);CHECK(external.open(QIODevice::WriteOnly));external.write("{\"schema_version\":4,\"external\":1}");external.close();return true;
    }
    bool external_conflict_refused() {
        CHECK(!manager.saveNumeric(document,graph,change,&error));CHECK(document.source.bytes()==unchanged);CHECK(document.disk_baseline==disk_before);return true;
    }
    bool opaque_roundtrip() {
        return opaque_typed_graph()&&opaque_project_source()&&write_opaque_fixture()&&reload_two_presets()
            &&disable_typed_node()&&reorder_and_append()&&raw_numeric_tokens()&&raw_graph_depths()
            &&raw_presentation_depths()&&reload_three_presets()&&preset_order_and_names()&&retained_node_oracles();
    }
public:
    bool run() {
        const bool complete=initialize_document()&&initialize_saved_graph()&&create_initial_presets()&&reload_two_presets()
            &&opaque_roundtrip()&&remove_third_and_save_as()&&remember_baseline()&&duplicate_preset_refused()
            &&future_preset_refused()&&malformed_preset_refused()&&stage_external_conflict()&&external_conflict_refused();
        if(complete)std::printf("preset vertical PASS\n");return complete;
    }
};
static bool preset_vertical() {PresetVerticalScenario scenario;return scenario.run();}
class ProtectedSaveAsScenario {
    QTemporaryDir directory;
    const QString active=directory.filePath("active.json"),target=directory.filePath("protected.json");
    ConfigManager manager{active};
    QString error;
    NumericProcessingGraph graph=sample(),edit=graph,restored;
    NumericProjectDocument document,loaded;
    NumericDraftPresentation layout,restored_layout;
    std::string source_before;
    QByteArray baseline_before;
    QString path_before,manager_before;
    bool initialize() {
        CHECK(directory.isValid());CHECK(read_numeric_project("{\"schema_version\":4,\"opaque\":1e9999}",document));
        CHECK(manager.saveNumeric(document,graph,&error));return true;
    }
    bool write_protected(const QByteArray& bytes) {
        QFile file(target);CHECK(file.open(QIODevice::WriteOnly|QIODevice::Truncate));CHECK(file.write(bytes)==bytes.size());file.close();return true;
    }
    void remember_active_source() {
        source_before=std::string(document.source.bytes());baseline_before=document.disk_baseline;
        path_before=document.active_path;manager_before=manager.getConfigPath();
    }
    bool reject_without_publication() {
        remember_active_source();CHECK(!manager.saveNumericAs(document,edit,layout,target,&error));CHECK(!error.isEmpty());
        CHECK(document.source.bytes()==source_before&&document.disk_baseline==baseline_before&&document.active_path==path_before&&manager.getConfigPath()==manager_before);return true;
    }
    bool retry_supported_destination() {
        QFile file(target);CHECK(file.open(QIODevice::WriteOnly|QIODevice::Truncate));
        file.write("{\"schema_version\":4,\"replaceable\":true}");file.close();
        CHECK(manager.saveNumericAs(document,edit,layout,target,&error));CHECK(error.isEmpty());return true;
    }
    bool committed_graph_and_paths() {
        CHECK(document.active_path==target&&manager.getConfigPath()==target);CHECK(manager.loadNumeric(loaded,restored,&error));
        CHECK(!restored.nodes[0].enabled);return true;
    }
    bool committed_layout() {
        CHECK(read_numeric_presentation(loaded,restored_layout));CHECK(restored_layout.positions==layout.positions);return true;
    }
    bool restore_independent_active_path() {
        CHECK(document.source.bytes().find("1e9999")!=std::string_view::npos);
        CHECK(manager.saveNumericAs(document,graph,active,&error));return true;
    }
    bool protected_destination_case(const QByteArray& bytes) {
        return write_protected(bytes)&&reject_without_publication()&&file_bytes_equal(target,bytes)
            &&retry_supported_destination()&&committed_graph_and_paths()&&committed_layout()
            &&file_bytes_equal(target,document.disk_baseline)&&restore_independent_active_path();
    }
public:
    bool run() {
        if(!initialize())return false;
        edit.nodes[0].enabled=false;layout.positions["7"]=QJsonObject{{"x",123},{"y",456}};
        for(const auto& bytes:{QByteArray("{invalid"),QByteArray("{\"schema_version\":99,\"sentinel\":true}"),QByteArray("{\"schema_version\":4,\"scene_graph\":false}")})
            if(!protected_destination_case(bytes))return false;
        return true;
    }
};
static bool manager_save_as_protected_targets() {ProtectedSaveAsScenario scenario;return scenario.run();}
class PreferencesOnlyScenario {
    QTemporaryDir directory;
    const QString path=directory.filePath("preferences.json");
    ConfigManager manager{path};
    const QByteArray future="{\"schema_version\":999}";
    QByteArray applied,committed,baseline;
    NumericProjectDocument document;
    NumericProcessingGraph draft;
    FileSaveApplicationConfig settings,restored;
    QString error;
    bool initialize() {
        CHECK(directory.isValid());CHECK(encode_numeric_graph(sample(),applied));
        const auto original="{\"schema_version\":4,\"pipeline_graph\":"+applied+",\"ui\":{\"theme\":\"light\",\"language\":\"en\",\"opaque\":1e9999},\"opaque\":9007199254740993}";
        CHECK(read_numeric_project(original,document));return true;
    }
    bool save_applied_only_source() {
        CHECK(save_numeric_project(document,path));CHECK(manager.readNumericSettings(document,settings,&error));
        CHECK(!read_numeric_draft(document,draft));return true;
    }
    bool save_preferences_without_draft() {
        settings.ui.theme="dark";settings.ui.language="ru";
        CHECK(manager.saveNumericPreferences(document,settings,&error));CHECK(error.isEmpty());
        committed=QByteArray(document.source.bytes().data(),qsizetype(document.source.bytes().size()));return true;
    }
    bool applied_graph_preserved() const {
        CHECK(!committed.contains("scene_graph"));CHECK(committed.contains(applied));return true;
    }
    bool opaque_lexemes_preserved() const {
        CHECK(committed.contains("1e9999"));CHECK(committed.contains("9007199254740993"));return true;
    }
    bool settings_restored() {
        CHECK(manager.readNumericSettings(document,restored,&error));CHECK(restored.ui.theme=="dark"&&restored.ui.language=="ru");
        CHECK(!read_numeric_draft(document,draft));return true;
    }
    bool unchanged_preferences_never_write() {
        QFile file(path);CHECK(file.open(QIODevice::ReadOnly));
        const auto before=file.readAll();file.close();
        const auto modified=QFileInfo(path).lastModified();
        // Simulate repeated dock/camera notifications: they must not trigger
        // complete canonical-project rewrites if the UI data is unchanged.
        for(int i=0;i<20;++i)CHECK(manager.saveNumericPreferences(document,settings,&error));
        CHECK(file.open(QIODevice::ReadOnly));const auto after=file.readAll();file.close();
        CHECK(after==before && document.disk_baseline==before);
        CHECK(QFileInfo(path).lastModified()==modified);
        return true;
    }
    bool externally_replace_destination() {
        QFile external(path);CHECK(external.open(QIODevice::WriteOnly|QIODevice::Truncate));CHECK(external.write(future)==future.size());external.close();
        baseline=document.disk_baseline;settings.ui.theme="light";return true;
    }
    bool refuse_conflicting_preferences() {
        CHECK(!manager.saveNumericPreferences(document,settings,&error));CHECK(!error.isEmpty());return true;
    }
    bool refused_candidate_unpublished() const {
        CHECK(document.disk_baseline==baseline);CHECK(QByteArray(document.source.bytes().data(),qsizetype(document.source.bytes().size()))==committed);return true;
    }
public:
    bool run() {
        return initialize()&&save_applied_only_source()&&save_preferences_without_draft()&&applied_graph_preserved()
            &&opaque_lexemes_preserved()&&settings_restored()&&unchanged_preferences_never_write()&&externally_replace_destination()
            &&refuse_conflicting_preferences()&&refused_candidate_unpublished()&&file_bytes_equal(path,future);
    }
};
static bool preferences_only_source_atomicity() {PreferencesOnlyScenario scenario;return scenario.run();}
class ManagerRollbackScenario {
    QTemporaryDir directory;
    const QString path=directory.filePath("numeric.json"),next=directory.filePath("nested/next.json");
    ConfigManager manager{path};
    NumericProjectDocument document,loaded;
    NumericProcessingGraph graph=sample(),read;
    QString error;
    bool initialize() {
        CHECK(directory.isValid());CHECK(read_numeric_project("{\"schema_version\":4,\"ui\":{\"unknown\":1e9999}}",document));return true;
    }
    bool initial_save() {
        CHECK(manager.saveNumeric(document,graph,&error));CHECK(error.isEmpty());return true;
    }
    bool initial_load() {
        CHECK(manager.loadNumeric(loaded,read,&error));CHECK(same_processing_parameters(graph.nodes[0].parameters,read.nodes[0].parameters));return true;
    }
    bool blank_path_refused() {
        const auto old=manager.getConfigPath();const auto raw=std::string(document.source.bytes());
        CHECK(!manager.saveNumericAs(document,graph,QString{},&error));CHECK(manager.getConfigPath()==old);CHECK(document.source.bytes()==raw);return true;
    }
    bool nested_save_as() {
        CHECK(manager.saveNumericAs(document,graph,next,&error));CHECK(manager.getConfigPath()==next);CHECK(document.active_path==next);return true;
    }
    bool external_future_source() {
        QFile external(next);CHECK(external.open(QIODevice::WriteOnly));external.write("{\"schema_version\":100}");external.close();return true;
    }
    bool conflicting_save_refused() {
        const auto raw=std::string(document.source.bytes());CHECK(!manager.saveNumeric(document,graph,&error));
        CHECK(document.source.bytes()==raw);CHECK(manager.getConfigPath()==next);return true;
    }
public:
    bool run() {return initialize()&&initial_save()&&initial_load()&&blank_path_refused()&&nested_save_as()&&external_future_source()&&conflicting_save_refused();}
};
static bool manager_path_and_rollback() {ManagerRollbackScenario scenario;return scenario.run();}
// Explicit validation of a separately prepared current-format recovery artifact.
// This is not a legacy loader: the normal current reader validates the input,
// and every write is confined to a new destination supplied by the caller.
class CurrentProjectVerificationScenario {
    const QString input,output;
    QFile original;
    ConfigManager manager;
    QByteArray original_bytes,encoded;
    NumericProjectDocument document,reloaded;
    NumericProcessingGraph graph,resolved,decoded,roundtrip;
    QString error;
    const ProcessingAdmissionLimits limits{4096,65536,64,64,1024,16777216,{64,16777216,16777216,16777216,1024,16777216}};
    NumericDraftPresentation presentation,restored_presentation;
    std::optional<NumericProcessingGraph> applied,restored_applied;
    FileSaveApplicationConfig settings,restored;
    QJsonObject initial_rendering;
    bool remember_input() {
        CHECK(!QFile::exists(output));CHECK(original.open(QIODevice::ReadOnly));original_bytes=original.readAll();original.close();return true;
    }
    bool load_current_project() {
        if(!manager.loadNumeric(document,graph,&error)) {
            std::fprintf(stderr,"Current project validation: %s\n",qPrintable(error));return false;
        }
        return true;
    }
    bool resolve_bindings() {
        const auto result=resolve_configuration_bindings(graph,limits,resolved);
        if(!result){std::fprintf(stderr,"Configuration admission: node=%d error=%u\n",result.node,unsigned(result.error));return false;}
        return true;
    }
    bool admit_node(const ConfiguredProcessingNode& node) const {
        ResolvedProcessingNode operation;const auto result=resolve_processing_node(node,limits,operation);
        if(!result){std::fprintf(stderr,"Node admission: node=%d error=%u\n",result.node,unsigned(result.error));return false;}
        return true;
    }
    bool admit_nodes() const {
        for(const auto& node:resolved.nodes)if(!admit_node(node))return false;
        return true;
    }
    bool graph_codec_roundtrip() {
        CHECK(encode_numeric_graph(graph,encoded));CHECK(decode_numeric_graph(encoded,decoded));
        CHECK(decoded.nodes.size()==graph.nodes.size()&&decoded.edges==graph.edges&&decoded.configuration_bindings==graph.configuration_bindings);return true;
    }
    bool exact_parameter_identity() const {
        for(std::size_t index=0;index<graph.nodes.size();++index) {
            CHECK(decoded.nodes[index].id==graph.nodes[index].id);
            CHECK(same_processing_parameters(decoded.nodes[index].parameters,graph.nodes[index].parameters));
        }
        return true;
    }
    bool remember_project_projections() {
        CHECK(read_numeric_presentation(document,presentation));CHECK(read_numeric_applied(document,applied));
        CHECK(manager.readNumericSettings(document,settings,&error));return true;
    }
    bool save_as_and_change_preferences() {
        CHECK(manager.saveNumericAs(document,graph,output,&error));initial_rendering=settings.ui.graph_rendering;
        settings.ui.graph_rendering["gui_fps_mode"]="fixed";settings.ui.graph_rendering["gui_target_fps"]=201;
        CHECK(manager.saveNumericPreferences(document,settings,&error));return true;
    }
    bool modified_preferences_roundtrip() {
        CHECK(manager.loadNumeric(reloaded,roundtrip,&error));CHECK(manager.readNumericSettings(reloaded,restored,&error));
        CHECK(restored.ui.graph_rendering["gui_fps_mode"]=="fixed"&&restored.ui.graph_rendering["gui_target_fps"]==201);return true;
    }
    bool restore_preferences() {
        settings.ui.graph_rendering=initial_rendering;CHECK(manager.saveNumericPreferences(document,settings,&error));
        CHECK(manager.loadNumeric(reloaded,roundtrip,&error));CHECK(manager.readNumericSettings(reloaded,restored,&error));return true;
    }
    bool restored_graph_and_preferences() const {
        CHECK(restored.ui.graph_rendering==initial_rendering);
        CHECK(roundtrip.nodes.size()==graph.nodes.size()&&roundtrip.edges==graph.edges&&roundtrip.configuration_bindings==graph.configuration_bindings);return true;
    }
    bool restored_node_presentation() {
        CHECK(read_numeric_presentation(reloaded,restored_presentation));
        CHECK(restored_presentation.positions==presentation.positions&&restored_presentation.node_presentations==presentation.node_presentations);return true;
    }
    bool restored_applied_graph() {
        CHECK(read_numeric_applied(reloaded,restored_applied));CHECK(bool(applied)==bool(restored_applied));
        if(applied)CHECK(restored_applied->nodes.size()==applied->nodes.size()&&restored_applied->edges==applied->edges&&restored_applied->configuration_bindings==applied->configuration_bindings);
        return true;
    }
    bool input_unchanged() {
        CHECK(original.open(QIODevice::ReadOnly));CHECK(original.readAll()==original_bytes);return true;
    }
public:
    CurrentProjectVerificationScenario(const QString& input,const QString& output):input(input),output(output),original(input),manager(input) {}
    bool run() {
        const bool complete=remember_input()&&load_current_project()&&resolve_bindings()&&admit_nodes()
            &&graph_codec_roundtrip()&&exact_parameter_identity()&&remember_project_projections()
            &&save_as_and_change_preferences()&&modified_preferences_roundtrip()&&restore_preferences()
            &&restored_graph_and_preferences()&&restored_node_presentation()&&restored_applied_graph()&&input_unchanged();
        if(complete)std::printf("Current project verification: %zu nodes, %zu data edges, %zu explicit bindings; encode/SaveAs/preferences/reload PASS; input unchanged\n",graph.nodes.size(),graph.edges.size(),graph.configuration_bindings.size());
        return complete;
    }
};
static bool verify_current_project(const QString& input,const QString& output) {CurrentProjectVerificationScenario scenario(input,output);return scenario.run();}
#endif
static ConfiguredProcessingNode typed_array_node() {
    ConfiguredProcessingNode node;node.id=31;node.kind="ArrayFill";
    node.parameters=TransformProcessingParameters{{{"size",NumericLiteral{std::uint64_t{3}}},
        {"values",NumericArray{{std::bit_cast<float>(std::uint32_t{0x80000000}),
            std::bit_cast<float>(std::uint32_t{0x3f800001}),std::bit_cast<float>(std::uint32_t{1})}}}}};
    return node;
}
static const std::vector<float>& array_values(const ConfiguredProcessingNode& node) {
    return std::get<NumericArray>(std::get<TransformProcessingParameters>(node.parameters).values[1].value).values;
}
static bool identical_array_bits(std::span<const float> a,std::span<const float> b) {
    CHECK(a.size()==b.size());
    for(std::size_t i=0;i<a.size();++i)
        CHECK(std::bit_cast<std::uint32_t>(a[i])==std::bit_cast<std::uint32_t>(b[i]));
    return true;
}
static bool typed_array_storage_roundtrip() {
    const auto source=typed_array_node();QByteArray snapshot;
    CHECK(encode_numeric_node(source,snapshot));CHECK(snapshot.contains("numeric_array"));
    ConfiguredProcessingNode restored;CHECK(decode_numeric_node(snapshot,restored));
    return identical_array_bits(array_values(source),array_values(restored));
}
static bool node_snapshot_equal(const ConfiguredProcessingNode& node,const QByteArray& snapshot) {
    QByteArray encoded;CHECK(encode_numeric_node(node,encoded));CHECK(encoded==snapshot);return true;
}
static bool corrupt_array_storage_refused(const ConfiguredProcessingNode& source,const QByteArray& snapshot) {
    auto corrupt=snapshot;corrupt.replace("f32le-base64:","f32le-base64:!");
    auto restored=source;CHECK(!decode_numeric_node(corrupt,restored));
    return node_snapshot_equal(restored,snapshot);
}
static bool nonfinite_array_storage_refused(const ConfiguredProcessingNode& source) {
    auto malformed=source;
    std::get<NumericArray>(std::get<TransformProcessingParameters>(malformed.parameters).values[1].value).values[0]=std::numeric_limits<float>::infinity();
    QByteArray after="preserve";CHECK(!encode_numeric_node(malformed,after));CHECK(after=="preserve");
    return true;
}
static bool typed_array_storage_refusal() {
    const auto source=typed_array_node();
    QByteArray snapshot;
    CHECK(encode_numeric_node(source,snapshot));
    return corrupt_array_storage_refused(source,snapshot) && nonfinite_array_storage_refused(source);
}
static bool replace_string_array_parameter(QJsonObject& encoded,bool corrupt) {
    auto nodes=encoded["nodes"].toArray();CHECK(nodes.size()==1);
    auto configured=nodes[0].toObject();auto parameters=configured["parameters"].toObject();
    auto values=parameters["values"].toArray();CHECK(values.size()==2);
    auto array_parameter=values[1].toObject();
    CHECK(array_parameter["key"].toString()=="values");
    CHECK(array_parameter["value"].toObject()["type"].toString()=="numeric_array");
    auto payload=array_parameter["value"].toObject()["values"].toString();
    CHECK(payload.startsWith("f32le-base64:"));
    if(corrupt)payload.insert(13,'!');
    array_parameter["value"]=payload;values[1]=array_parameter;
    parameters["values"]=values;configured["parameters"]=parameters;nodes[0]=configured;encoded["nodes"]=nodes;
    return true;
}
static bool string_encoded_array_snapshot(QByteArray& snapshot,bool corrupt) {
    NumericProcessingGraph graph;graph.graph_id="typed-array";graph.nodes.push_back(typed_array_node());
    QByteArray typed;CHECK(encode_numeric_graph(graph,typed));
    auto encoded=QJsonDocument::fromJson(typed).object();
    CHECK(replace_string_array_parameter(encoded,corrupt));
    snapshot=QJsonDocument(encoded).toJson(QJsonDocument::Compact);
    return true;
}
static bool string_encoded_array_project_roundtrip() {
    QByteArray snapshot;CHECK(string_encoded_array_snapshot(snapshot,false));
    const QByteArray project="{\"schema_version\":4,\"scene_graph\":"+snapshot+"}";
    NumericProjectDocument source,edited;
    CHECK(read_numeric_project(project,source));
    NumericProcessingGraph restored;CHECK(read_numeric_draft(source,restored));
    CHECK(restored.nodes.size()==1);
    CHECK(std::holds_alternative<TransformProcessingParameters>(restored.nodes[0].parameters));
    CHECK(std::get<TransformProcessingParameters>(restored.nodes[0].parameters).values.size()==2);
    CHECK(identical_array_bits(array_values(typed_array_node()),array_values(restored.nodes[0])));
    CHECK(stage_numeric_draft(source,restored,edited));
    const auto saved=std::string(edited.source.bytes());
    CHECK(saved.find("f32le-base64:")!=saved.npos);
    CHECK(saved.find("numeric_array")==saved.npos);
    return true;
}
static bool malformed_string_array_project_refused() {
    QByteArray stale;CHECK(string_encoded_array_snapshot(stale,true));
    NumericProjectDocument document;
    CHECK(read_numeric_project("{\"schema_version\":4,\"opaque\":1e9999}",document));
    document.active_path="retained.json";
    const auto retained=std::string(document.source.bytes());
    for(const char* section:{"scene_graph","pipeline_graph"}) {
        const QByteArray project="{\"schema_version\":4,\""+QByteArray(section)+"\":"+stale+"}";
        const auto result=read_numeric_project(project,document);
        CHECK(!result);
        CHECK(result.path.contains("values"));
        CHECK(document.source.bytes()==retained && document.active_path=="retained.json");
    }
    return true;
}
class VisualPresetOptionsScenario {
    NumericProcessingGraph graph=sample(),editor_output;
    NumericProjectDocument source,output;
    QJsonObject visual,preset;
    NumericDraftPresentation change;
    std::string sentinel;
    bool initialize() {
        graph.nodes[0].inputs.push_back(graph.nodes[0].outputs[0]);
        graph.nodes[0].inputs[0].direction=PortDirection::Input;
        QByteArray typed;CHECK(encode_numeric_node(graph.nodes[0],typed));
        const QJsonObject internal{{"model-name","Converter"},{"numeric_processing",QString::fromUtf8(typed)}};
        visual=QJsonObject{{"version",5},{"processing_version",4},{"graph_id","main"},
            {"nodes",QJsonArray{QJsonObject{{"id",7},{"internal-data",internal}}}},{"connections",QJsonArray{}}};
        CHECK(read_numeric_project("{\"schema_version\":4,\"sentinel\":1e9999}",source));
        sentinel=std::string(source.source.bytes());source.active_path="sentinel-path";source.disk_baseline="sentinel-disk";
        return true;
    }
    void set_option(const char* key,const QJsonValue& value) {
        QJsonObject edge{{"outNodeId",7},{"outPortIndex",0},{"inNodeId",7},{"inPortIndex",0}};
        edge[key]=value;visual["connections"]=QJsonArray{edge};
        preset=QJsonObject{{"version",1},{"id","options"},{"name","Options"},{"graph",visual}};
        change.application_settings=QJsonObject{{"ui",QJsonObject{{"pipeline_presets",QJsonArray{preset}}}}};
    }
    QByteArray project_bytes() const {
        return QJsonDocument(QJsonObject{{"schema_version",4},{"ui",QJsonObject{{"pipeline_presets",QJsonArray{preset}}}}}).toJson(QJsonDocument::Compact);
    }
    bool document_unpublished() const {
        CHECK(output.source.bytes()==sentinel);CHECK(output.active_path=="sentinel-path");
        CHECK(output.disk_baseline=="sentinel-disk");return true;
    }
    bool reader_refuses() {
        CHECK(clone_numeric_project(source,output));CHECK(!read_numeric_project(project_bytes(),output));
        return document_unpublished();
    }
    bool replacement_refuses() {
        CHECK(clone_numeric_project(source,output));CHECK(!stage_numeric_presentation(source,change,output));
        return document_unpublished();
    }
    bool editor_refuses() {
        editor_output=graph;
        CHECK(!decode_numeric_editor_graph(QJsonDocument(visual).toJson(QJsonDocument::Compact),editor_output));
        return snapshot_graphs_equal(graph,editor_output);
    }
    bool invalid_option(const char* key,const QJsonValue& value) {
        set_option(key,value);return reader_refuses()&&replacement_refuses()&&editor_refuses();
    }
    bool valid_fixture() {
        set_option("conversion",0);CHECK(read_numeric_project(project_bytes(),output));
        CHECK(stage_numeric_presentation(source,change,output));
        CHECK(decode_numeric_editor_graph(QJsonDocument(visual).toJson(QJsonDocument::Compact),editor_output));return true;
    }
public:
    bool run() {
        return initialize()&&valid_fixture()&&invalid_option("conversion",4)&&invalid_option("conversion","bad")
            &&invalid_option("configuration",2)&&invalid_option("configuration","bad");
    }
};
static bool visual_preset_options_refusal() {VisualPresetOptionsScenario scenario;return scenario.run();}
class NetworkProjectionScenario {
    NumericProjectDocument document;
    QJsonObject output;
    const QJsonObject sentinel{{"preserve",17}};
    bool missing_network() {
        CHECK(read_numeric_project("{\"schema_version\":4,\"opaque\":1e9999}",document));
        output=sentinel;CHECK(read_numeric_network_settings(document,0,output));CHECK(output.isEmpty());return true;
    }
    bool serial_preserved() const {
        const auto serial=output["serial"].toObject();
        CHECK(serial==QJsonObject({{"port_name","COM5"},{"baud_rate",115200},{"input_node",7}}));return true;
    }
    bool route_preserved() const {
        const auto routes=output["routes"].toArray();CHECK(routes.size()==1);
        const auto route=routes[0].toObject();CHECK(route["node_id"]==7 && route["host"]=="127.0.0.1" && route["port"]==8213);
        CHECK(route["remote_endpoint"].toObject()==QJsonObject({{"graph_id","remote"},{"node_id",2},{"direction","output"},{"port_index",0}}));return true;
    }
    bool opaque_network() {
        const QByteArray raw=R"({"schema_version":4,"root_opaque":1e9999,"network":{"opaque":1e9999,"serial":{"port_name":"COM5","baud_rate":115200,"input_node":7,"opaque":1e9999},"routes":[{"node_id":7,"host":"127.0.0.1","port":8213,"opaque":1e9999,"remote_endpoint":{"graph_id":"remote","node_id":2,"direction":"output","port_index":0,"opaque":1e9999}}]}})";
        CHECK(read_numeric_project(raw,document));CHECK(read_numeric_network_settings(document,1,output));
        CHECK(document.source.bytes()==std::string_view(raw.constData(),std::size_t(raw.size())));
        return serial_preserved()&&route_preserved();
    }
    bool refuse(const QByteArray& raw,std::size_t bound=1) {
        CHECK(read_numeric_project(raw,document));output=sentinel;
        CHECK(!read_numeric_network_settings(document,bound,output));CHECK(output==sentinel);return true;
    }
    bool malformed_serial() {
        for(const auto* serial:{R"({"port_name":5})",R"({"port_name":"COM5","baud_rate":"bad"})",
            R"({"port_name":"COM5","input_node":"bad"})",R"({"port_name":"COM5","baud_rate":0})",
            R"({"port_name":"COM5","input_node":-1})",R"({"port_name":"COM5","port_name":"COM6"})"})
            if(!refuse(QByteArray("{\"schema_version\":4,\"network\":{\"serial\":")+serial+"}}"))return false;
        return true;
    }
    bool malformed_route() {
        for(const auto* route:{R"({"node_id":"bad"})",R"({"node_id":7,"node_id":8})",
            R"({"node_id":7,"host":5})",R"({"node_id":7,"port":0})"})
            if(!refuse(QByteArray("{\"schema_version\":4,\"network\":{\"routes\":[")+route+"]}}"))return false;
        return true;
    }
    bool malformed_network() {
        for(const auto* raw:{R"({"schema_version":4,"network":false})",
            R"({"schema_version":4,"network":{},"network":{}})",
            R"({"schema_version":4,"network":{"serial":{},"serial":{}}})",
            R"({"schema_version":4,"network":{"routes":[],"routes":[]}})",
            R"({"schema_version":4,"network":{"routes":{}}})"})if(!refuse(raw))return false;
        return true;
    }
    bool route_bound() {
        return refuse(R"({"schema_version":4,"network":{"routes":[{"node_id":7},{"node_id":8}]}})")
            &&refuse(R"({"schema_version":4,"network":{"routes":[{"node_id":7}]}})",0);
    }
public:
    bool run(){return missing_network()&&opaque_network()&&malformed_serial()&&malformed_route()&&malformed_network()&&route_bound();}
};
static bool network_projection_boundary() {NetworkProjectionScenario scenario;return scenario.run();}
static int check_source_argument(const QString& path) {
    NumericProjectDocument document;NumericProcessingGraph graph;
    const auto result=load_numeric_project(path,document,graph);
    if(!result){std::fprintf(stderr,"Project load refused: %s at %s\n",qPrintable(result.error),qPrintable(result.path));return 1;}
    std::printf("Project source load PASS: %zu nodes\n",graph.nodes.size());return 0;
}
static bool path_normalization() {
    const auto path=QDir::current().filePath("config.json");
    CHECK(numeric_project_path(path+"/")==numeric_project_path(path));
#ifdef Q_OS_WIN
    CHECK(numeric_project_path(QDir::toNativeSeparators(path)+"\\")==numeric_project_path(path));
#endif
    return true;
}
class DocumentVersionBoundaryScenario {
    NumericProcessingGraph graph=sample();
    NumericProjectDocument source, output;
    QByteArray snapshot;
    bool initialize() {
        CHECK(encode_numeric_graph(graph,snapshot));
        CHECK(read_numeric_project("{\"schema_version\":4,\"opaque\":1e9999}",output));
        output.active_path="sentinel.json";
        output.disk_baseline="sentinel";
        return true;
    }
    bool refuse_schema(const QByteArray& edition) {
        const QByteArray raw="{\"schema_version\":"+edition+",\"scene_graph\":"+snapshot+",\"opaque\":1e9999}";
        CHECK(project_storage::parse_project_source(std::string_view(raw.constData(),std::size_t(raw.size())),source.source));
        const auto retained=std::string(output.source.bytes());
        CHECK(!stage_numeric_draft(source,graph,output));
        CHECK(output.source.bytes()==retained && output.active_path=="sentinel.json" && output.disk_baseline=="sentinel");
        CHECK(source.source.bytes()==std::string_view(raw.constData(),std::size_t(raw.size())));
        return true;
    }
public:
    bool run() {
        return initialize() && refuse_schema("99") && refuse_schema("\"4\"") && refuse_schema("0")
            && refuse_schema("4,\"schema_version\":4");
    }
};
static bool document_version_boundary() { DocumentVersionBoundaryScenario scenario;return scenario.run(); }

static bool second_node_raw(const NumericProjectDocument& source,std::string& raw){
    project_storage::NodeHandle scene,nodes;
    CHECK(project_storage::find_unique_member(source.source,source.source.root(),"scene_graph",scene));
    CHECK(project_storage::find_unique_member(source.source,scene,"nodes",nodes));
    project_storage::NodeView array,first,second;
    CHECK(source.source.view(nodes,array));CHECK(source.source.view(array.first_child,first));
    CHECK(source.source.view(first.next_sibling,second));raw=std::string(second.raw);return true;
}
static bool selective_node_settings() {
    auto graph=sample();auto other=graph.nodes.front();other.id=8;graph.nodes.push_back(other);
    NumericProjectDocument source;CHECK(create_numeric_project(graph,{},source));
    QByteArray raw(source.source.bytes().data(),qsizetype(source.source.bytes().size()));
    raw.replace("\"id\":7","\"id\":7,\"future\":1e9999");
    CHECK(read_numeric_project(raw,source));
    const auto original=std::string(source.source.bytes());
    std::string untouched;CHECK(second_node_raw(source,untouched));
    auto changed=graph.nodes.front();changed.enabled=false;
    NumericProjectDocument staged;CHECK(stage_numeric_nodes(source,std::span(&changed,1),staged));
    NumericProcessingGraph result;CHECK(read_numeric_draft(staged,result));
    CHECK(!result.nodes[0].enabled && result.nodes[1].enabled);
    CHECK(staged.source.bytes().find("\"future\":1e9999")!=std::string_view::npos);
    CHECK(staged.source.bytes().find(untouched)!=std::string_view::npos);
    CHECK(source.source.bytes()==original);
    const auto before=std::string(staged.source.bytes());changed.id=999;
    CHECK(!stage_numeric_nodes(source,std::span(&changed,1),staged));CHECK(staged.source.bytes()==before);
    return true;
}
static bool spectral_quantity_roundtrip() {
    for(const auto quantity:{Quantity::SpectrumRmsAmplitude,Quantity::SpectrumBinPower,Quantity::SpectrumWindowScaledMagnitude,
            Quantity::BroadbandBinPowerSum,Quantity::BroadbandBinPowerMean,Quantity::BroadbandBinPowerRms,Quantity::BroadbandBinPowerMaximum,Quantity::BroadbandBinPowerFlux}){
        auto graph=sample();auto& output=graph.nodes.front().outputs.front().contract;
        auto value=std::get<NumericContract>(output.view());value.dtype=DType::Float32;value.quantity=quantity;
        value.unit=(quantity==Quantity::SpectrumRmsAmplitude || quantity==Quantity::SpectrumWindowScaledMagnitude || quantity==Quantity::BroadbandBinPowerRms)
            ?NumericUnit::RawAmplitude:NumericUnit::SquaredAmplitude;
        OwnedProcessingContract owner;
        CHECK(own_processing_contract(value,FrameLimits{64,1000000,16777216,16777216,1024,16777216},16777216,owner)==Status::ok);
        output=std::move(owner);QByteArray encoded;NumericProcessingGraph decoded;
        CHECK(encode_numeric_graph(graph,encoded));CHECK(decode_numeric_graph(encoded,decoded));
        const auto restored=std::get<NumericContract>(decoded.nodes.front().outputs.front().contract.view());
        CHECK(restored.quantity==quantity && restored.unit==value.unit && restored.dtype==DType::Float32);
    }
    return true;
}
static int run_document_scenarios() {
    int failures=0;
    failures+=!spectral_quantity_roundtrip();
    failures+=!selective_node_settings();
    for(auto test:{malformed_position_projection,typed_array_storage_roundtrip,typed_array_storage_refusal,string_encoded_array_project_roundtrip,malformed_string_array_project_refused,confirmed_replacement,explicit_binding_storage,exact_clipboard_and_file,refusal_and_output_rollback,unsupported_version_diagnostics,alternatives_and_commit_failure,draft_applied_and_atomic_publication,editor_snapshot_boundary,retained_layout_identity_oracles,settings_arrays_and_opaque_refusal,fresh_project_and_node_presentation,node_display_name_persistence,preset_display_snapshot_roundtrip,node_adapter_storage_and_projection,presentation_vertical,visual_preset_options_refusal,network_projection_boundary,document_version_boundary})failures+=!test();
    return failures;
}
#ifdef NUMERIC_CONFIG_MANAGER_TESTS
static int run_manager_scenarios() {
    int failures=0;failures+=!manager_path_and_rollback();failures+=!preferences_only_source_atomicity();
    failures+=!manager_save_as_protected_targets();failures+=!preset_vertical();return failures;
}
#endif
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);const auto arguments=app.arguments();
    if(arguments.size()==3&&arguments[1]=="--check-source")return check_source_argument(arguments[2]);
    if(!path_normalization())return 1;
#ifdef NUMERIC_CONFIG_MANAGER_TESTS
    if(arguments.size()==4&&arguments[1]=="--verify-project")return verify_current_project(arguments[2],arguments[3])?0:1;
    if(arguments.contains("--presets-only"))return preset_vertical()?0:1;
#endif
    int failures=run_document_scenarios();
#ifdef NUMERIC_CONFIG_MANAGER_TESTS
    failures+=run_manager_scenarios();
#endif
    std::printf("canonical numeric storage: %d failures\n",failures);return failures?1:0;
}
