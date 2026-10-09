#include <desktop/numeric_project_document.h>
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
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %d %s\n",__LINE__,#x); return false; } } while(false)
static NumericProcessingGraph sample() {
    NumericProcessingGraph g; g.graph_id="main";
    ConfiguredProcessingNode n; n.id=7; n.kind="Converter";
    ConverterProcessingParameters p;
    p.policy.scaling.gain.numerator=std::numeric_limits<std::uint64_t>::max();
    p.policy.scaling.offset.numerator=std::numeric_limits<std::int64_t>::min();
    p.policy.nan_replacement=std::bit_cast<float>(std::uint32_t{0x7fc00001});
    p.policy.negative_inf_replacement=std::bit_cast<double>(std::uint64_t{0x8000000000000000});
    n.parameters=p;
    std::uint32_t channel=0;
    for(int i=0;i<10;++i) {
        NumericContract c; c.dtype=static_cast<DType>(i); c.channel_order=std::span(&channel,1); c.representation.alignment=8;
        OwnedProcessingContract owned; if(own_processing_contract(c,{64,1000000,16777216,16777216,1024,16777216},16777216,owned)!=Status::ok)std::abort();
        n.outputs.push_back({static_cast<PortId>(i),PortDirection::Output,owned});
    }
    g.nodes.push_back(n); return g;
}
static bool exact_clipboard_and_file() {
    auto g=sample(); QByteArray bytes; CHECK(encode_numeric_graph(g,bytes));
    CHECK(bytes.contains("18446744073709551615")); CHECK(bytes.contains("-9223372036854775808")); CHECK(bytes.contains("7fc00001")); CHECK(bytes.contains("8000000000000000"));
    NumericProcessingGraph copy; CHECK(decode_numeric_graph(bytes,copy)); CHECK(same_processing_parameters(g.nodes[0].parameters,copy.nodes[0].parameters));
    for(std::size_t i=0;i<10;++i)CHECK(same_contract(g.nodes[0].outputs[i].contract.view(),copy.nodes[0].outputs[i].contract.view()));
    QByteArray node; CHECK(encode_numeric_node(g.nodes[0],node)); ConfiguredProcessingNode pasted; CHECK(decode_numeric_node(node,pasted));
    auto undo=pasted; std::get<ConverterProcessingParameters>(pasted.parameters).policy.scaling.gain.numerator=std::uint64_t{9}; pasted=undo;
    CHECK(same_processing_parameters(g.nodes[0].parameters,pasted.parameters));
    // Independent opaque source oracle is inserted manually, not by the writer.
    bytes.replace("\"parameters\":{","\"parameters\":{\"unknown\":123456789012345678901234567890,");
    QByteArray project="{\"schema_version\":2,\"scene_graph\":"+bytes+",\"ui\":{\"n\":9007199254740993},\"presets\":[{\"version\":1,\"id\":\"saved\",\"unknown\":1e9999}],\"pipeline_graph\":"+bytes+",\"app\":-0}";
    NumericProjectDocument baseline,edited; CHECK(read_numeric_project(project,baseline));
    CHECK(read_numeric_draft(baseline,copy)); std::get<ConverterProcessingParameters>(copy.nodes[0].parameters).policy.scaling.offset.exponent2=-123;
    CHECK(stage_numeric_draft(baseline,copy,edited));
    auto raw=edited.source.bytes(); CHECK(raw.find("123456789012345678901234567890")!=raw.npos); CHECK(raw.find("9007199254740993")!=raw.npos); CHECK(raw.find("1e9999")!=raw.npos); CHECK(raw.find("\"app\":-0")!=raw.npos);
    QTemporaryDir dir; CHECK(dir.isValid()); auto path=dir.filePath("sub/config.json"); CHECK(save_numeric_project(edited,path));
    NumericProjectDocument reloaded; NumericProcessingGraph read; CHECK(load_numeric_project(path,reloaded,read)); CHECK(same_processing_parameters(copy.nodes[0].parameters,read.nodes[0].parameters)); CHECK(reloaded.active_path==edited.active_path);
    QFile disk(path); CHECK(disk.open(QIODevice::ReadOnly)); CHECK(disk.readAll()==edited.disk_baseline); disk.close();
    // External edit must not be overwritten or become runtime/draft state.
    CHECK(disk.open(QIODevice::WriteOnly)); CHECK(disk.write("{\"schema_version\":999}")>0); disk.close();
    CHECK(!save_numeric_project(edited)); CHECK(disk.open(QIODevice::ReadOnly)); CHECK(disk.readAll()=="{\"schema_version\":999}");
    return true;
}
static bool refusal_and_output_rollback() {
    NumericProjectDocument p,out; CHECK(read_numeric_project("{\"schema_version\":2,\"unknown\":18446744073709551615}",out));
    const auto before=std::string(out.source.bytes());
    for(auto bad:{"{\"schema_version\":1}","{\"schema_version\":2,\"schema_version\":2}","{\"schema_version\":3}","{\"nodes\":[],\"connections\":[]}","{\"schema_version\":2,\"ui\":{\"pipeline_presets\":[{\"version\":1,\"id\":\"future\",\"graph\":{\"version\":4,\"processing_version\":2}}]}}"}){
        CHECK(!read_numeric_project(bad,out)); CHECK(out.source.bytes()==before);
    }
    auto graph=sample(); CHECK(stage_numeric_draft(out,graph,p));
    QByteArray bytes; CHECK(encode_numeric_node(graph.nodes[0],bytes)); auto saved=graph.nodes[0];
    bytes.replace("\"uint64\"","\"uint64\",\"dtype\":\"uint64\""); CHECK(!decode_numeric_node(bytes,saved)); CHECK(same_processing_parameters(saved.parameters,graph.nodes[0].parameters));
    QTemporaryDir dir; auto path=dir.filePath("future.json"); QFile file(path); CHECK(file.open(QIODevice::WriteOnly)); file.write("{\"schema_version\":99}"); file.close(); CHECK(!save_numeric_project(p,path)); CHECK(file.open(QIODevice::ReadOnly)); CHECK(file.readAll()=="{\"schema_version\":99}");
    return true;
}
static bool alternatives_and_commit_failure() {
    auto g=sample();
    ConfiguredProcessingNode history;history.id=8;history.kind="Spectrogram";history.parameters=HistoryProcessingParameters{18446744073709551615ULL};g.nodes.push_back(history);
    ConfiguredProcessingNode route;route.id=9;route.kind="Gate";route.parameters=RouteProcessingParameters{2,1,false};g.nodes.push_back(route);
    ConfiguredProcessingNode reduce;reduce.id=10;reduce.kind="ScalarReduce";ReduceProcessingParameters reduction;reduction.selected_channels={0,4294967295u};reduction.channels=ReductionChannels::SelectedChannels;reduce.parameters=reduction;g.nodes.push_back(reduce);
    ConfiguredProcessingNode endpoint;endpoint.id=11;endpoint.kind="TestSignal";endpoint.parameters=EndpointProcessingParameters{{{"amplitude",NumericLiteral{18446744073709551615ULL}}}};g.nodes.push_back(endpoint);
    ConfiguredProcessingNode transform;transform.id=12;transform.kind="SpectrumGain";transform.parameters=TransformProcessingParameters{{{"gain",NumericLiteral{std::numeric_limits<std::int64_t>::min()}}}};g.nodes.push_back(transform);
    QByteArray graph;CHECK(encode_numeric_graph(g,graph));NumericProcessingGraph read;CHECK(decode_numeric_graph(graph,read));
    for(std::size_t i=0;i<g.nodes.size();++i)CHECK(same_processing_parameters(g.nodes[i].parameters,read.nodes[i].parameters));
    NumericProjectDocument source,staged;CHECK(read_numeric_project("{\"schema_version\":2,\"ui\":{\"unknown\":9007199254740993}}",source));CHECK(stage_numeric_draft(source,g,staged));
    QTemporaryDir dir;auto path=dir.filePath("config.json");CHECK(save_numeric_project(staged,path));auto baseline=staged.disk_baseline;
#ifdef _WIN32
    const auto wide=path.toStdWString();auto lock=CreateFileW(wide.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);CHECK(lock!=INVALID_HANDLE_VALUE);
    auto result=save_numeric_project(staged);CloseHandle(lock);CHECK(!result);CHECK(staged.disk_baseline==baseline);CHECK(staged.active_path==path);
    QFile file(path);CHECK(file.open(QIODevice::ReadOnly));CHECK(file.readAll()==baseline);file.close();CHECK(save_numeric_project(staged));
#endif
    return true;
}
static bool draft_applied_and_atomic_publication() {
    NumericProjectDocument document;CHECK(read_numeric_project("{\"schema_version\":2}",document));
    auto draft=sample();CHECK(stage_numeric_draft(document,draft,document));
    std::optional<NumericProcessingGraph> applied=sample();CHECK(read_numeric_applied(document,applied));CHECK(!applied);
    NumericDraftPresentation change;change.applied_graph=draft;
    CHECK(stage_numeric_presentation(document,change,document));
    CHECK(read_numeric_applied(document,applied));CHECK(applied && applied->nodes[0].enabled);
    // A later draft edit must neither Apply nor rewrite the saved applied graph.
    draft.nodes[0].enabled=false;CHECK(stage_numeric_draft(document,draft,document));
    NumericProcessingGraph loaded;CHECK(read_numeric_draft(document,loaded));CHECK(!loaded.nodes[0].enabled);
    CHECK(read_numeric_applied(document,applied));CHECK(applied->nodes[0].enabled);
    QTemporaryDir dir;CHECK(dir.isValid());const auto path=dir.filePath("separate.json");CHECK(save_numeric_project(document,path));
    NumericProjectDocument disk;CHECK(load_numeric_project(path,disk,loaded));CHECK(!loaded.nodes[0].enabled);
    CHECK(read_numeric_applied(disk,applied));CHECK(applied->nodes[0].enabled);
    CHECK(same_processing_parameters(loaded.nodes[0].parameters,applied->nodes[0].parameters));
    const auto source_before=std::string(disk.source.bytes());const auto baseline=disk.disk_baseline;const auto active_path=disk.active_path;
    // A combined applied/UI edit cannot publish its early stages if UI fails.
    change.applied_graph=draft;change.positions["999"]=QJsonObject{{"x",4}};
    CHECK(!stage_numeric_presentation(disk,change,disk));CHECK(disk.source.bytes()==source_before);
    CHECK(disk.disk_baseline==baseline && disk.active_path==active_path);
    change.positions={};change.applied_graph->versions.processing=3;
    CHECK(!stage_numeric_presentation(disk,change,disk));CHECK(disk.source.bytes()==source_before);
    NumericProjectDocument malformed;CHECK(project_storage::parse_project_source("{\"schema_version\":2,\"pipeline_graph\":{\"version\":4}}",malformed.source));
    CHECK(!read_numeric_applied(malformed,applied));CHECK(applied && applied->nodes[0].enabled);
    CHECK(QFile::remove(path));CHECK(!save_numeric_project(disk));CHECK(!QFile::exists(path));
    CHECK(disk.disk_baseline==baseline && disk.active_path==active_path && disk.source.bytes()==source_before);
    return true;
}
static bool editor_snapshot_boundary() {
    auto graph=sample();auto input=graph.nodes[0].outputs[0];input.direction=PortDirection::Input;graph.nodes[0].inputs.push_back(input);auto second=graph.nodes[0];second.id=19;graph.nodes.push_back(second);graph.edges.push_back({7,19,9,0,3});
    QByteArray canonical;CHECK(encode_numeric_graph(graph,canonical));NumericProcessingGraph decoded;
    CHECK(decode_numeric_editor_graph(canonical,decoded));CHECK(decoded.edges==graph.edges);
    QByteArray first_node,second_node;CHECK(encode_numeric_node(graph.nodes[0],first_node));CHECK(encode_numeric_node(second,second_node));
    const auto visual_node=[](int id,const QByteArray& typed){return QJsonObject{{"id",id},{"internal-data",QJsonObject{{"model-name","Converter"},{"numeric_processing",QString::fromUtf8(typed)}}}};};
    QJsonObject visual{{"version",3},{"processing_version",2},{"graph_id","main"},{"nodes",QJsonArray{visual_node(7,first_node),visual_node(19,second_node)}},{"connections",QJsonArray{QJsonObject{{"outNodeId",7},{"outPortIndex",9},{"inNodeId",19},{"inPortIndex",0},{"conversion",3}}}}};
    const auto encoded=QJsonDocument(visual).toJson(QJsonDocument::Compact);
    CHECK(decode_numeric_editor_graph(encoded,decoded));CHECK(decoded.graph_id=="main" && decoded.nodes.size()==2 && decoded.edges==graph.edges);
    const auto& policy=std::get<ConverterProcessingParameters>(decoded.nodes[0].parameters).policy;
    CHECK(std::get<std::uint64_t>(policy.scaling.gain.numerator)==18446744073709551615ULL);
    CHECK(std::get<std::int64_t>(policy.scaling.offset.numerator)==std::numeric_limits<std::int64_t>::min());
    CHECK(std::bit_cast<std::uint32_t>(std::get<float>(policy.nan_replacement))==0x7fc00001u);
    CHECK(std::bit_cast<std::uint64_t>(std::get<double>(policy.negative_inf_replacement))==0x8000000000000000ULL);
    const auto unchanged=[&](){return decoded.graph_id=="main" && decoded.nodes.size()==2 && decoded.edges==graph.edges && same_processing_parameters(decoded.nodes[0].parameters,graph.nodes[0].parameters);};
    for(auto field:{"version","processing_version"}){auto bad=visual;bad[field]=99;CHECK(!decode_numeric_editor_graph(QJsonDocument(bad).toJson(),decoded));CHECK(unchanged());}
    auto bad=visual;bad["graph_id"]="";CHECK(!decode_numeric_editor_graph(QJsonDocument(bad).toJson(),decoded));CHECK(unchanged());
    auto nodes=visual["nodes"].toArray();nodes[0]=visual_node(8,first_node);bad=visual;bad["nodes"]=nodes;CHECK(!decode_numeric_editor_graph(QJsonDocument(bad).toJson(),decoded));CHECK(unchanged());
    nodes=visual["nodes"].toArray();nodes[1]=nodes[0];bad["nodes"]=nodes;CHECK(!decode_numeric_editor_graph(QJsonDocument(bad).toJson(),decoded));CHECK(unchanged());
    nodes=visual["nodes"].toArray();nodes[1]=QJsonDocument::fromJson(canonical)["nodes"].toArray()[1];bad["nodes"]=nodes;CHECK(!decode_numeric_editor_graph(QJsonDocument(bad).toJson(),decoded));CHECK(unchanged());
    bad=visual;auto edges=bad["connections"].toArray();edges.append(edges[0]);bad["connections"]=edges;CHECK(!decode_numeric_editor_graph(QJsonDocument(bad).toJson(),decoded));CHECK(unchanged());
    NumericProjectDocument document;CHECK(read_numeric_project("{\"schema_version\":2}",document));
    CHECK(!read_numeric_project("{\"schema_version\":2,\"scene_graph\":"+encoded+"}",document));CHECK(document.source.bytes()=="{\"schema_version\":2}");
    return true;
}
static bool retained_layout_identity_oracles() {
    auto graph=sample();auto input=graph.nodes[0].outputs[0];input.direction=PortDirection::Input;graph.nodes[0].inputs.push_back(input);auto second=graph.nodes[0];second.id=19;graph.nodes.push_back(second);graph.edges.push_back({7,19,0,0});
    QByteArray snapshot;CHECK(encode_numeric_graph(graph,snapshot));
    snapshot.replace("\"kind\":\"Converter\"","\"kind\":\"Converter\",\"position\":{\"x\":12,\"y\":34},\"local_node_extension\":1e9999");
    snapshot.replace("\"out_id\":7","\"local_edge_extension\":9007199254740993,\"out_id\":7");
    CHECK(snapshot.contains("local_edge_extension"));NumericProjectDocument document;
    CHECK(read_numeric_project("{\"schema_version\":2,\"scene_graph\":"+snapshot+"}",document));
    NumericDraftPresentation restored;CHECK(read_numeric_presentation(document,restored));CHECK(restored.positions.size()==2);
    const auto unchanged=std::string(document.source.bytes());auto foreign=graph;foreign.graph_id="foreign";
    CHECK(!stage_numeric_draft(document,foreign,document));CHECK(document.source.bytes()==unchanged);
    graph.nodes[0].kind="Spectrogram";graph.nodes[0].parameters=HistoryProcessingParameters{8};
    CHECK(stage_numeric_draft(document,graph,document));CHECK(read_numeric_presentation(document,restored));
    CHECK(!restored.positions.contains("7") && restored.positions.contains("19"));
    CHECK(document.source.bytes().find("local_edge_extension")!=std::string_view::npos);
    graph.edges[0].output_port=1;CHECK(stage_numeric_draft(document,graph,document));
    CHECK(document.source.bytes().find("local_edge_extension")==std::string_view::npos);
    graph.nodes.erase(graph.nodes.begin()+1);graph.edges.clear();CHECK(stage_numeric_draft(document,graph,document));
    CHECK(read_numeric_presentation(document,restored));CHECK(restored.positions.isEmpty());
    CHECK(document.source.bytes().find("local_node_extension")==std::string_view::npos);
    return true;
}
static bool settings_arrays_and_opaque_refusal() {
    NumericProjectDocument document;CHECK(read_numeric_project(R"({"schema_version":2,"ui":{"camera":{"position":[1.2300E+000,2,-0.000E+000],"opaque":1e9999},"graph_rendering":{"items":[{"id":"a","value":1.2300E+000,"opaque":7},{"id":"b","value":2,"opaque":9}]},"unsafe":[9007199254740993,2]}})",document));
    auto graph=sample();CHECK(stage_numeric_draft(document,graph,document));NumericDraftPresentation change;
    change.application_settings=QJsonObject{{"ui",QJsonObject{{"camera",QJsonObject{{"position",QJsonArray{1.23,3,0}}}},{"graph_rendering",QJsonObject{{"items",QJsonArray{QJsonObject{{"id","b"},{"value",4}},QJsonObject{{"id","a"},{"value",1.23}}}}}}}}};
    CHECK(stage_numeric_presentation(document,change,document));const auto raw=document.source.bytes();
    CHECK(raw.find("[1.2300E+000,3,-0.000E+000]")!=raw.npos);
    CHECK(raw.find("\"opaque\":1e9999")!=raw.npos && raw.find("\"opaque\":7")!=raw.npos && raw.find("\"opaque\":9")!=raw.npos);
    QJsonObject settings;CHECK(read_numeric_application_settings(document,settings));
    const auto items=settings["ui"].toObject()["graph_rendering"].toObject()["items"].toArray();CHECK(items[0].toObject()["id"]=="b" && items[0].toObject()["value"]==4);CHECK(items[1].toObject()["id"]=="a");
    const auto before=std::string(document.source.bytes());
    change.application_settings=QJsonObject{{"ui",QJsonObject{{"theme","light"},{"unsafe",QJsonArray{0,2}}}}};
    CHECK(!stage_numeric_presentation(document,change,document));CHECK(document.source.bytes()==before);
#ifdef NUMERIC_CONFIG_MANAGER_TESTS
    QTemporaryDir dir;CHECK(dir.isValid());ConfigManager manager(dir.filePath("vectors.json"));CHECK(manager.saveNumeric(document,graph));
    FileSaveApplicationConfig config;CHECK(manager.readNumericSettings(document,config));config.ui.camera["position"]=QJsonArray{1.23,5,0};
    CHECK(manager.numericSettingsDelta(document,config,change.application_settings));CHECK(manager.saveNumeric(document,graph,change));
    CHECK(manager.loadNumeric(document,graph));CHECK(manager.readNumericSettings(document,config));CHECK(config.ui.camera["position"].toArray()[1]==5);
    CHECK(document.source.bytes().find("[1.2300E+000,5,-0.000E+000]")!=std::string_view::npos);
#endif
    return true;
}
static bool fresh_project_and_node_presentation() {
    const auto graph=sample();NumericProjectDocument document;
    NumericDraftPresentation presentation;presentation.positions["7"]=QJsonObject{{"x",31},{"y",47}};
    const auto flags=QJsonObject{{"inline_parameters",QJsonArray{"enabled","converter.overflow"}}};presentation.node_presentations["7"]=flags;
    CHECK(create_numeric_project(graph,presentation,document));CHECK(document.active_path.isEmpty() && document.disk_baseline.isEmpty());
    std::optional<NumericProcessingGraph> applied=graph;CHECK(read_numeric_applied(document,applied));CHECK(!applied);
    NumericDraftPresentation restored;CHECK(read_numeric_presentation(document,restored));CHECK(restored.positions==presentation.positions && restored.node_presentations==presentation.node_presentations);
    NumericProcessingGraph decoded;CHECK(read_numeric_draft(document,decoded));CHECK(same_processing_parameters(graph.nodes[0].parameters,decoded.nodes[0].parameters));
    QByteArray before,after;CHECK(encode_numeric_graph(graph,before));CHECK(encode_numeric_graph(decoded,after));CHECK(before==after);CHECK(!after.contains("inline_parameters"));
    // Unknown raw presentation metadata is owned independently of the UI flags.
    auto raw=QByteArray(document.source.bytes().data(),static_cast<qsizetype>(document.source.bytes().size()));
    raw.replace("\"presentation\":{","\"presentation\":{\"future\":18446744073709551616,");CHECK(read_numeric_project(raw,document));
    presentation.positions={};presentation.node_presentations["7"]=QJsonObject{{"inline_parameters",QJsonArray{"enabled"}}};
    CHECK(stage_numeric_presentation(document,presentation,document));CHECK(document.source.bytes().find("18446744073709551616")!=std::string_view::npos);
    CHECK(stage_numeric_draft(document,graph,document));CHECK(read_numeric_presentation(document,restored));CHECK(restored.node_presentations==presentation.node_presentations);
    const auto unchanged=std::string(document.source.bytes());
    for(auto invalid:{QJsonArray{"enabled","enabled"},QJsonArray{42},QJsonArray{""}}){presentation.node_presentations["7"]=QJsonObject{{"inline_parameters",invalid}};CHECK(!stage_numeric_presentation(document,presentation,document));CHECK(document.source.bytes()==unchanged);}
    presentation.node_presentations={};presentation.node_presentations["999"]=flags;CHECK(!stage_numeric_presentation(document,presentation,document));CHECK(document.source.bytes()==unchanged);
    presentation.node_presentations={};presentation.node_presentations["7"]=QJsonObject{{"inline_parameters",QJsonArray{}}};
#ifdef NUMERIC_CONFIG_MANAGER_TESTS
    QTemporaryDir dir;CHECK(dir.isValid());ConfigManager manager(dir.filePath("flags.json"));
    CHECK(manager.saveNumeric(document,graph,presentation));CHECK(manager.loadNumeric(document,decoded));
    CHECK(read_numeric_presentation(document,restored));CHECK(restored.node_presentations==presentation.node_presentations);CHECK(document.source.bytes().find("18446744073709551616")!=std::string_view::npos);
#else
    CHECK(stage_numeric_presentation(document,presentation,document));CHECK(read_numeric_presentation(document,restored));CHECK(restored.node_presentations==presentation.node_presentations);
#endif
    // Constructing a startup candidate never borrows or overwrites a loaded project.
    NumericProjectDocument fresh;CHECK(create_numeric_startup_project(graph,fresh));CHECK(fresh.active_path.isEmpty() && fresh.disk_baseline.isEmpty());
    CHECK(document.source.bytes().find("18446744073709551616")!=std::string_view::npos);
    QJsonObject settings;CHECK(read_numeric_application_settings(fresh,settings));const auto presets=settings["ui"].toObject()["pipeline_presets"].toArray();
    for(auto key:{"show_raw_samples","show_amplitudes","show_test_amplitudes","show_spectrogram"})CHECK(settings["ui"].toObject()[key]==true);
    CHECK(presets.size()==1 && presets[0].toObject()["id"]=="builtin-test-signal" && presets[0].toObject()["version"]==1);
    CHECK(decode_numeric_editor_graph(QJsonDocument(presets[0].toObject()["graph"].toObject()).toJson(),decoded));CHECK(decoded.graph_id==graph.graph_id);
    CHECK(read_numeric_applied(fresh,applied));CHECK(!applied);
    const auto fresh_before=std::string(fresh.source.bytes());auto bad_graph=graph;bad_graph.versions.graph=99;
    CHECK(!create_numeric_startup_project(bad_graph,fresh));CHECK(fresh.source.bytes()==fresh_before);
    return true;
}
static bool node_display_name_persistence() {
    const auto graph=sample();NumericProjectDocument document;NumericDraftPresentation delta;
    delta.node_presentations["7"]=QJsonObject{{"inline_parameters",QJsonArray{"converter.overflow"}}};
    CHECK(create_numeric_project(graph,delta,document));
    auto raw=QByteArray(document.source.bytes().data(),qsizetype(document.source.bytes().size()));
    raw.replace("\"presentation\":{","\"presentation\":{\"future\":18446744073709551616,");
    CHECK(read_numeric_project(raw,document));
    const auto name=QString::fromUtf8("Мой преобразователь");
    delta.node_presentations["7"]=QJsonObject{{"display_name",name}};
    CHECK(stage_numeric_presentation(document,delta,document));
    NumericDraftPresentation projected;CHECK(read_numeric_presentation(document,projected));
    CHECK(projected.node_presentations["7"].toObject()["display_name"]==name);
    CHECK(projected.node_presentations["7"].toObject()["inline_parameters"].toArray()==QJsonArray{"converter.overflow"});
    CHECK(document.source.bytes().find("18446744073709551616")!=std::string_view::npos);
    NumericProcessingGraph draft;QByteArray expected,actual;CHECK(read_numeric_draft(document,draft));
    CHECK(encode_numeric_graph(graph,expected) && encode_numeric_graph(draft,actual));CHECK(expected==actual);
    delta.node_presentations["7"]=QJsonObject{{"display_name",QString(256,QChar(0x0416))}};
    CHECK(stage_numeric_presentation(document,delta,document));CHECK(read_numeric_presentation(document,projected));
    CHECK(projected.node_presentations["7"].toObject()["display_name"].toString()==QString(256,QChar(0x0416)));
    const auto unchanged=std::string(document.source.bytes());
    for(const QJsonValue invalid:{QJsonValue(42),QJsonValue(QString(257,'x')),QJsonValue(QString("bad")+QChar(1))}){
        delta.node_presentations["7"]=QJsonObject{{"display_name",invalid}};
        CHECK(!stage_numeric_presentation(document,delta,document));CHECK(document.source.bytes()==unchanged);
    }
    delta.node_presentations["7"]=QJsonObject{{"scalar_inputs",QJsonObject{{"0",QJsonObject{{"display_name","Count"},{"enabled",false}}},{"3",QJsonObject{{"display_name","Peak"},{"enabled",true}}}}}};
    CHECK(stage_numeric_presentation(document,delta,document));CHECK(read_numeric_presentation(document,projected));
    CHECK(projected.node_presentations["7"].toObject()["scalar_inputs"].toObject().value("0").toObject().value("enabled")==false);
    delta.node_presentations["7"]=QJsonObject{{"scalar_inputs",QJsonObject{{"3",QJsonObject{{"display_name","Peak"},{"enabled",true}}}}}};
    CHECK(stage_numeric_presentation(document,delta,document));CHECK(read_numeric_presentation(document,projected));
    CHECK(!projected.node_presentations["7"].toObject()["scalar_inputs"].toObject().contains("0"));
    delta.node_presentations["7"]=QJsonObject{{"display_name",""}};
    CHECK(stage_numeric_presentation(document,delta,document));CHECK(read_numeric_presentation(document,projected));
    CHECK(projected.node_presentations["7"].toObject()["display_name"].isString() && projected.node_presentations["7"].toObject()["display_name"].toString().isEmpty());
    QTemporaryDir dir;CHECK(dir.isValid());const auto path=dir.filePath("named.json");CHECK(save_numeric_project(document,path));
    NumericProjectDocument reloaded;CHECK(load_numeric_project(path,reloaded,draft));CHECK(read_numeric_presentation(reloaded,projected));
    CHECK(projected.node_presentations["7"].toObject()["display_name"].toString().isEmpty());
    CHECK(reloaded.source.bytes().find("18446744073709551616")!=std::string_view::npos);
    return true;
}
static bool preset_display_snapshot_roundtrip() {
    const auto graph=sample();QByteArray snapshot;CHECK(encode_numeric_graph(graph,snapshot));
    const QByteArray display=R"({"version":1,"show_raw_samples":true,"show_amplitudes":false,"show_spectrogram":false,"show_data_flow":true,"graph_views":{"fft":{"open":true,"mode":"3d","opaque":9007199254740993}},"graph_rendering":{"version":1,"defaults":{"axis_precision":4},"future":1e9999},"workspace":{"version":1,"layout_version":5,"backend":"qt-ads","ads_state":"saved-tabs","geometry":"saved-geometry","visibility":{"fft":true,"dataFlow":true},"opaque":18446744073709551616},"opaque":1e9999})";
    QJsonObject projected;CHECK(decode_numeric_preset_presentation(display,projected));CHECK(projected["show_raw_samples"]==true && projected["show_amplitudes"]==false);
    CHECK(!projected.contains("opaque"));CHECK(!projected["graph_views"].toObject()["fft"].toObject().contains("opaque"));
    CHECK(projected["workspace"].toObject()["ads_state"]=="saved-tabs");
    NumericProjectDocument source;const auto raw="{\"schema_version\":2,\"scene_graph\":"+snapshot+",\"pipeline_graph\":"+snapshot+",\"ui\":{\"pipeline_presets\":[{\"version\":1,\"id\":\"scene\",\"name\":\"Scene\",\"graph\":"+snapshot+",\"presentation\":"+display+"}]}}";
    CHECK(read_numeric_project(raw,source));QJsonObject settings;CHECK(read_numeric_application_settings(source,settings));
    auto presets=settings["ui"].toObject()["pipeline_presets"].toArray();CHECK(presets.size()==1);
    auto preset=presets[0].toObject();auto saved=preset["presentation"].toObject();saved["show_raw_samples"]=false;
    saved["graph_views"]=QJsonObject{};saved["graph_rendering"]=QJsonObject{};saved["workspace"]=QJsonObject{};preset["presentation"]=saved;presets[0]=preset;
    NumericDraftPresentation delta;delta.application_settings=QJsonObject{{"ui",QJsonObject{{"pipeline_presets",presets}}}};
    CHECK(stage_numeric_presentation(source,delta,source));CHECK(read_numeric_application_settings(source,settings));
    const auto restored=settings["ui"].toObject()["pipeline_presets"].toArray()[0].toObject()["presentation"].toObject();
    CHECK(restored["show_raw_samples"]==false && !restored["graph_views"].toObject().contains("fft"));
    CHECK(!restored["graph_rendering"].toObject().contains("defaults") && !restored["workspace"].toObject().contains("ads_state"));
    for(auto lexeme:{"1e9999","18446744073709551616"})CHECK(source.source.bytes().find(lexeme)!=std::string_view::npos);
    std::optional<NumericProcessingGraph> applied;CHECK(read_numeric_applied(source,applied));CHECK(applied && same_processing_parameters(applied->nodes[0].parameters,graph.nodes[0].parameters));
    QTemporaryDir dir;CHECK(dir.isValid());const auto path=dir.filePath("display.json");CHECK(save_numeric_project(source,path));NumericProjectDocument reloaded;NumericProcessingGraph draft;CHECK(load_numeric_project(path,reloaded,draft));
    CHECK(read_numeric_application_settings(reloaded,settings));CHECK(settings["ui"].toObject()["pipeline_presets"].toArray()[0].toObject()["presentation"]==restored);
    const auto unchanged=std::string(source.source.bytes());const auto projection_before=projected;
    for(auto invalid:{QByteArray("{\"version\":99}"),QByteArray("{\"version\":1,\"version\":1}"),QByteArray("{\"version\":1,\"show_spectrogram\":0}"),QByteArray("{\"version\":1,\"workspace\":[]}"),QByteArray("{\"version\":1,\"workspace\":{\"layout_version\":1.5}}"),QByteArray("{\"version\":1,\"workspace\":{\"visibility\":{\"fft\":0}}}"),QByteArray("{\"version\":1,\"graph_views\":{\"fft\":{\"open\":1}}}")}){
        CHECK(!decode_numeric_preset_presentation(invalid,projected));CHECK(projected==projection_before);
    }
    preset["presentation"]=QJsonObject{{"version",99}};presets[0]=preset;delta.application_settings=QJsonObject{{"ui",QJsonObject{{"pipeline_presets",presets}}}};
    CHECK(!stage_numeric_presentation(source,delta,source));CHECK(source.source.bytes()==unchanged);
    return true;
}
static bool node_adapter_storage_and_projection() {
    const auto graph=sample();const QByteArray raw=R"({"provider_uuid":"11111111-1111-4111-8111-111111111111","stream_id":"mel-main","host":"stale-host","port":8213,"device_identity":"numeric-desktop","remote_endpoint":{"graph_id":"publisher","node_id":19,"direction":"output","port_index":0,"opaque":9007199254740993},"opaque":1e9999})";
    QJsonObject adapter;CHECK(decode_numeric_node_adapter(raw,adapter));CHECK(!adapter.contains("opaque") && !adapter["remote_endpoint"].toObject().contains("opaque"));
    NumericDraftPresentation change;change.node_adapters["7"]=adapter;NumericProjectDocument document;CHECK(create_numeric_project(graph,change,document));
    auto source=QByteArray(document.source.bytes().data(),static_cast<qsizetype>(document.source.bytes().size()));
    source.replace(QJsonDocument(adapter).toJson(QJsonDocument::Compact),raw);CHECK(read_numeric_project(source,document));
    NumericDraftPresentation restored;CHECK(read_numeric_presentation(document,restored));CHECK(restored.node_adapters==change.node_adapters);
    NumericProcessingGraph processing;CHECK(read_numeric_draft(document,processing));QByteArray before,after;CHECK(encode_numeric_graph(graph,before));CHECK(encode_numeric_graph(processing,after));CHECK(before==after && !after.contains("provider_uuid"));
    // An explicit full replacement clears provider priority, not just host values.
    const QJsonObject direct{{"host","127.0.0.1"},{"port",8214},{"remote_endpoint",QJsonObject{{"graph_id","direct"},{"node_id",4},{"direction","input"},{"port_index",1}}}};
    change.node_adapters["7"]=direct;CHECK(stage_numeric_presentation(document,change,document));CHECK(read_numeric_presentation(document,restored));CHECK(restored.node_adapters["7"]==direct);
    CHECK(document.source.bytes().find("provider_uuid")==std::string_view::npos);CHECK(document.source.bytes().find("1e9999")!=std::string_view::npos && document.source.bytes().find("9007199254740993")!=std::string_view::npos);
    const auto unchanged=std::string(document.source.bytes());const auto old=adapter;
    for(auto invalid:{QByteArray("{\"provider_uuid\":\"bad\"}"),QByteArray("{\"host\":\"\"}"),QByteArray("{\"port\":0}"),QByteArray("{\"port\":65536}"),QByteArray("{\"port\":1,\"port\":2}"),QByteArray("{\"remote_endpoint\":{\"node_id\":2147483648}}"),QByteArray("{\"remote_endpoint\":{\"direction\":\"invalid\"}}")}){
        CHECK(!decode_numeric_node_adapter(invalid,adapter));CHECK(adapter==old);
    }
    change.node_adapters["7"]=QJsonObject{{"port",0}};CHECK(!stage_numeric_presentation(document,change,document));CHECK(document.source.bytes()==unchanged);
    change.node_adapters["7"]=QJsonObject{{"host","peer"},{"unregistered",true}};CHECK(!stage_numeric_presentation(document,change,document));CHECK(document.source.bytes()==unchanged);
    change.node_adapters["7"]=QJsonObject{{"host","partial-peer"}};CHECK(stage_numeric_presentation(document,change,document));CHECK(read_numeric_presentation(document,restored));CHECK(restored.node_adapters["7"]==change.node_adapters["7"]);
    change.node_adapters["7"]=QJsonObject{};CHECK(stage_numeric_presentation(document,change,document));CHECK(read_numeric_presentation(document,restored));CHECK(restored.node_adapters["7"].toObject().isEmpty());
    CHECK(document.source.bytes().find("1e9999")!=std::string_view::npos && document.source.bytes().find("9007199254740993")!=std::string_view::npos);
#ifdef NUMERIC_CONFIG_MANAGER_TESTS
    QTemporaryDir dir;CHECK(dir.isValid());ConfigManager manager(dir.filePath("routing.json"));CHECK(manager.saveNumeric(document,graph));
    change.node_adapters["7"]=direct;CHECK(manager.saveNumeric(document,graph,change));CHECK(manager.loadNumeric(document,processing));CHECK(read_numeric_presentation(document,restored));CHECK(restored.node_adapters["7"]==direct);
#endif
    return true;
}
static bool presentation_vertical() {
    NumericProjectDocument settings_only;std::uint32_t edition=99;
    CHECK(read_numeric_project("{\"schema_version\":2,\"ui\":{\"theme\":\"dark\"}}",settings_only));CHECK(!read_project_graph_edition(settings_only,edition));CHECK(edition==99);
    auto g=sample();NumericProjectDocument base,typed,layout;
    CHECK(read_numeric_project(R"({"schema_version":2,"ui":{"unknown":9007199254740993,"editor_view":{"version":1,"scale":1.2300E+000,"opaque":1e9999}}})",base));
    CHECK(stage_numeric_draft(base,g,typed));NumericDraftPresentation change;
    change.positions["7"]=QJsonObject{{"x",17.5},{"y",-28.25}};change.editor_view=QJsonObject{{"version",1},{"scale",1.5},{"center_x",11.0},{"center_y",12.0}};
    CHECK(stage_numeric_presentation(typed,change,layout));NumericDraftPresentation read;CHECK(read_numeric_presentation(layout,read));CHECK(read.positions==change.positions);CHECK(read.editor_view==change.editor_view);
    CHECK(layout.source.bytes().find("9007199254740993")!=std::string_view::npos);CHECK(layout.source.bytes().find("1e9999")!=std::string_view::npos);
    NumericDraftPresentation bad;bad.positions["7"]=QJsonObject{{"hidden-field",7}};auto baseline=std::string(layout.source.bytes());CHECK(!stage_numeric_presentation(layout,bad,layout));CHECK(layout.source.bytes()==baseline);
    NumericDraftPresentation preferences;preferences.application_settings=QJsonObject{{"ui",QJsonObject{{"theme","dark"},{"camera",QJsonObject{{"zoom",2.0}}}}}};
    CHECK(stage_numeric_presentation(layout,preferences,layout));QJsonObject settings;CHECK(read_numeric_application_settings(layout,settings));CHECK(settings["ui"].toObject()["theme"]=="dark");CHECK(settings["ui"].toObject()["camera"].toObject()["zoom"]==2.0);CHECK(layout.source.bytes().find("1e9999")!=std::string_view::npos);
    preferences.application_settings=QJsonObject{{"ui",QJsonObject{{"unknown",0}}}};baseline=std::string(layout.source.bytes());CHECK(!stage_numeric_presentation(layout,preferences,layout));CHECK(layout.source.bytes()==baseline);
    QTemporaryDir dir;auto path=dir.filePath("layout.json");CHECK(save_numeric_project(layout,path));NumericProjectDocument disk;NumericProcessingGraph graph;CHECK(load_numeric_project(path,disk,graph));CHECK(read_numeric_presentation(disk,read));CHECK(read.positions==change.positions);
#ifdef NUMERIC_CONFIG_MANAGER_TESTS
    ConfigManager manager(path);change.positions["7"]=QJsonObject{{"x",29.0},{"y",41.0}};CHECK(manager.saveNumeric(disk,graph,change));CHECK(manager.loadNumeric(disk,graph));CHECK(read_numeric_presentation(disk,read));CHECK(read.positions==change.positions);
    FileSaveApplicationConfig config;CHECK(manager.readNumericSettings(disk,config));CHECK(config.ui.theme=="dark");QJsonObject delta;CHECK(manager.numericSettingsDelta(disk,config,delta));CHECK(delta.isEmpty());config.ui.theme="light";CHECK(manager.numericSettingsDelta(disk,config,delta));change.application_settings=delta;CHECK(manager.saveNumeric(disk,graph,change));CHECK(manager.readNumericSettings(disk,config));CHECK(config.ui.theme=="light");
    config.ui.pipeline_presets=QJsonArray{QJsonObject{{"version",1},{"id","stable"},{"name","explicit"}}};CHECK(manager.numericSettingsDelta(disk,config,delta));change.application_settings=delta;CHECK(manager.saveNumeric(disk,graph,change));
    config.ui.pipeline_presets=QJsonArray{QJsonObject{{"version",1},{"id","stable"},{"name","changed"}}};CHECK(manager.numericSettingsDelta(disk,config,delta));change.application_settings=delta;CHECK(manager.saveNumeric(disk,graph,change));CHECK(manager.readNumericSettings(disk,config));CHECK(config.ui.pipeline_presets[0].toObject()["name"]=="changed");change.application_settings={};
    auto other=dir.filePath("existing.json");QFile target(other);CHECK(target.open(QIODevice::WriteOnly));target.write("{\"schema_version\":2,\"old\":1}");target.close();CHECK(manager.saveNumericAs(disk,graph,change,other));CHECK(manager.getConfigPath()==other);
#endif
    return true;
}
#ifdef NUMERIC_CONFIG_MANAGER_TESTS
static bool preset_vertical() {
    QTemporaryDir dir;CHECK(dir.isValid());const auto path=dir.filePath("presets.json");ConfigManager manager(path);QString error;
    auto graph=sample();NumericProjectDocument document;CHECK(read_numeric_project("{\"schema_version\":2,\"ui\":{\"opaque\":1e9999}}",document));CHECK(manager.saveNumeric(document,graph,&error));
    QByteArray typed;CHECK(encode_numeric_node(graph.nodes[0],typed));
    const auto preset_flags=QJsonObject{{"inline_parameters",QJsonArray{"enabled"}}};
    QJsonObject saved_graph{{"version",3},{"processing_version",2},{"graph_id","main"},{"nodes",QJsonArray{QJsonObject{{"id",7},{"internal-data",QJsonObject{{"model-name","Converter"},{"numeric_processing",QString::fromUtf8(typed)},{"presentation",preset_flags}}},{"position",QJsonObject{{"x",11.0},{"y",12.0}}}}}},{"connections",QJsonArray{}}};
    auto preset=[&](QString id,QString name){return QJsonObject{{"version",1},{"id",id},{"name",name},{"graph",saved_graph},{"view",QJsonObject{{"version",1},{"scale",1.0}}}};};
    FileSaveApplicationConfig settings;CHECK(manager.readNumericSettings(document,settings,&error));settings.ui.pipeline_presets=QJsonArray{preset("first","First")};NumericDraftPresentation change;
    CHECK(manager.numericSettingsDelta(document,settings,change.application_settings,&error));CHECK(manager.saveNumeric(document,graph,change,&error));const auto first_bytes=std::string(document.source.bytes());
    settings.ui.pipeline_presets.append(preset("second","Second"));CHECK(manager.numericSettingsDelta(document,settings,change.application_settings,&error));const auto second=manager.saveNumeric(document,graph,change,&error);
    CHECK(second);CHECK(manager.loadNumeric(document,graph,&error));CHECK(manager.readNumericSettings(document,settings,&error));CHECK(settings.ui.pipeline_presets.size()==2);
    // Load an owning source with unknown fields at every retained preset/graph
    // depth; the GUI projection must keep identified entries despite raw numbers.
    QByteArray opaque_graph;CHECK(encode_numeric_graph(graph,opaque_graph));opaque_graph.insert(1,"\"graph_opaque\":1e9999,");opaque_graph.replace("\"kind\":\"Converter\"","\"kind\":\"Converter\",\"node_opaque\":9007199254740993");
    auto opaque_typed=typed;opaque_typed.insert(1,"\"inner_opaque\":18446744073709551616,");auto qt_graph=saved_graph;auto nodes=qt_graph["nodes"].toArray();auto node=nodes[0].toObject();auto internal=node["internal-data"].toObject();internal["numeric_processing"]=QString::fromUtf8(opaque_typed);node["internal-data"]=internal;nodes[0]=node;qt_graph["nodes"]=nodes;
    auto qt_bytes=QJsonDocument(qt_graph).toJson(QJsonDocument::Compact);qt_bytes.insert(1,"\"qt_opaque\":-0.000E+000,");qt_bytes.replace("\"position\":{","\"position\":{\"position_opaque\":1e9999,");
    QByteArray source="{\"schema_version\":2,\"scene_graph\":";QByteArray scene;CHECK(encode_numeric_graph(graph,scene));auto applied=scene;applied.insert(1,"\"applied_opaque\":1e9999,");source+=scene+",\"pipeline_graph\":"+applied+",\"ui\":{\"opaque\":1e9999,\"camera\":{\"x\":1.2300E+000},\"pipeline_presets\":[{\"version\":1,\"id\":\"first\",\"name\":\"First\",\"preset_opaque\":9007199254740993,\"graph\":"+qt_bytes+",\"view\":{\"version\":1,\"scale\":1.2300E+000,\"view_opaque\":1e9999}},{\"version\":1,\"id\":\"typed\",\"name\":\"Typed\",\"graph\":"+opaque_graph+"}]}}";
    QFile fixture(path);CHECK(fixture.open(QIODevice::WriteOnly));CHECK(fixture.write(source)==source.size());fixture.close();CHECK(manager.loadNumeric(document,graph,&error));CHECK(manager.readNumericSettings(document,settings,&error));CHECK(settings.ui.pipeline_presets.size()==2);
    auto first=settings.ui.pipeline_presets[0].toObject();first["name"]="Renamed";auto edited=first["graph"].toObject();nodes=edited["nodes"].toArray();node=nodes[0].toObject();node["position"]=QJsonObject{{"x",99.0},{"y",88.0}};internal=node["internal-data"].toObject();auto edited_node=graph.nodes[0];edited_node.enabled=false;QByteArray edited_snapshot;CHECK(encode_numeric_node(edited_node,edited_snapshot));internal["numeric_processing"]=QString::fromUtf8(edited_snapshot);node["internal-data"]=internal;nodes[0]=node;edited["nodes"]=nodes;first["graph"]=edited;
    settings.ui.pipeline_presets=QJsonArray{settings.ui.pipeline_presets[1],first,preset("third","Third")};CHECK(manager.numericSettingsDelta(document,settings,change.application_settings,&error));CHECK(manager.saveNumeric(document,graph,change,&error));
    auto raw=QByteArray(document.source.bytes().data(),static_cast<qsizetype>(document.source.bytes().size()));for(auto token:{"9007199254740993","1e9999","18446744073709551616","-0.000E+000","1.2300E+000","18446744073709551615"})CHECK(raw.contains(token));
    CHECK(raw.contains("\"applied_opaque\":1e9999"));CHECK(raw.contains("\"preset_opaque\":9007199254740993"));CHECK(raw.contains("\"node_opaque\":9007199254740993"));CHECK(raw.contains("\"position_opaque\":1e9999"));CHECK(raw.contains("\"view_opaque\":1e9999"));
    CHECK(manager.loadNumeric(document,graph,&error));CHECK(manager.readNumericSettings(document,settings,&error));CHECK(settings.ui.pipeline_presets.size()==3);CHECK(settings.ui.pipeline_presets[0].toObject()["id"]=="typed");CHECK(settings.ui.pipeline_presets[1].toObject()["name"]=="Renamed");
    auto retained=settings.ui.pipeline_presets[1].toObject();auto reloaded_node=retained["graph"].toObject()["nodes"].toArray()[0].toObject();ConfiguredProcessingNode decoded;CHECK(decode_numeric_node(reloaded_node["internal-data"].toObject()["numeric_processing"].toString().toUtf8(),decoded));CHECK(!decoded.enabled);CHECK(std::get<std::uint64_t>(std::get<ConverterProcessingParameters>(decoded.parameters).policy.scaling.gain.numerator)==std::numeric_limits<std::uint64_t>::max());
    CHECK(reloaded_node["internal-data"].toObject()["presentation"]==preset_flags);
    const auto& policy=std::get<ConverterProcessingParameters>(decoded.parameters).policy;CHECK(std::get<std::int64_t>(policy.scaling.offset.numerator)==std::numeric_limits<std::int64_t>::min());CHECK(std::bit_cast<std::uint32_t>(std::get<float>(policy.nan_replacement))==0x7fc00001u);CHECK(std::bit_cast<std::uint64_t>(std::get<double>(policy.negative_inf_replacement))==0x8000000000000000ULL);
    settings.ui.pipeline_presets.removeAt(2);CHECK(manager.numericSettingsDelta(document,settings,change.application_settings,&error));CHECK(manager.saveNumeric(document,graph,change,&error));const auto target=dir.filePath("presets-as.json");CHECK(manager.saveNumericAs(document,graph,change,target,&error));CHECK(manager.loadNumeric(document,graph,&error));CHECK(manager.readNumericSettings(document,settings,&error));CHECK(settings.ui.pipeline_presets.size()==2);
    const auto unchanged=std::string(document.source.bytes());const auto disk_before=document.disk_baseline;auto bad=settings.ui.pipeline_presets;bad.append(bad[0]);change.application_settings=QJsonObject{{"ui",QJsonObject{{"pipeline_presets",bad}}}};CHECK(!manager.saveNumeric(document,graph,change,&error));CHECK(document.source.bytes()==unchanged);CHECK(document.disk_baseline==disk_before);
    bad=settings.ui.pipeline_presets;auto future=bad[0].toObject();future["version"]=2;bad[0]=future;change.application_settings=QJsonObject{{"ui",QJsonObject{{"pipeline_presets",bad}}}};CHECK(!manager.saveNumeric(document,graph,change,&error));CHECK(document.source.bytes()==unchanged);
    auto malformed=retained;edited=malformed["graph"].toObject();nodes=edited["nodes"].toArray();node=nodes[0].toObject();internal=node["internal-data"].toObject();internal["numeric_processing"]="{}";node["internal-data"]=internal;nodes[0]=node;edited["nodes"]=nodes;malformed["graph"]=edited;change.application_settings=QJsonObject{{"ui",QJsonObject{{"pipeline_presets",QJsonArray{malformed}}}}};CHECK(!manager.saveNumeric(document,graph,change,&error));CHECK(document.source.bytes()==unchanged);
    settings.ui.pipeline_presets.append(preset("fourth","Fourth"));CHECK(manager.numericSettingsDelta(document,settings,change.application_settings,&error));QFile external(target);CHECK(external.open(QIODevice::WriteOnly));const auto changed="{\"schema_version\":2,\"external\":1}";external.write(changed);external.close();CHECK(!manager.saveNumeric(document,graph,change,&error));CHECK(document.source.bytes()==unchanged);CHECK(document.disk_baseline==disk_before);
    std::printf("preset vertical PASS\n");return true;
}
static bool manager_save_as_protected_targets() {
    QTemporaryDir dir;CHECK(dir.isValid());const auto active=dir.filePath("active.json");ConfigManager manager(active);QString error;
    auto graph=sample();NumericProjectDocument document;CHECK(read_numeric_project("{\"schema_version\":2,\"opaque\":1e9999}",document));CHECK(manager.saveNumeric(document,graph,&error));
    const auto target=dir.filePath("protected.json");auto edit=graph;edit.nodes[0].enabled=false;NumericDraftPresentation layout;layout.positions["7"]=QJsonObject{{"x",123},{"y",456}};
    for(const auto& protected_bytes:{QByteArray("{invalid"),QByteArray("{\"schema_version\":99,\"sentinel\":true}"),QByteArray("{\"schema_version\":2,\"scene_graph\":false}")}){
        QFile file(target);CHECK(file.open(QIODevice::WriteOnly|QIODevice::Truncate));CHECK(file.write(protected_bytes)==protected_bytes.size());file.close();
        const auto source_before=std::string(document.source.bytes());const auto baseline_before=document.disk_baseline;const auto path_before=document.active_path;const auto manager_before=manager.getConfigPath();
        CHECK(!manager.saveNumericAs(document,edit,layout,target,&error));CHECK(!error.isEmpty());
        CHECK(document.source.bytes()==source_before && document.disk_baseline==baseline_before && document.active_path==path_before && manager.getConfigPath()==manager_before);
        CHECK(file.open(QIODevice::ReadOnly));CHECK(file.readAll()==protected_bytes);file.close();
        // An explicit supported replacement destination permits retry; rejected
        // candidate edits must not be published until this successful commit.
        CHECK(file.open(QIODevice::WriteOnly|QIODevice::Truncate));file.write("{\"schema_version\":2,\"replaceable\":true}");file.close();
        CHECK(manager.saveNumericAs(document,edit,layout,target,&error));CHECK(error.isEmpty());CHECK(document.active_path==target && manager.getConfigPath()==target);
        NumericProjectDocument loaded;NumericProcessingGraph restored;CHECK(manager.loadNumeric(loaded,restored,&error));CHECK(!restored.nodes[0].enabled);
        NumericDraftPresentation restored_layout;CHECK(read_numeric_presentation(loaded,restored_layout));CHECK(restored_layout.positions==layout.positions);
        CHECK(file.open(QIODevice::ReadOnly));CHECK(file.readAll()==document.disk_baseline);file.close();
        CHECK(document.source.bytes().find("1e9999")!=std::string_view::npos);
        // Re-establish an independent active path before the next refused Save As.
        CHECK(manager.saveNumericAs(document,graph,active,&error));
    }
    return true;
}
static bool preferences_only_source_atomicity() {
    QTemporaryDir dir; CHECK(dir.isValid());
    const auto path=dir.filePath("preferences.json"); ConfigManager manager(path);
    QByteArray applied; CHECK(encode_numeric_graph(sample(),applied));
    // A valid source without any draft must allow an unrelated preferences edit.
    const QByteArray original="{\"schema_version\":2,\"pipeline_graph\":"+applied+
        ",\"ui\":{\"theme\":\"light\",\"language\":\"en\",\"opaque\":1e9999},\"opaque\":9007199254740993}";
    NumericProjectDocument document; CHECK(read_numeric_project(original,document));
    CHECK(save_numeric_project(document,path));
    FileSaveApplicationConfig settings; QString error;
    CHECK(manager.readNumericSettings(document,settings,&error));
    domain::NumericProcessingGraph draft; CHECK(!read_numeric_draft(document,draft));
    settings.ui.theme="dark"; settings.ui.language="ru";
    CHECK(manager.saveNumericPreferences(document,settings,&error)); CHECK(error.isEmpty());
    const auto committed=QByteArray(document.source.bytes().data(),qsizetype(document.source.bytes().size()));
    CHECK(!committed.contains("scene_graph")); CHECK(committed.contains(applied));
    CHECK(committed.contains("1e9999")); CHECK(committed.contains("9007199254740993"));
    FileSaveApplicationConfig restored; CHECK(manager.readNumericSettings(document,restored,&error));
    CHECK(restored.ui.theme=="dark" && restored.ui.language=="ru");
    CHECK(!read_numeric_draft(document,draft));
    // A refused commit never publishes candidate preference changes in memory.
    QFile external(path); CHECK(external.open(QIODevice::WriteOnly|QIODevice::Truncate));
    const QByteArray future="{\"schema_version\":999}"; CHECK(external.write(future)==future.size()); external.close();
    const auto baseline=document.disk_baseline; settings.ui.theme="light";
    CHECK(!manager.saveNumericPreferences(document,settings,&error)); CHECK(!error.isEmpty());
    CHECK(document.disk_baseline==baseline);
    CHECK(QByteArray(document.source.bytes().data(),qsizetype(document.source.bytes().size()))==committed);
    CHECK(external.open(QIODevice::ReadOnly)); CHECK(external.readAll()==future);
    return true;
}

static bool manager_path_and_rollback() {
    QTemporaryDir dir;CHECK(dir.isValid());auto path=dir.filePath("numeric.json");ConfigManager manager(path);
    NumericProjectDocument project;CHECK(read_numeric_project("{\"schema_version\":2,\"ui\":{\"unknown\":1e9999}}",project));auto g=sample();QString error;
    CHECK(manager.saveNumeric(project,g,&error));CHECK(error.isEmpty());NumericProjectDocument loaded;NumericProcessingGraph read;CHECK(manager.loadNumeric(loaded,read,&error));CHECK(same_processing_parameters(g.nodes[0].parameters,read.nodes[0].parameters));
    auto old=manager.getConfigPath();auto raw=std::string(project.source.bytes());CHECK(!manager.saveNumericAs(project,g,QString{},&error));CHECK(manager.getConfigPath()==old);CHECK(project.source.bytes()==raw);
    auto next=dir.filePath("nested/next.json");CHECK(manager.saveNumericAs(project,g,next,&error));CHECK(manager.getConfigPath()==next);CHECK(project.active_path==next);
    QFile external(next);CHECK(external.open(QIODevice::WriteOnly));external.write("{\"schema_version\":100}");external.close();raw=std::string(project.source.bytes());CHECK(!manager.saveNumeric(project,g,&error));CHECK(project.source.bytes()==raw);CHECK(manager.getConfigPath()==next);
    return true;
}
#endif
int main(int argc,char** argv) { QCoreApplication app(argc,argv);
#ifdef NUMERIC_CONFIG_MANAGER_TESTS
    if(app.arguments().contains("--presets-only"))return preset_vertical()?0:1;
#endif
    int failures=0;for(auto test:{exact_clipboard_and_file,refusal_and_output_rollback,alternatives_and_commit_failure,draft_applied_and_atomic_publication,editor_snapshot_boundary,retained_layout_identity_oracles,settings_arrays_and_opaque_refusal,fresh_project_and_node_presentation,node_display_name_persistence,preset_display_snapshot_roundtrip,node_adapter_storage_and_projection,presentation_vertical})failures+=!test();
#ifdef NUMERIC_CONFIG_MANAGER_TESTS
    failures+=!manager_path_and_rollback();
    failures+=!preferences_only_source_atomicity();
    failures+=!manager_save_as_protected_targets();
    failures+=!preset_vertical();
#endif
    std::printf("canonical numeric storage: %d failures\n",failures);return failures?1:0;
}
