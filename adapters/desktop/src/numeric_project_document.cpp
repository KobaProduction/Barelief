#include <desktop/numeric_project_document.h>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QSaveFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QUuid>
#include <bit>
#include <charconv>
#include <limits>
#include <type_traits>
#include <cmath>

namespace desktop {
namespace {
using namespace domain;
using namespace signal_contracts;
using namespace project_storage;
using Source=OwnedProjectSource;
using Handle=NodeHandle;
using NodeKind=project_storage::NodeKind;
constexpr FrameLimits frame_limits{64,16777216,16777216,16777216,1024,16777216};
std::string_view view(QByteArrayView b) { return {b.data(),static_cast<std::size_t>(b.size())}; }
QByteArray bytes(std::string_view s) { return QByteArray(s.data(),static_cast<qsizetype>(s.size())); }
QString text(std::string_view s) { return QString::fromUtf8(s.data(),static_cast<qsizetype>(s.size())); }
NumericDocumentResult failure(QString message,QString path={}) { return {std::move(message),std::move(path)}; }
NumericDocumentResult source_error(Diagnostic d,QString path={}) { return failure("JSON source error "+QString::number(static_cast<int>(d.code))+" at "+QString::number(d.offset),path); }
NodeView node(const Source& s,Handle h) { NodeView v; (void)s.view(h,v); return v; }
Handle member(const Source& s,Handle h,std::string_view key) { Handle out; (void)find_unique_member(s,h,key,out); return out; }
template<class T> struct Names;
#define NAMES(T,...) template<> struct Names<T>{static constexpr std::string_view values[]={__VA_ARGS__};}
NAMES(DType,"Int8","UInt8","Int16","UInt16","Int32","UInt32","Int64","UInt64","Float32","Float64");
NAMES(Rank,"Scalar","Vector","Matrix");
NAMES(Semantic,"Scalar","PCM","Window","Windowed","SpectrumMagnitude","MelEnergy","History","Numeric");
NAMES(Axis,"None","Sample","Bin","Band","Time","Declared");
NAMES(Quantity,"Unspecified","Amplitude","SpectrumMagnitude","MelEnergy","FilledFrameCount","Packet");
NAMES(NumericUnit,"Unspecified","NormalizedAmplitude","RawAmplitude","Frames","Hertz","Seconds","Values");
NAMES(StorageEncoding,"Native","FixedLittleEndian","FixedBigEndian","PackedSigned24LE","PackedSigned24BE");
NAMES(BitPlacement,"LeastSignificant","MostSignificant");
NAMES(ChannelLayout,"Planar","Interleaved");
NAMES(Normalization,"None","SignedUnit","UnsignedUnit","FloatUnit");
NAMES(CoordinateMode,"None","Regular","ExplicitFloat64LE");
NAMES(Rounding,"TowardZero","NearestTiesEven"); NAMES(Overflow,"Reject","Saturate");
NAMES(PrecisionLoss,"RejectInexact","AllowDeclared"); NAMES(NonFinite,"Reject","ReplaceFinite","ClampFinite");
NAMES(ScaleMode,"Identity","IntegerAffine","DyadicAffine","PcmNormalize","PcmDenormalize","Binary64Affine");
NAMES(NumericReduction,"MaxAbs","SignedMean","RMS"); NAMES(ReductionChannels,"AllChannels","BothChannels","SelectedChannels");
NAMES(ReductionRows,"WholeValue","LatestHistoryRow");
#undef NAMES

struct Mapper;
template<class T> QJsonValue encode(Mapper&,const T&);
template<class T> void decode(Mapper&,Handle,T&);
template<class T> QJsonValue encode(Mapper&,const std::vector<T>&);
template<class T,std::size_t N> QJsonValue encode(Mapper&,const std::array<T,N>&);
template<class T> void decode(Mapper&,Handle,std::vector<T>&);
template<class T,std::size_t N> void decode(Mapper&,Handle,std::array<T,N>&);
struct Mapper {
    const Source* source=nullptr; Handle handle; QJsonObject output;
    NumericDocumentResult* result; QString path; std::string kind;
    bool reading() const { return source!=nullptr; }
    void fail(QString message,QString suffix={}) { if(*result)*result=failure(std::move(message),path+suffix); }
    Handle get(std::string_view key) {
        Handle h; auto d=find_unique_member(*source,handle,key,h);
        if(!d && *result)*result=source_error(d,path+"."+text(key)); return h;
    }
    template<class T> void field(std::string_view name,T& value) {
        if(!*result)return;
        if(reading()) { auto h=get(name); if(*result) { auto child=*this; child.path+="."+text(name); decode(child,h,value); } }
        else output[text(name)]=encode(*this,value);
    }
};
template<class T> QJsonValue encode(Mapper& m,const T& value) {
    if constexpr(std::is_same_v<T,bool>)return value;
    else if constexpr(std::is_same_v<T,std::string>)return text(value);
    else if constexpr(std::is_enum_v<T>) {
        const auto i=static_cast<std::size_t>(value);
        if(i>=std::size(Names<T>::values)){m.fail("Invalid enum");return {};}
        return text(Names<T>::values[i]);
    } else if constexpr(std::is_integral_v<T>) {
        if constexpr(sizeof(T)==8) {
            return QJsonObject{{"dtype",std::is_signed_v<T>?"int64":"uint64"},{"decimal",std::is_signed_v<T>?QString::number(static_cast<qlonglong>(value)):QString::number(static_cast<qulonglong>(value))}};
        } else return static_cast<int>(value); // uint32 handled below without narrowing.
    } else {
        auto copy=value; Mapper child{nullptr,{}, {},m.result,m.path,m.kind}; map(child,copy); return child.output;
    }
}
template<> QJsonValue encode(Mapper&,const std::uint32_t& v) { return QJsonValue(static_cast<qint64>(v)); }
template<> QJsonValue encode(Mapper& m,const NumericLiteral& value) {
    if(validate_literal(value)!=Status::ok){m.fail("Invalid numeric literal");return {};}
    return std::visit([&](const auto& v)->QJsonValue {
        using T=std::decay_t<decltype(v)>;
        if constexpr(std::is_integral_v<T>)return encode(m,v);
        else if constexpr(std::is_same_v<T,float>)return QJsonObject{{"dtype","float32"},{"bits",QString::number(std::bit_cast<std::uint32_t>(v),16).rightJustified(8,'0')}};
        else return QJsonObject{{"dtype","float64"},{"bits",QString::number(std::bit_cast<std::uint64_t>(v),16).rightJustified(16,'0')}};
    },value);
}
template<class T> QJsonValue encode(Mapper& m,const std::vector<T>& value) { QJsonArray a; for(const auto& v:value)a.append(encode(m,v)); return a; }
template<class T,std::size_t N> QJsonValue encode(Mapper& m,const std::array<T,N>& value) { QJsonArray a; for(const auto& v:value)a.append(encode(m,v)); return a; }

void literal(Mapper& m,Handle h,NumericLiteral& out) {
    if(node(*m.source,h).kind!=NodeKind::Object){m.fail("Expected tagged literal");return;}
    auto old=m.handle;m.handle=h;
    constexpr std::string_view known[]={"dtype","decimal","bits"}; auto d=validate_known_keys(*m.source,h,known);
    if(!d){*m.result=source_error(d,m.path);m.handle=old;return;}
    std::string dtype; m.field("dtype",dtype);
    const bool integer=dtype=="int64"||dtype=="uint64";
    if(member(*m.source,h,integer?"bits":"decimal").source){m.fail("Mutually exclusive literal payloads");m.handle=old;return;}
    std::string payload; m.field(integer?"decimal":"bits",payload);
    if(*m.result && integer) {
        const bool signed_type=dtype=="int64";
        if(payload.empty() || payload[0]=='+' || payload=="-0" || (payload[0]=='0' && payload.size()>1)
            || (payload[0]=='-' && (payload.size()==1 || payload[1]=='0')))m.fail("Noncanonical decimal");
        else {
            auto first=payload.data(),last=first+payload.size();
            if(signed_type) {std::int64_t v=0;auto r=std::from_chars(first,last,v);if(r.ec!=std::errc{}||r.ptr!=last)m.fail("Int64 range/grammar");else out=v;}
            else {std::uint64_t v=0;auto r=std::from_chars(first,last,v);if(r.ec!=std::errc{}||r.ptr!=last)m.fail("UInt64 range/grammar");else out=v;}
        }
    } else if(*m.result) {
        const auto width=dtype=="float32"?8u:dtype=="float64"?16u:0u;
        if(!width || payload.size()!=width)m.fail("Float literal tag/width");
        else {
            std::uint64_t v=0;for(char c:payload){if(!((c>='0'&&c<='9')||(c>='a'&&c<='f'))){m.fail("Noncanonical hex");break;}v=v*16u+static_cast<unsigned>(c<='9'?c-'0':c-'a'+10);}
            if(*m.result){if(width==8)out=std::bit_cast<float>(static_cast<std::uint32_t>(v));else out=std::bit_cast<double>(v);}
        }
    }
    m.handle=old;
}
template<class T> void decode(Mapper& m,Handle h,T& value) {
    if(!*m.result)return; const auto v=node(*m.source,h);
    if constexpr(std::is_same_v<T,std::string>) {if(v.kind!=NodeKind::String)m.fail("Expected string");else value.assign(v.decoded);}
    else if constexpr(std::is_same_v<T,bool>) {if(v.kind!=NodeKind::Boolean)m.fail("Expected boolean");else value=v.raw=="true";}
    else if constexpr(std::is_enum_v<T>) {
        std::string name;decode(m,h,name); if(!*m.result)return;
        for(std::size_t i=0;i<std::size(Names<T>::values);++i)if(name==Names<T>::values[i]){value=static_cast<T>(i);return;}
        m.fail("Unknown enum identifier");
    } else if constexpr(std::is_integral_v<T>) {
        if constexpr(sizeof(T)==8) {
            NumericLiteral n;literal(m,h,n);if(!*m.result)return;
            if(const auto* p=std::get_if<T>(&n))value=*p;else m.fail("Integer width/tag mismatch");
        } else {
            std::int64_t n=0;auto d=read_int64(*m.source,h,n);
            if(!d || n<static_cast<std::int64_t>(std::numeric_limits<T>::min()) || n>static_cast<std::int64_t>(std::numeric_limits<T>::max()))m.fail("Bounded integer range/grammar");else value=static_cast<T>(n);
        }
    } else {
        if(v.kind!=NodeKind::Object){m.fail("Expected object");return;}
        Mapper child{m.source,h,{},m.result,m.path,m.kind};map(child,value);
    }
}
template<> void decode(Mapper& m,Handle h,NumericLiteral& value) { literal(m,h,value); }
template<class T> void decode(Mapper& m,Handle h,std::vector<T>& value) {
    auto v=node(*m.source,h);if(v.kind!=NodeKind::Array){m.fail("Expected array");return;}
    std::vector<T> candidate;
    for(auto child=v.first_child;child.source && *m.result;child=node(*m.source,child).next_sibling) {
        if(candidate.size()>=65536){m.fail("Array limit");return;} T item{};decode(m,child,item);candidate.push_back(std::move(item));
    }if(*m.result)value=std::move(candidate);
}
template<class T,std::size_t N> void decode(Mapper& m,Handle h,std::array<T,N>& value) {
    auto v=node(*m.source,h);if(v.kind!=NodeKind::Array){m.fail("Expected array");return;}
    auto child=v.first_child;for(std::size_t i=0;i<N;++i){if(!child.source){m.fail("Array extent");return;}decode(m,child,value[i]);child=node(*m.source,child).next_sibling;}
    if(child.source)m.fail("Array extent");
}
#define FIELD(x) m.field(#x,v.x)
void map(Mapper& m,Ratio& v) {FIELD(numerator);FIELD(denominator);}
void map(Mapper& m,ExtentBounds& v) {FIELD(minimum);FIELD(maximum);}
void map(Mapper& m,ShapeContract& v) {FIELD(rank);FIELD(axes);FIELD(extents);}
void map(Mapper& m,RepresentationContract& v) {FIELD(encoding);FIELD(valid_bits);FIELD(placement);FIELD(layout);FIELD(normalization);FIELD(alignment);}
void map(Mapper& m,AxisCoordinates& v) {FIELD(mode);FIELD(unit);FIELD(origin);FIELD(step);}
void map(Mapper& m,Dyadic& v) {FIELD(numerator);FIELD(exponent2);}
void map(Mapper& m,Scaling& v) {FIELD(mode);FIELD(gain);FIELD(offset);}
void map(Mapper& m,ConversionPolicy& v) {FIELD(rounding);FIELD(overflow);FIELD(precision_loss);FIELD(nonfinite);FIELD(scaling);FIELD(nan_replacement);FIELD(negative_inf_replacement);FIELD(positive_inf_replacement);}
void numeric_contract(Mapper& m,NumericContract& v,std::vector<std::uint32_t>& channels,std::string& clock) {
    FIELD(dtype);FIELD(shape);FIELD(semantic);FIELD(history_element);FIELD(quantity);FIELD(unit);FIELD(representation);FIELD(coordinates);
    m.field("channel_order",channels);m.field("clock_domain",clock);m.field("tick_seconds",v.clock.tick_seconds);m.field("sample_rate",v.clock.sample_rate);
    // Decode borrows these caller-owned staging fields until they are retained.
    // Encode must keep the original owned views: its staging locals expire
    // before the final contract validation in encode(OwnedProcessingContract).
    if(m.reading()){v.channel_order=channels;v.clock.domain=clock;}
}
template<> QJsonValue encode(Mapper& parent,const OwnedProcessingContract& owned) {
    Mapper m{nullptr,{}, {},parent.result,parent.path,parent.kind};
    if(!owned){m.fail("Missing owned contract");return {};}
    auto data=owned.view();std::string type=std::holds_alternative<NumericContract>(data)?"numeric":"structured";m.field("type",type);
    if(auto* v=std::get_if<NumericContract>(&data)){std::vector<std::uint32_t> channels(v->channel_order.begin(),v->channel_order.end());std::string clock(v->clock.domain);numeric_contract(m,*v,channels,clock);}
    else {auto& packet=std::get<StructuredContract>(data);std::string schema(packet.schema),encoding(packet.encoding);m.field("schema",schema);m.field("version",packet.version);m.field("encoding",encoding);m.field("payload_bytes",packet.payload_bytes);}
    if(validate_contract(data,frame_limits)!=Status::ok)m.fail("Invalid contract");return m.output;
}
template<> void decode(Mapper& parent,Handle h,OwnedProcessingContract& owned) {
    Mapper m{parent.source,h,{},parent.result,parent.path,parent.kind};std::string type;m.field("type",type);
    DataContract data;std::vector<std::uint32_t> channels;std::string clock,schema,encoding;
    if(type=="numeric"){NumericContract v;numeric_contract(m,v,channels,clock);data=v;}
    else if(type=="structured"){StructuredContract v;m.field("schema",schema);m.field("version",v.version);m.field("encoding",encoding);m.field("payload_bytes",v.payload_bytes);v.schema=schema;v.encoding=encoding;data=v;}
    else m.fail("Unknown contract category");
    if(*m.result && own_processing_contract(data,frame_limits,16777216,owned)!=Status::ok)m.fail("Invalid/bounded contract");
}
void map(Mapper& m,ProcessingPort& v) {FIELD(id);FIELD(contract);} // Direction comes from the containing field.
template<> QJsonValue encode(Mapper& m,const ProcessingParameterValue& v) {return std::visit([&](const auto& x){return encode(m,x);},v);}
template<> void decode(Mapper& m,Handle h,ProcessingParameterValue& v) {
    const auto k=node(*m.source,h).kind;
    if(k==NodeKind::Boolean){bool x=false;decode(m,h,x);v=x;}
    else if(k==NodeKind::String){std::string x;decode(m,h,x);v=std::move(x);}
    else {NumericLiteral x;decode(m,h,x);v=x;}
}
void map(Mapper& m,ProcessingParameter& v) {FIELD(key);FIELD(value);}
bool registered(std::string_view kind,std::string_view key) {
    auto k=node_kind(kind);if(!k)return false;auto* d=parameter_descriptor(*k,key);
    return d && d->classification==ParameterClassification::Processing && key!="enabled";
}
void parameter_values(Mapper& m,std::vector<ProcessingParameter>& values) {
    if(!m.reading()){
        QSet<QString> seen;
        for(const auto& v:values){if(!registered(m.kind,v.key) || seen.contains(text(v.key))){m.fail("Unregistered/duplicate typed parameter", ".values");return;}seen.insert(text(v.key));}
        m.field("values",values);return;
    }
    auto array=m.get("values");auto v=node(*m.source,array);if(v.kind!=NodeKind::Array){m.fail("Expected parameter array");return;}
    QSet<QString> seen;
    for(auto h=v.first_child;h.source && *m.result;h=node(*m.source,h).next_sibling){
        Mapper child{m.source,h,{},m.result,m.path+".values",m.kind};std::string key;child.field("key",key);
        if(!*m.result)return;if(seen.contains(text(key))){m.fail("Duplicate parameter key");return;}seen.insert(text(key));
        if(!registered(m.kind,key))continue;ProcessingParameter p;decode(child,h,p);values.push_back(std::move(p));
    }
}
void parameters(Mapper& m,ProcessingParameters& value) {
    std::string type;
    if(!m.reading())type=std::holds_alternative<EndpointProcessingParameters>(value)?"endpoint":std::holds_alternative<TransformProcessingParameters>(value)?"transform":std::holds_alternative<ConverterProcessingParameters>(value)?"converter":std::holds_alternative<RouteProcessingParameters>(value)?"route":std::holds_alternative<ReduceProcessingParameters>(value)?"reduce":"history";
    m.field("type",type);
    if(type=="endpoint"||type=="transform") {
        std::vector<ProcessingParameter> v;if(!m.reading())v=type=="endpoint"?std::get<EndpointProcessingParameters>(value).values:std::get<TransformProcessingParameters>(value).values;
        parameter_values(m,v);if(type=="endpoint")value=EndpointProcessingParameters{std::move(v)};else value=TransformProcessingParameters{std::move(v)};
    }else if(type=="converter"){auto v=m.reading()?ConverterProcessingParameters{}:std::get<ConverterProcessingParameters>(value);m.field("policy",v.policy);value=v;}
    else if(type=="route"){auto v=m.reading()?RouteProcessingParameters{}:std::get<RouteProcessingParameters>(value);m.field("outputs",v.outputs);m.field("selected",v.selected);m.field("open",v.open);value=v;}
    else if(type=="reduce"){auto v=m.reading()?ReduceProcessingParameters{}:std::get<ReduceProcessingParameters>(value);m.field("operation",v.operation);m.field("channels",v.channels);m.field("rows",v.rows);m.field("selected_channels",v.selected_channels);m.field("precision_loss",v.precision_loss);m.field("rounding",v.rounding);value=std::move(v);}
    else if(type=="history"){auto v=m.reading()?HistoryProcessingParameters{}:std::get<HistoryProcessingParameters>(value);m.field("capacity",v.capacity);value=v;}
    else m.fail("Unknown parameter category");
}
template<> QJsonValue encode(Mapper& parent,const ProcessingParameters& v) {auto copy=v;Mapper m{nullptr,{}, {},parent.result,parent.path,parent.kind};parameters(m,copy);return m.output;}
template<> void decode(Mapper& parent,Handle h,ProcessingParameters& v) {Mapper m{parent.source,h,{},parent.result,parent.path,parent.kind};parameters(m,v);}
void map(Mapper& m,ConfiguredProcessingNode& v) {
    if(!m.reading() && (v.inputs.size()>64 || v.outputs.size()>64 || v.kind.size()>1024)){m.fail("Node limits");return;}
    FIELD(id);FIELD(kind);FIELD(enabled);m.kind=v.kind;
    if(v.id<0 || (v.kind!="Converter" && !node_kind(v.kind))){m.fail("Invalid node identity/kind");return;}
    FIELD(parameters);FIELD(inputs);FIELD(outputs);
    for(auto& p:v.inputs)p.direction=PortDirection::Input;for(auto& p:v.outputs)p.direction=PortDirection::Output;
    for(const auto* list:{&v.inputs,&v.outputs}){QSet<int> ids;for(const auto& p:*list){if(ids.contains(p.id)){m.fail("Duplicate port ID");return;}ids.insert(p.id);}}
}
void map(Mapper& m,Edge& v) {m.field("out_id",v.from);m.field("out_index",v.output_port);m.field("in_id",v.to);m.field("in_index",v.input_port);
    if(!m.reading() || member(*m.source,m.handle,"conversion").source)m.field("conversion",v.conversion);
    if(v.conversion>3)m.fail("Invalid edge conversion policy");
}
void map(Mapper& m,NumericProcessingGraph& v) {
    if(!m.reading() && (v.nodes.size()>4096 || v.edges.size()>65536 || v.graph_id.size()>1024)){m.fail("Graph limits");return;}
    m.field("version",v.versions.graph);m.field("processing_version",v.versions.processing);
    if(v.versions.graph!=3 || v.versions.processing!=2){m.fail("Unsupported graph/processing version");return;}
    FIELD(graph_id);
    if(!m.reading() || member(*m.source,m.handle,"execution_interval").source)FIELD(execution_interval);
    if(!domain::valid_execution_interval(v.execution_interval)){m.path+=".execution_interval";m.fail("Invalid execution interval");return;}
    FIELD(nodes);m.field("connections",v.edges);
    if(v.nodes.size()>4096 || v.edges.size()>65536 || v.graph_id.empty()){m.fail("Graph bounds/identity");return;}
    QSet<int> ids;for(const auto& n:v.nodes){if(ids.contains(n.id)){m.fail("Duplicate node ID");return;}ids.insert(n.id);}
    QSet<QString> edges;for(const auto& e:v.edges){auto key=QString::number(e.from)+":"+QString::number(e.output_port)+":"+QString::number(e.to)+":"+QString::number(e.input_port);if(edges.contains(key)){m.fail("Duplicate edge identity");return;}edges.insert(key);}
}
#undef FIELD
template<class T> NumericDocumentResult write_snapshot(const T& v,QByteArray& out) {
    NumericDocumentResult r;Mapper m{nullptr,{}, {},&r,"snapshot",{}};auto value=encode(m,v);
    if(!r)return r;auto candidate=QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact);
    if(candidate.size()>16777216)return failure("Snapshot limit");out=std::move(candidate);return {};
}
template<class T> NumericDocumentResult read_snapshot(QByteArrayView bytes,T& out) {
    Source s;auto d=parse_project_source(view(bytes),s);if(!d)return source_error(d);
    NumericDocumentResult r;Mapper m{&s,s.root(),{},&r,"snapshot",{}};T candidate;decode(m,s.root(),candidate);if(r)out=std::move(candidate);return r;
}
NumericDocumentResult object_keys(const Source& s,Handle h,std::initializer_list<std::string_view> keys,QString path) {
    if(node(s,h).kind!=NodeKind::Object)return failure("Expected object",path);
    auto d=validate_known_keys(s,h,{keys.begin(),keys.size()});return d?NumericDocumentResult{}:source_error(d,path);
}
NumericDocumentResult edition(const Source& s,Handle h,std::string_view key,std::uint32_t& out,QString path,bool optional=false) {
    Handle field;auto d=find_unique_member(s,h,key,field);
    if(optional && d.code==Error::MissingMember)return {};
    if(!d)return source_error(d,path+"."+text(key));
    std::uint64_t value=0;d=read_uint64(s,field,value);
    if(!d || value==0 || value>std::numeric_limits<std::uint32_t>::max())return failure("Invalid version",path+"."+text(key));
    out=static_cast<std::uint32_t>(value);return {};
}
NumericDocumentResult check_visual_graph(const Source&,Handle,NumericProcessingGraph* out=nullptr);
NumericDocumentResult check_node_adapter(const Source& s,Handle h) {
    auto result=object_keys(s,h,{"provider_uuid","stream_id","host","port","device_identity","remote_endpoint"},"node.adapter");if(!result)return result;
    for(auto key:{"provider_uuid","stream_id","host","device_identity"}){
        const auto field=member(s,h,key);if(!field.source)continue;const auto value=node(s,field);
        if(value.kind!=NodeKind::String||value.decoded.empty()||value.decoded.size()>256)return failure("Invalid adapter string","node.adapter."+QString(key));
        if(std::string_view(key)=="provider_uuid" && QUuid(text(value.decoded)).isNull())return failure("Invalid provider UUID","node.adapter.provider_uuid");
    }
    auto port=member(s,h,"port");if(port.source){std::uint64_t value=0;auto d=read_uint64(s,port,value);if(!d||value<1||value>65535)return failure("Invalid adapter port","node.adapter.port");}
    const auto endpoint=member(s,h,"remote_endpoint");if(endpoint.source){
        result=object_keys(s,endpoint,{"graph_id","node_id","direction","port_index"},"node.adapter.remote_endpoint");if(!result)return result;
        auto graph_id=member(s,endpoint,"graph_id");if(graph_id.source){const auto value=node(s,graph_id);if(value.kind!=NodeKind::String||value.decoded.empty()||value.decoded.size()>256)return failure("Invalid endpoint graph ID","node.adapter.remote_endpoint.graph_id");}
        auto direction=member(s,endpoint,"direction");if(direction.source){const auto value=node(s,direction);if(value.kind!=NodeKind::String||(value.decoded!="input"&&value.decoded!="output"))return failure("Invalid endpoint direction","node.adapter.remote_endpoint.direction");}
        for(auto key:{"node_id","port_index"}){const auto field=member(s,endpoint,key);if(!field.source)continue;std::uint64_t value=0;auto d=read_uint64(s,field,value);const auto maximum=std::string_view(key)=="node_id"?static_cast<std::uint64_t>(std::numeric_limits<NodeId>::max()):65535u;if(!d||value>maximum)return failure("Invalid endpoint integer","node.adapter.remote_endpoint."+QString(key));}
    }
    return {};
}
NumericDocumentResult check_node_presentation(const Source& s,Handle h,QJsonObject* out=nullptr) {
    auto result=object_keys(s,h,{"inline_parameters","input_port_order","output_port_order","display_name","scalar_inputs"},"node.presentation");if(!result)return result;
    const auto inline_parameters=member(s,h,"inline_parameters");QJsonObject candidate;
    const auto scalar_inputs=member(s,h,"scalar_inputs");
    if(scalar_inputs.source){
        if(node(s,scalar_inputs).kind!=NodeKind::Object)return failure("Expected scalar input settings object");
        QJsonObject inputs;unsigned count=0;
        for(auto entry=node(s,scalar_inputs).first_child;entry.source;entry=node(s,entry).next_sibling){
            const auto key=text(node(s,entry).key);bool valid=false;const auto id=key.toUInt(&valid);
            if(++count>8 || !valid || id>65535 || QString::number(id)!=key)return failure("Invalid scalar input ID");
            QJsonObject input;auto r=check_node_presentation(s,entry,&input);if(!r)return r;
            const auto enabled=member(s,entry,"enabled");
            if(enabled.source){const auto value=node(s,enabled);if(value.kind!=NodeKind::Boolean)return failure("Expected scalar input enabled boolean");input["enabled"]=value.raw=="true";}
            inputs[key]=input;
        }
        candidate["scalar_inputs"]=inputs;
    }
    const auto display_name=member(s,h,"display_name");
    if(display_name.source){
        const auto value=node(s,display_name);
        if(value.kind!=NodeKind::String)return failure("Expected display name string","node.presentation.display_name");
        const auto name=text(value.decoded);
        if(name.size()>256)return failure("Display name limit","node.presentation.display_name");
        for(const auto character:name)if(character.category()==QChar::Other_Control)return failure("Invalid display name","node.presentation.display_name");
        candidate["display_name"]=name;
    }
    if(inline_parameters.source){
        if(node(s,inline_parameters).kind!=NodeKind::Array)return failure("Expected inline parameter array","node.presentation.inline_parameters");
        QJsonArray keys;QSet<QString> seen;
        for(auto key=node(s,inline_parameters).first_child;key.source;key=node(s,key).next_sibling){
            const auto value=node(s,key);
            if(keys.size()>=128||value.kind!=NodeKind::String||value.decoded.empty()||value.decoded.size()>1024)return failure("Invalid/bounded inline parameter key","node.presentation.inline_parameters");
            const auto name=text(value.decoded);if(seen.contains(name))return failure("Duplicate inline parameter key","node.presentation.inline_parameters");
            seen.insert(name);keys.append(name);
        }
        candidate["inline_parameters"]=keys;
    }
    for(auto name:{"input_port_order","output_port_order"}){const auto array=member(s,h,name);if(!array.source)continue;
        if(node(s,array).kind!=NodeKind::Array)return failure("Expected port order array");QJsonArray ids;QSet<int> seen;
        for(auto item=node(s,array).first_child;item.source;item=node(s,item).next_sibling){std::uint64_t id=0;if(ids.size()>=8 || !read_uint64(s,item,id) || id>65535 || seen.contains(int(id)))return failure("Invalid port order");seen.insert(int(id));ids.append(int(id));}candidate[name]=ids;
    }
    if(out)*out=std::move(candidate);return {};
}
NumericDocumentResult check_graph(const Source& s,Handle h,QString path,std::uint32_t schema,bool allow_visual=false) {
    auto r=object_keys(s,h,{"version","processing_version","graph_id","execution_interval","nodes","connections"},path);if(!r)return r;
    std::uint32_t graph=0,processing=0;r=edition(s,h,"version",graph,path);if(!r)return r;
    r=edition(s,h,"processing_version",processing,path);if(!r)return r;
    if(graph!=3 || processing!=2 || schema!=2)return failure("Unsupported graph/processing version combination",path);
    if(allow_visual){bool canonical=false,visual=false;auto nodes=member(s,h,"nodes");
        for(auto n=node(s,nodes).first_child;n.source;n=node(s,n).next_sibling){if(member(s,n,"kind").source)canonical=true;else if(member(s,n,"internal-data").source)visual=true;else return failure("Unknown preset node representation",path);}
        if(canonical&&visual)return failure("Mixed preset graph representations",path);
        if(visual)return check_visual_graph(s,h);
    }
    NumericProcessingGraph known;r=read_snapshot(bytes(node(s,h).raw),known);if(!r)return r;
    for(auto n=node(s,member(s,h,"nodes")).first_child;n.source;n=node(s,n).next_sibling){
        Handle presentation;auto d=find_unique_member(s,n,"presentation",presentation);if(d.code==Error::AmbiguousKey)return source_error(d,path+".nodes.presentation");
        if(presentation.source){r=check_node_presentation(s,presentation);if(!r)return r;}
        Handle adapter;d=find_unique_member(s,n,"adapter",adapter);if(d.code==Error::AmbiguousKey)return source_error(d,path+".nodes.adapter");
        if(adapter.source){r=check_node_adapter(s,adapter);if(!r)return r;}
    }
    return {};
}
bool registered_display_member(std::string_view context,std::string_view key);
NumericDocumentResult check_preset_presentation(const Source& s,Handle h,QString path) {
    auto result=object_keys(s,h,{"version","graph_views","graph_rendering","workspace","show_raw_samples","show_amplitudes","show_test_amplitudes","show_spectrogram","show_window_signal","show_windowed_signal","show_window_function","show_data_flow"},path);if(!result)return result;
    std::uint32_t version=0;result=edition(s,h,"version",version,path);if(!result)return result;
    if(version!=1)return failure("Unsupported preset presentation version",path);
    for(auto key:{"show_raw_samples","show_amplitudes","show_test_amplitudes","show_spectrogram","show_window_signal","show_windowed_signal","show_window_function","show_data_flow"}){
        const auto field=member(s,h,key);if(field.source && node(s,field).kind!=NodeKind::Boolean)return failure("Expected display visibility boolean",path+"."+key);
    }
    for(auto key:{"graph_views","graph_rendering","workspace"}){
        const auto field=member(s,h,key);if(!field.source)continue;
        if(node(s,field).kind!=NodeKind::Object)return failure("Expected preset display object",path+"."+key);
        QSet<QString> names;for(auto entry=node(s,field).first_child;entry.source;entry=node(s,entry).next_sibling){const auto name=text(node(s,entry).key);if(names.contains(name))return failure("Ambiguous display entry",path+"."+key);names.insert(name);}
        if(std::string_view(key)=="graph_views")for(auto entry=node(s,field).first_child;entry.source;entry=node(s,entry).next_sibling){
            if(!registered_display_member("graph_views",node(s,entry).key))continue;
            result=object_keys(s,entry,{"open"},path+".graph_views."+text(node(s,entry).key));if(!result)return result;
            auto open=member(s,entry,"open");if(open.source&&node(s,open).kind!=NodeKind::Boolean)return failure("Expected graph visibility boolean",path+".graph_views.open");
        }
        if(std::string_view(key)=="workspace"){
            result=object_keys(s,field,{"version","layout_version","backend","geometry","ads_state","visibility"},path+".workspace");if(!result)return result;
            auto edition_field=member(s,field,"version");if(edition_field.source){std::uint32_t edition_value=0;result=edition(s,field,"version",edition_value,path+".workspace");if(!result)return result;if(edition_value!=1)return failure("Unsupported workspace version",path+".workspace");}
            auto layout=member(s,field,"layout_version");if(layout.source){std::uint64_t value=0;auto d=read_uint64(s,layout,value);if(!d||value>std::numeric_limits<int>::max())return failure("Invalid workspace layout version",path+".workspace.layout_version");}
            for(auto name:{"backend","geometry","ads_state"}){auto value=member(s,field,name);if(value.source&&node(s,value).kind!=NodeKind::String)return failure("Expected workspace string",path+".workspace."+name);}
            auto visibility=member(s,field,"visibility");if(visibility.source&&node(s,visibility).kind!=NodeKind::Object)return failure("Expected workspace visibility object",path+".workspace.visibility");
            QSet<QString> seen;for(auto entry=visibility.source?node(s,visibility).first_child:Handle{};entry.source;entry=node(s,entry).next_sibling){
                const auto value=node(s,entry);const auto name=text(value.key);const bool known=value.key=="dataFlow"||registered_display_member("graph_views",value.key)||(value.key.starts_with("nodeView_")&&registered_display_member("graph_views",value.key.substr(9)));
                if(!known)continue;if(seen.contains(name)||value.kind!=NodeKind::Boolean)return failure("Invalid workspace visibility boolean",path+".workspace.visibility."+name);seen.insert(name);
            }
        }
    }
    return {};
}
NumericDocumentResult check_presets(const Source& s,Handle h,QString path,std::uint32_t schema) {
    if(node(s,h).kind!=NodeKind::Array)return failure("Expected preset array",path);
    QSet<QString> ids;
    for(auto p=node(s,h).first_child;p.source;p=node(s,p).next_sibling){
        auto r=object_keys(s,p,{"version","id","name","graph","view","presentation"},path);if(!r)return r;
        std::uint32_t version=0;r=edition(s,p,"version",version,path);if(!r || version!=1)return failure("Unsupported preset version",path);
        auto id=member(s,p,"id");auto v=node(s,id);if(!id.source||v.kind!=NodeKind::String||v.decoded.empty()||ids.contains(text(v.decoded)))return failure("Invalid/ambiguous preset ID",path);ids.insert(text(v.decoded));
        auto graph=member(s,p,"graph");if(graph.source){r=check_graph(s,graph,path+".graph",schema,true);if(!r)return r;}
        auto presentation=member(s,p,"presentation");if(presentation.source){r=check_preset_presentation(s,presentation,path+".presentation");if(!r)return r;}
    }return {};
}
NumericDocumentResult check_project(const Source& s,std::uint32_t& schema) {
    auto root=s.root();auto r=object_keys(s,root,{"schema_version","pipeline_graph","scene_graph","ui","presets","devices"},"project");if(!r)return r;
    schema=0;r=edition(s,root,"schema_version",schema,"project");if(!r)return r;
    if(schema!=2)return failure("Unsupported document version","schema_version");
    auto scan=[&](Handle owner,QString path)->NumericDocumentResult {
        auto known=object_keys(s,owner,{"pipeline_graph","scene_graph","presets"},path);if(!known)return known;
        for(auto key:{"pipeline_graph","scene_graph"}){auto g=member(s,owner,key);if(g.source){auto e=check_graph(s,g,path+"."+key,schema);if(!e)return e;}}
        auto presets=member(s,owner,"presets");if(presets.source)return check_presets(s,presets,path+".presets",schema);return {};
    };
    r=scan(root,"project");if(!r)return r;
    Handle standalone_nodes;auto standalone_lookup=find_unique_member(s,root,"nodes",standalone_nodes);
    if(standalone_lookup.code==Error::AmbiguousKey)return source_error(standalone_lookup,"nodes");
    if(standalone_nodes.source)return failure("Standalone graph is not a numeric project","nodes");
    auto ui=member(s,root,"ui");if(ui.source){r=object_keys(s,ui,{"pipeline_presets"},"ui");if(!r)return r;auto p=member(s,ui,"pipeline_presets");if(p.source){r=check_presets(s,p,"ui.pipeline_presets",schema);if(!r)return r;}}
    auto devices=member(s,root,"devices");if(devices.source){auto v=node(s,devices);if(v.kind!=NodeKind::Object && v.kind!=NodeKind::Array)return failure("Expected devices","devices");for(auto d=v.first_child;d.source;d=node(s,d).next_sibling){r=scan(d,"devices");if(!r)return r;}}
    return {};
}
QJsonValue snapshot_leaf(const Source& s,Handle h,NumericDocumentResult& result) {
    auto raw=node(s,h);if(raw.kind==NodeKind::Object || raw.kind==NodeKind::Array){result=failure("Expected scalar");return {};}
    QJsonParseError error;auto value=QJsonDocument::fromJson("["+bytes(raw.raw)+"]",&error);
    if(error.error!=QJsonParseError::NoError){result=failure("Invalid known value");return {};}
    return value.array().first(); // Only a schema-known leaf, never opaque source.
}
// Merge a typed known snapshot into its local source subtree. New fields define
// the registered write set; old unrelated members are never projected through Qt.
// Arrays with identity use IDs/keys/endpoints, never a reordered array index.
QString identity(const Source& s,Handle h,std::string_view array) {
    auto scalar=[&](std::string_view key){auto v=node(s,member(s,h,key));return v.kind==NodeKind::String?text(v.decoded):text(v.raw);};
    if(array=="nodes"){
        auto kind=scalar("kind");
        if(kind.isEmpty()){auto internal=member(s,h,"internal-data");auto model=node(s,member(s,internal,"model-name"));kind=text(model.decoded);}
        return scalar("id")+"/"+kind;
    }
    if(array=="values")return scalar("key");
    if(array=="inputs"||array=="outputs")return scalar("id");
    if(array=="settings_ids")return scalar("id");
    if(array=="pipeline_presets"||array=="presets"){
        return "id:"+scalar("id");
    }
    if(array=="connections"){
        if(member(s,h,"outNodeId").source)return scalar("outNodeId")+":"+scalar("outPortIndex")+":"+scalar("inNodeId")+":"+scalar("inPortIndex");
        return scalar("out_id")+":"+scalar("out_index")+":"+scalar("in_id")+":"+scalar("in_index");
    }
    return {};
}
QByteArray member_prefix(const Source& s,Handle parent,Handle value) {
    auto begin=node(s,parent).offset+1;
    for(auto h=node(s,parent).first_child;h.source && h!=value;h=node(s,h).next_sibling){const auto v=node(s,h);begin=v.offset+v.raw.size();}
    const auto end=node(s,value).offset;auto raw=s.bytes();
    while(begin<end && (raw[begin]==' '||raw[begin]=='\n'||raw[begin]=='\r'||raw[begin]=='\t'||raw[begin]==','))++begin;
    return bytes(raw.substr(begin,end-begin));
}
QJsonValue settings_projection(const Source&,Handle,bool&,bool preset_context=false);
bool registered_display_member(std::string_view context,std::string_view key) {
    if(context=="adapter")return key=="provider_uuid"||key=="stream_id"||key=="host"||key=="port"||key=="device_identity"||key=="remote_endpoint";
    if(context=="remote_endpoint")return key=="graph_id"||key=="node_id"||key=="direction"||key=="port_index";
    if(context=="presentation")return key=="version"||key=="graph_views"||key=="graph_rendering"||key=="workspace"||key=="show_raw_samples"||key=="show_amplitudes"||key=="show_test_amplitudes"||key=="show_spectrogram"||key=="show_window_signal"||key=="show_windowed_signal"||key=="show_window_function"||key=="show_data_flow";
    if(context=="graph_rendering")return key=="version"||key=="defaults"||key=="color_presets"||key=="gui_fps_mode"||key=="gui_target_fps";
    if(context=="workspace")return key=="version"||key=="layout_version"||key=="backend"||key=="geometry"||key=="ads_state"||key=="visibility";
    if(context!="graph_views")return false;
    for(auto pane:{"rawSamples","fft","mel","spectrogram","windowSignal","windowedSignal","windowFunction"})if(key==pane)return true;
    // Node-view identity is the existing hex-encoded [device,graph,node,instance?].
    // Other dictionary keys remain opaque, including future extension records.
    if(key.size()>1024)return false;const auto encoded=bytes(key),decoded=QByteArray::fromHex(encoded);if(decoded.toHex()!=encoded)return false;
    const auto identity=QJsonDocument::fromJson(decoded).array();
    const auto integer=[](const QJsonValue& value,qint64 maximum){return value.isDouble()&&value.toInteger(-1)>=0&&value.toInteger(-1)<=maximum&&value.toDouble()==static_cast<double>(value.toInteger(-1));};
    return (identity.size()==3||identity.size()==4)&&identity[0].isString()&&identity[1].isString()&&integer(identity[2],std::numeric_limits<NodeId>::max())&&(identity.size()==3||integer(identity[3],std::numeric_limits<qint64>::max()));
}
QByteArray merge(const Source& old,Handle old_h,const Source& fresh,Handle new_h,
                 std::string_view context,NumericDocumentResult& result,std::string kind={},bool preserve_equal=false,std::string_view display_scope={}) {
    const auto a=node(old,old_h),b=node(fresh,new_h);
    if(preserve_equal){bool complete=true;const auto projected=settings_projection(old,old_h,complete,true);bool fresh_complete=true;const auto replacement=settings_projection(fresh,new_h,fresh_complete,true);if(complete&&fresh_complete&&projected==replacement)return bytes(a.raw);}
    if(a.kind!=b.kind)return bytes(b.raw);
    if(preserve_equal && context=="numeric_processing" && b.kind==NodeKind::String){
        Source original,replacement;auto d=parse_project_source(a.decoded,original);if(!d){result=source_error(d,"preset.numeric_processing");return {};}
        d=parse_project_source(b.decoded,replacement);if(!d){result=source_error(d,"preset.numeric_processing");return {};}
        auto retained=merge(original,original.root(),replacement,replacement.root(),"node",result,{},true);if(!result)return {};
        auto quoted=QJsonDocument(QJsonArray{QString::fromUtf8(retained)}).toJson(QJsonDocument::Compact);return quoted.mid(1,quoted.size()-2);
    }
    if(b.kind==NodeKind::Object) {
        if(context=="nodes"){auto k=node(fresh,member(fresh,new_h,"kind"));kind.assign(k.decoded);}
        QByteArray out="{";bool first=true;QSet<QString> processed;
        auto append=[&](QByteArray member_bytes){if(!first)out+=',';first=false;out+=member_bytes;};
        for(auto h=a.first_child;h.source && result;h=node(old,h).next_sibling){
            auto v=node(old,h);Handle replacement;auto d=find_unique_member(fresh,new_h,v.key,replacement);
            if(d.code==Error::AmbiguousKey){result=source_error(d);break;}
            if(replacement.source){
                Handle unique;d=find_unique_member(old,old_h,v.key,unique);if(!d){result=source_error(d);break;}
                processed.insert(text(v.key));
                std::string_view display;
                if((context=="pipeline_presets"||context=="presets")&&v.key=="presentation")display="presentation";
                else if((context=="nodes"||context=="internal-data")&&v.key=="adapter")display="adapter";
                else if(display_scope=="presentation"&&(v.key=="graph_views"||v.key=="graph_rendering"||v.key=="workspace"))display=v.key;
                else if(display_scope=="adapter"&&v.key=="remote_endpoint")display=v.key;
                append(member_prefix(old,old_h,h)+merge(old,h,fresh,replacement,v.key,result,kind,preserve_equal,display));
            }else {
                if(context=="scalar_inputs")continue; // Full map removes retired port IDs; surviving entries merge opaque metadata.
                if(display_scope=="adapter"&&v.key=="remote_endpoint"){
                    QByteArray opaque="{";bool any=false;
                    for(auto child=v.first_child;child.source;child=node(old,child).next_sibling){const auto value=node(old,child);if(registered_display_member("remote_endpoint",value.key))continue;if(any)opaque+=',';any=true;opaque+=member_prefix(old,h,child)+bytes(value.raw);}
                    if(any){opaque+='}';append(member_prefix(old,old_h,h)+opaque);}continue;
                }
                if(!display_scope.empty()&&registered_display_member(display_scope,v.key))continue;
                // The other literal payload is a known member, never an opaque extension.
                if((context=="numerator"||context=="nan_replacement"||context=="negative_inf_replacement"||context=="positive_inf_replacement"||context=="value"||context=="capacity"||context=="minimum"||context=="maximum"||context=="denominator"||context=="alignment") && (v.key=="bits"||v.key=="decimal"))continue;
                append(member_prefix(old,old_h,h)+bytes(v.raw));
            }
        }
        for(auto h=b.first_child;h.source && result;h=node(fresh,h).next_sibling){auto v=node(fresh,h);if(processed.contains(text(v.key)))continue;auto k=QJsonDocument(QJsonArray{text(v.key)}).toJson(QJsonDocument::Compact);k=k.mid(1,k.size()-2);append(k+":"+bytes(v.raw));}
        out+='}';return out;
    }
    if(b.kind==NodeKind::Array) {
        const bool identified=context=="nodes"||context=="values"||context=="inputs"||context=="outputs"||context=="connections"||context=="pipeline_presets"||context=="presets"||context=="settings_ids";
        QByteArray out="[";bool first=true;QSet<QString> retained;
        auto old_child=a.first_child;
        for(auto child=b.first_child;child.source && result;child=node(fresh,child).next_sibling){
            Handle match;auto id=identity(fresh,child,context);
            if(identified){if(retained.contains(id)){result=failure("Duplicate replacement identity",text(context));break;}for(auto h=a.first_child;h.source;h=node(old,h).next_sibling)if(identity(old,h,context)==id){if(match.source){result=failure("Ambiguous source identity",text(context));break;}match=h;}retained.insert(id);}
            else {match=old_child;if(old_child.source)old_child=node(old,old_child).next_sibling;}
            if(!first)out+=',';first=false;out+=match.source?merge(old,match,fresh,child,context,result,kind,preserve_equal,display_scope):bytes(node(fresh,child).raw);
        }
        // Opaque unregistered parameter records remain local, including huge lexemes.
        if(context=="values")for(auto h=a.first_child;h.source && result;h=node(old,h).next_sibling){auto key=node(old,member(old,h,"key"));auto id=text(key.decoded);if(!retained.contains(id) && !registered(kind,key.decoded)){if(!first)out+=',';first=false;out+=bytes(node(old,h).raw);}}
        out+=']';return out;
    }
    return a.raw==b.raw?bytes(a.raw):bytes(b.raw);
}
} // namespace
QString numeric_project_path(const QString& explicit_path) {
    return QFileInfo(explicit_path.isEmpty()?QDir(QCoreApplication::applicationDirPath()).filePath("config.json"):explicit_path).absoluteFilePath();
}
NumericDocumentResult encode_numeric_node(const ConfiguredProcessingNode& v,QByteArray& out) {return write_snapshot(v,out);}
NumericDocumentResult decode_numeric_node(QByteArrayView bytes,ConfiguredProcessingNode& out) {return read_snapshot(bytes,out);}
NumericDocumentResult encode_numeric_graph(const NumericProcessingGraph& v,QByteArray& out) {return write_snapshot(v,out);}
NumericDocumentResult decode_numeric_graph(QByteArrayView bytes,NumericProcessingGraph& out) {return read_snapshot(bytes,out);}
NumericDocumentResult decode_numeric_editor_graph(QByteArrayView input,NumericProcessingGraph& out) {
    Source source;auto diagnostic=parse_project_source(view(input),source);if(!diagnostic)return source_error(diagnostic);
    const auto graph=source.root();auto result=check_graph(source,graph,"snapshot",2,true);if(!result)return result;
    const auto first=node(source,member(source,graph,"nodes")).first_child;
    if(first.source && !member(source,first,"kind").source && member(source,first,"internal-data").source)return check_visual_graph(source,graph,&out);
    return read_snapshot(input,out);
}
NumericDocumentResult read_numeric_project(QByteArrayView input,NumericProjectDocument& out) {
    NumericProjectDocument candidate;auto d=parse_project_source(view(input),candidate.source);if(!d)return source_error(d);
    std::uint32_t schema=0;auto r=check_project(candidate.source,schema);if(!r)return r;out=std::move(candidate);return {};
}
NumericDocumentResult clone_numeric_project(const NumericProjectDocument& source,NumericProjectDocument& out) {
    NumericProjectDocument candidate;auto r=read_numeric_project(bytes(source.source.bytes()),candidate);if(!r)return r;
    candidate.active_path=source.active_path;candidate.disk_baseline=source.disk_baseline;out=std::move(candidate);return {};
}
NumericDocumentResult read_numeric_draft(const NumericProjectDocument& source,NumericProcessingGraph& out) {
    std::uint32_t schema=0;auto r=check_project(source.source,schema);if(!r)return r;
    auto graph=member(source.source,source.source.root(),"scene_graph");if(!graph.source)return failure("No numeric draft","scene_graph");
    return decode_numeric_graph(bytes(node(source.source,graph).raw),out);
}
NumericDocumentResult read_numeric_applied(const NumericProjectDocument& source,std::optional<NumericProcessingGraph>& out) {
    std::uint32_t schema=0;auto result=check_project(source.source,schema);if(!result)return result;
    const auto graph=member(source.source,source.source.root(),"pipeline_graph");if(!graph.source){out.reset();return {};}
    NumericProcessingGraph candidate;result=decode_numeric_graph(bytes(node(source.source,graph).raw),candidate);if(result)out=std::move(candidate);return result;
}
namespace {
NumericDocumentResult stage_project_graph(const NumericProjectDocument& source,const NumericProcessingGraph& graph,std::string_view section,NumericProjectDocument& out) {
    if(graph.versions.document!=2)return failure("Unsupported document version","schema_version");
    QByteArray snapshot;auto r=encode_numeric_graph(graph,snapshot);if(!r)return r;
    NumericProjectDocument staged_source;r=clone_numeric_project(source,staged_source);if(!r)return r;
    Source fresh;auto d=parse_project_source(view(snapshot),fresh);if(!d)return source_error(d);
    auto root=staged_source.source.root(),old=member(staged_source.source,root,section);
    QByteArray replacement=snapshot;
    if(old.source){
        auto old_id=member(staged_source.source,old,"graph_id");
        if(old_id.source && node(staged_source.source,old_id).decoded!=graph.graph_id)return failure("Graph identity changed; explicit new project required",text(section)+".graph_id");
        replacement=merge(staged_source.source,old,fresh,fresh.root(),"graph",r);if(!r)return r;
    }
    auto quoted=QJsonDocument(QJsonArray{text(section)}).toJson(QJsonDocument::Compact);quoted=quoted.mid(1,quoted.size()-2);QByteArray insertion=quoted+":"+replacement;
    DocumentEdit edit=old.source?DocumentEdit{EditKind::Replace,old,{}, {},node(staged_source.source,old).raw,view(replacement)}
                               :DocumentEdit{EditKind::Insert,{},root,{},node(staged_source.source,root).raw,view(insertion)};
    NumericProjectDocument candidate;d=stage_project_edits(staged_source.source,std::span(&edit,1),candidate.source);if(!d)return source_error(d,text(section));
    std::uint32_t schema=0;r=check_project(candidate.source,schema);if(!r)return r;
    candidate.active_path=source.active_path;candidate.disk_baseline=source.disk_baseline;out=std::move(candidate);return {};
}
}
NumericDocumentResult stage_numeric_draft(const NumericProjectDocument& source,const NumericProcessingGraph& graph,NumericProjectDocument& out) {
    return stage_project_graph(source,graph,"scene_graph",out);
}
NumericDocumentResult create_numeric_project(const NumericProcessingGraph& graph,const NumericDraftPresentation& presentation,NumericProjectDocument& out) {
    NumericProjectDocument candidate;auto result=read_numeric_project("{\"schema_version\":2}",candidate);if(!result)return result;
    result=stage_numeric_draft(candidate,graph,candidate);if(!result)return result;
    result=stage_numeric_presentation(candidate,presentation,candidate);if(result)out=std::move(candidate);return result;
}
NumericDocumentResult create_numeric_startup_project(const NumericProcessingGraph& graph,NumericProjectDocument& out) {
    QByteArray snapshot;auto result=encode_numeric_graph(graph,snapshot);if(!result)return result;
    const QJsonObject display{{"version",1},{"graph_views",QJsonObject{}},{"graph_rendering",QJsonObject{}},{"workspace",QJsonObject{}},
        {"show_raw_samples",true},{"show_amplitudes",true},{"show_test_amplitudes",true},{"show_spectrogram",true},
        {"show_window_signal",false},{"show_windowed_signal",false},{"show_window_function",false},{"show_data_flow",true}};
    NumericDraftPresentation presentation;
    presentation.application_settings=QJsonObject{{"ui",QJsonObject{{"show_raw_samples",true},{"show_amplitudes",true},
        {"show_test_amplitudes",true},{"show_spectrogram",true},{"pipeline_presets",QJsonArray{
        QJsonObject{{"version",1},{"id","builtin-test-signal"},{"name","TestSignal"},
            {"graph",QJsonDocument::fromJson(snapshot).object()},{"presentation",display}}
    }}}}};
    return create_numeric_project(graph,presentation,out);
}
NumericDocumentResult read_numeric_presentation(const NumericProjectDocument& source,NumericDraftPresentation& out) {
    NumericDraftPresentation candidate;NumericDocumentResult result;
    std::uint32_t schema=0;result=check_project(source.source,schema);if(!result)return result;
    const auto& s=source.source;auto graph=member(s,s.root(),"scene_graph"),nodes=member(s,graph,"nodes");
    auto fields=[&](Handle object,std::initializer_list<const char*> keys)->QJsonObject {
        QJsonObject values;
        if(node(s,object).kind!=NodeKind::Object){result=failure("Malformed presentation object");return values;}
        for(auto key:keys){Handle h;auto d=find_unique_member(s,object,key,h);if(d.code==Error::AmbiguousKey){result=source_error(d);return values;}
            if(h.source){auto value=snapshot_leaf(s,h,result);if(!value.isDouble()||!std::isfinite(value.toDouble()))result=failure("Invalid presentation number",key);values[key]=value;}}
        return values;
    };
    for(auto h=node(s,nodes).first_child;h.source && result;h=node(s,h).next_sibling){
        std::int64_t id=0;auto d=read_int64(s,member(s,h,"id"),id);if(!d)return source_error(d);
        const auto key=QString::number(id);auto position=member(s,h,"position");if(position.source)candidate.positions[key]=fields(position,{"x","y"});
        auto presentation=member(s,h,"presentation");if(presentation.source){QJsonObject known;result=check_node_presentation(s,presentation,&known);if(result)candidate.node_presentations[key]=known;}
        auto adapter=member(s,h,"adapter");if(adapter.source){QJsonObject known;result=decode_numeric_node_adapter(bytes(node(s,adapter).raw),known);if(result)candidate.node_adapters[key]=known;}
    }
    auto ui=member(s,s.root(),"ui"),editor=member(s,ui,"editor_view");if(editor.source)candidate.editor_view=fields(editor,{"version","scale","center_x","center_y","node_width","port_spacing","port_radius","port_font"});
    if(result)out=std::move(candidate);return result;
}
NumericDocumentResult read_project_graph_edition(const NumericProjectDocument& source,std::uint32_t& out) {
    std::uint32_t schema=0;auto result=check_project(source.source,schema);if(!result)return result;
    auto graph=member(source.source,source.source.root(),"scene_graph");
    if(!graph.source)return failure("Scene graph missing");
    std::uint32_t version=1;result=edition(source.source,graph,"version",version,"scene_graph",true);if(result)out=version;return result;
}
// This is a read-only UI projection. Unsupported numeric lexemes are omitted,
// and their exact source remains owned by the document, never by QJsonValue.
namespace {
QJsonValue settings_projection(const Source& s,Handle h,bool& complete,bool preset_context) {
    const auto v=node(s,h);
    if(v.kind==NodeKind::Object){QJsonObject object;QSet<QString> keys;for(auto child=v.first_child;child.source;child=node(s,child).next_sibling){const auto key=text(node(s,child).key);if(keys.contains(key)){object.remove(key);complete=false;continue;}keys.insert(key);bool ok=true;auto value=settings_projection(s,child,ok,preset_context);if(!value.isUndefined())object[key]=value;if(!ok)complete=false;}return object;}
    if(v.kind==NodeKind::Array){const bool partial=v.key=="pipeline_presets" || (preset_context&&(v.key=="nodes"||v.key=="connections"||v.key=="values"||v.key=="inputs"||v.key=="outputs"));QJsonArray array;for(auto child=v.first_child;child.source;child=node(s,child).next_sibling){bool ok=true;auto value=settings_projection(s,child,ok,preset_context||v.key=="pipeline_presets");if(!ok){complete=false;if(!partial || value.isUndefined())return QJsonValue(QJsonValue::Undefined);}array.append(value);}return array;}
    if(v.kind==NodeKind::Number && v.raw.find_first_of(".eE")==std::string_view::npos){std::int64_t integer=0;auto d=read_int64(s,h,integer);if(!d || integer>9007199254740991LL || integer<-9007199254740991LL){complete=false;return QJsonValue(QJsonValue::Undefined);}}
    NumericDocumentResult result;auto value=snapshot_leaf(s,h,result);if(!result || (value.isDouble()&&!std::isfinite(value.toDouble()))){complete=false;return QJsonValue(QJsonValue::Undefined);}return value;
}
NumericDocumentResult check_visual_graph(const Source& source,Handle graph,NumericProcessingGraph* out) {
    NumericDocumentResult result;
    NumericProcessingGraph candidate;const auto graph_id=node(source,member(source,graph,"graph_id"));
    if(graph_id.kind!=NodeKind::String||graph_id.decoded.empty()||graph_id.decoded.size()>1024)return failure("Invalid preset graph identity");
    candidate.graph_id.assign(graph_id.decoded);
    if(const auto interval=member(source,graph,"execution_interval");interval.source) {
        Mapper mapper{&source,graph,{},&result,"preset.execution_interval",{}};decode(mapper,interval,candidate.execution_interval);
        if(!result)return result;
        if(!domain::valid_execution_interval(candidate.execution_interval))return failure("Invalid execution interval");
    }
    auto nodes=member(source,graph,"nodes"),connections=member(source,graph,"connections");
    if(node(source,nodes).kind!=NodeKind::Array||node(source,connections).kind!=NodeKind::Array)return failure("Preset graph arrays missing");
    QSet<qint64> node_ids;std::size_t node_count=0;
    for(auto n=node(source,nodes).first_child;n.source;n=node(source,n).next_sibling){if(++node_count>4096)return failure("Preset node limit");result=object_keys(source,n,{"id","internal-data","position"},"preset.node");if(!result)return result;
        std::int64_t node_id=-1;auto d=read_int64(source,member(source,n,"id"),node_id);if(!d||node_id<0||node_id>std::numeric_limits<domain::NodeId>::max()||node_ids.contains(node_id))return failure("Invalid/ambiguous preset node ID");node_ids.insert(node_id);
        auto internal=member(source,n,"internal-data");result=object_keys(source,internal,{"model-name","numeric_processing","presentation","adapter"},"preset.node.internal-data");if(!result)return result;
        auto presentation=member(source,internal,"presentation");if(presentation.source){result=check_node_presentation(source,presentation);if(!result)return result;}
        auto adapter=member(source,internal,"adapter");if(adapter.source){result=check_node_adapter(source,adapter);if(!result)return result;}
        auto typed=member(source,internal,"numeric_processing");if(!typed.source)return failure("Preset typed node snapshot missing");if(node(source,typed).kind!=NodeKind::String)return failure("Invalid preset typed node snapshot");ConfiguredProcessingNode decoded;result=decode_numeric_node(bytes(node(source,typed).decoded),decoded);if(!result)return result;if(decoded.id!=node_id || node(source,member(source,internal,"model-name")).decoded!=decoded.kind)return failure("Preset typed/visual node identity mismatch");
        if(out)candidate.nodes.push_back(std::move(decoded));
    }
    QSet<QString> edges;std::size_t edge_count=0;
    for(auto e=node(source,connections).first_child;e.source;e=node(source,e).next_sibling){if(++edge_count>65536)return failure("Preset edge limit");result=object_keys(source,e,{"outNodeId","outPortIndex","inNodeId","inPortIndex","conversion"},"preset.edge");if(!result)return result;
        for(auto field:{"outNodeId","outPortIndex","inNodeId","inPortIndex"}){std::uint64_t number=0;auto d=read_uint64(source,member(source,e,field),number);const bool port=std::string_view(field)=="outPortIndex"||std::string_view(field)=="inPortIndex";if(!d||number>(port?65535u:static_cast<std::uint64_t>(std::numeric_limits<domain::NodeId>::max())))return failure("Invalid preset edge identity");}
        const auto key=identity(source,e,"connections");if(edges.contains(key))return failure("Duplicate preset edge identity");edges.insert(key);
        if(out){Edge edge;std::uint64_t value=0;
            (void)read_uint64(source,member(source,e,"outNodeId"),value);edge.from=static_cast<NodeId>(value);
            (void)read_uint64(source,member(source,e,"inNodeId"),value);edge.to=static_cast<NodeId>(value);
            (void)read_uint64(source,member(source,e,"outPortIndex"),value);edge.output_port=static_cast<std::uint16_t>(value);
            (void)read_uint64(source,member(source,e,"inPortIndex"),value);edge.input_port=static_cast<std::uint16_t>(value);
            const auto policy=member(source,e,"conversion");if(policy.source){std::uint64_t mode=0;if(!read_uint64(source,policy,mode) || mode>3)return failure("Invalid edge conversion policy");edge.conversion=std::uint8_t(mode);}
            const auto from=std::find_if(candidate.nodes.begin(),candidate.nodes.end(),[&](const auto& n){return n.id==edge.from;});
            const auto to=std::find_if(candidate.nodes.begin(),candidate.nodes.end(),[&](const auto& n){return n.id==edge.to;});
            if(from==candidate.nodes.end() || to==candidate.nodes.end() || edge.output_port>=from->outputs.size() || edge.input_port>=to->inputs.size())return failure("Invalid visual port index");
            edge.output_port=from->outputs[edge.output_port].id;edge.input_port=to->inputs[edge.input_port].id;
            candidate.edges.push_back(edge);
        }
    }
    if(out)*out=std::move(candidate);
    return {};
}
NumericDocumentResult validate_preset_replacement(const Source& source,Handle array) {
    auto result=check_presets(source,array,"ui.pipeline_presets",2);if(!result)return result;
    std::size_t count=0;
    for(auto p=node(source,array).first_child;p.source;p=node(source,p).next_sibling){
        if(++count>4096)return failure("Preset count limit","ui.pipeline_presets");
        auto name=member(source,p,"name");if(name.source&&node(source,name).kind!=NodeKind::String)return failure("Invalid preset name");
    }
    return {};
}
}
NumericDocumentResult read_numeric_application_settings(const NumericProjectDocument& source,QJsonObject& out) {
    std::uint32_t schema=0;auto result=check_project(source.source,schema);if(!result)return result;QJsonObject candidate;
    for(auto key:{"core","ui","wled_ar_server"}){Handle h;auto d=find_unique_member(source.source,source.source.root(),key,h);if(d.code==Error::AmbiguousKey)return source_error(d,key);if(!h.source)continue;if(node(source.source,h).kind!=NodeKind::Object)return failure("Malformed application settings",key);bool complete=true;candidate[key]=settings_projection(source.source,h,complete);}
    out=std::move(candidate);return {};
}
NumericDocumentResult decode_numeric_preset_presentation(QByteArrayView input,QJsonObject& out) {
    Source source;auto diagnostic=parse_project_source(view(input),source);if(!diagnostic)return source_error(diagnostic);
    auto result=check_preset_presentation(source,source.root(),"preset.presentation");if(!result)return result;
    QJsonObject candidate;
    for(auto key:{"version","graph_views","graph_rendering","workspace","show_raw_samples","show_amplitudes","show_test_amplitudes","show_spectrogram","show_window_signal","show_windowed_signal","show_window_function","show_data_flow"}){
        const auto field=member(source,source.root(),key);if(!field.source)continue;bool complete=true;
        auto value=settings_projection(source,field,complete);if(!value.isUndefined())candidate[key]=value;
    }
    out=std::move(candidate);return {};
}
NumericDocumentResult decode_numeric_node_adapter(QByteArrayView input,QJsonObject& out) {
    Source source;auto diagnostic=parse_project_source(view(input),source);if(!diagnostic)return source_error(diagnostic);
    auto result=check_node_adapter(source,source.root());if(!result)return result;QJsonObject candidate;
    for(auto key:{"provider_uuid","stream_id","host","port","device_identity","remote_endpoint"}){
        const auto field=member(source,source.root(),key);if(!field.source)continue;
        if(std::string_view(key)=="remote_endpoint"){
            QJsonObject endpoint;for(auto name:{"graph_id","node_id","direction","port_index"}){auto child=member(source,field,name);if(child.source)endpoint[name]=snapshot_leaf(source,child,result);}if(!endpoint.isEmpty()||!node(source,field).first_child.source)candidate[key]=endpoint;
        }else candidate[key]=snapshot_leaf(source,field,result);
    }
    if(result)out=std::move(candidate);return result;
}
NumericDocumentResult stage_numeric_presentation(const NumericProjectDocument& source,const NumericDraftPresentation& presentation,NumericProjectDocument& out) {
    if(presentation.applied_graph){NumericProjectDocument candidate;auto result=stage_project_graph(source,*presentation.applied_graph,"pipeline_graph",candidate);if(!result)return result;auto rest=presentation;rest.applied_graph.reset();return stage_numeric_presentation(candidate,rest,out);}
    if(!presentation.application_settings.isEmpty()){
        // Leaf deltas preserve all unknown siblings and original lexical values.
        // Registered preset arrays reconcile stable identities against source.
        // Other structural/opaque edits retain their explicit refusal boundary.
        const auto& s=source.source;std::vector<DocumentEdit> edits;std::vector<QByteArray> payloads;payloads.reserve(8192);edits.reserve(8192);NumericDocumentResult result;
        const auto proposed_ui=presentation.application_settings.value("ui").toObject();
        if(proposed_ui.contains("pipeline_presets")){const auto presets=proposed_ui.value("pipeline_presets");if(!presets.isArray())return failure("Expected preset array","ui.pipeline_presets");Source checked;auto encoded=QJsonDocument(presets.toArray()).toJson(QJsonDocument::Compact);auto d=parse_project_source(view(encoded),checked);if(!d)return source_error(d,"ui.pipeline_presets");result=validate_preset_replacement(checked,checked.root());if(!result)return result;}
        auto delta=[&](auto&& self,Handle parent,std::string_view key,const QJsonValue& value,QString path)->bool {
            Handle old;auto d=find_unique_member(s,parent,key,old);if(d.code==Error::AmbiguousKey){result=source_error(d,path);return false;}
            if(path=="ui.pipeline_presets"){
                if(!value.isArray() || (old.source&&node(s,old).kind!=NodeKind::Array)){result=failure("Expected preset array",path);return false;}
                auto encoded=QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact);Source fresh;d=parse_project_source(view(encoded),fresh);if(!d){result=source_error(d,path);return false;}
                result=validate_preset_replacement(fresh,fresh.root());if(!result)return false;
                auto replacement=old.source?merge(s,old,fresh,fresh.root(),"pipeline_presets",result,{},true):encoded;if(!result)return false;
                if(edits.size()>=8192){result=failure("Settings edit limit",path);return false;}
                if(old.source){payloads.push_back(std::move(replacement));edits.push_back({EditKind::Replace,old,{}, {},node(s,old).raw,view(payloads.back())});}
                else{payloads.push_back("\"pipeline_presets\":"+replacement);edits.push_back({EditKind::Insert,{},parent,{},node(s,parent).raw,view(payloads.back())});}return true;
            }
            if(old.source){bool complete=true;auto projected=settings_projection(s,old,complete);if(complete && projected==value)return true;
                if(node(s,old).kind==NodeKind::Object && value.isObject()){const auto object=value.toObject();for(auto it=object.begin();it!=object.end();++it){auto utf8=it.key().toUtf8();if(!self(self,old,view(utf8),it.value(),path+"."+it.key()))return false;}return true;}
                if(complete && node(s,old).kind==NodeKind::Array && value.isArray()){
                    // Registered settings contain primitive vectors and lists,
                    // such as camera coordinates and renderer gradients. Keep
                    // unchanged lexemes; stable-ID objects reconcile by ID.
                    auto encoded=QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact);Source fresh;d=parse_project_source(view(encoded),fresh);if(!d){result=source_error(d,path);return false;}
                    auto has_ids=[](const Source& source,Handle array){bool any=false;for(auto child=node(source,array).first_child;child.source;child=node(source,child).next_sibling){Handle id;auto d=find_unique_member(source,child,"id",id);if(!d||!id.source||(node(source,id).kind!=NodeKind::String&&node(source,id).kind!=NodeKind::Number))return false;any=true;}return any;};
                    const bool old_ids=has_ids(s,old),new_ids=has_ids(fresh,fresh.root());
                    if(old_ids&&!new_ids&&node(fresh,fresh.root()).first_child.source){result=failure("Settings array identity missing",path);return false;}
                    auto replacement=merge(s,old,fresh,fresh.root(),old_ids||new_ids?"settings_ids":key,result,{},true);if(!result)return false;
                    if(edits.size()>=8192){result=failure("Settings edit limit",path);return false;}payloads.push_back(std::move(replacement));edits.push_back({EditKind::Replace,old,{}, {},node(s,old).raw,view(payloads.back())});return true;
                }
                if(!complete || node(s,old).kind==NodeKind::Array || value.isArray() || node(s,old).kind==NodeKind::Object){result=failure("Unsupported structural/opaque settings edit",path);return false;}
            }
            if(edits.size()>=8192){result=failure("Settings edit limit",path);return false;}
            auto encoded=QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);encoded=encoded.mid(1,encoded.size()-2);
            if(old.source){payloads.push_back(encoded);edits.push_back({EditKind::Replace,old,{}, {},node(s,old).raw,view(payloads.back())});}
            else{auto quoted=QJsonDocument(QJsonArray{text(key)}).toJson(QJsonDocument::Compact);quoted=quoted.mid(1,quoted.size()-2);payloads.push_back(quoted+":"+encoded);edits.push_back({EditKind::Insert,{},parent,{},node(s,parent).raw,view(payloads.back())});}return true;
        };
        for(auto it=presentation.application_settings.begin();it!=presentation.application_settings.end();++it){if((it.key()!="core"&&it.key()!="ui"&&it.key()!="wled_ar_server")||!it.value().isObject())return failure("Unregistered application settings section",it.key());auto key=it.key().toUtf8();if(!delta(delta,s.root(),view(key),it.value(),it.key()))return result;}
        NumericProjectDocument staged;auto d=stage_project_edits(s,edits,staged.source);if(!d)return source_error(d);std::uint32_t schema=0;result=check_project(staged.source,schema);if(!result)return result;
        staged.active_path=source.active_path;staged.disk_baseline=source.disk_baseline;auto rest=presentation;rest.application_settings={};return stage_numeric_presentation(staged,rest,out);
    }
    NumericDocumentResult result;const auto& s=source.source;auto root=s.root();
    const auto presentation_edits=static_cast<std::size_t>(presentation.positions.size())+static_cast<std::size_t>(presentation.node_presentations.size())+static_cast<std::size_t>(presentation.node_adapters.size());
    if(presentation_edits>8192)return failure("Presentation edit limit");
    std::vector<DocumentEdit> edits;std::vector<QByteArray> payloads;payloads.reserve(presentation_edits+1u);edits.reserve(presentation_edits+1u);
    auto stage=[&](Handle parent,std::string_view key,const QJsonObject& update)->bool {
        auto known=QJsonDocument(update).toJson(QJsonDocument::Compact);Source fresh;auto d=parse_project_source(view(known),fresh);if(!d){result=source_error(d);return false;}
        Handle old;d=find_unique_member(s,parent,key,old);if(d.code==Error::AmbiguousKey){result=source_error(d);return false;}
        if(old.source && node(s,old).kind!=NodeKind::Object){result=failure("Malformed presentation object",text(key));return false;}
        auto replacement=old.source?merge(s,old,fresh,fresh.root(),key,result,{},false,key=="adapter"?"adapter":""):known;if(!result)return false;
        if(old.source){payloads.push_back(std::move(replacement));edits.push_back({EditKind::Replace,old,{}, {},node(s,old).raw,view(payloads.back())});}
        else {auto quoted=QJsonDocument(QJsonArray{text(key)}).toJson(QJsonDocument::Compact);quoted=quoted.mid(1,quoted.size()-2);payloads.push_back(quoted+":"+replacement);edits.push_back({EditKind::Insert,{},parent,{},node(s,parent).raw,view(payloads.back())});}
        return true;
    };
    auto graph=member(s,root,"scene_graph"),nodes=member(s,graph,"nodes");
    for(auto it=presentation.positions.begin();it!=presentation.positions.end();++it){bool valid=false;const auto id=it.key().toLongLong(&valid);if(!valid||id<0||id>std::numeric_limits<NodeId>::max()||QString::number(id)!=it.key()||!it.value().isObject())return failure("Invalid presentation node ID");
        const auto update=it.value().toObject();for(auto f=update.begin();f!=update.end();++f)if((f.key()!="x"&&f.key()!="y")||!f.value().isDouble()||!std::isfinite(f.value().toDouble()))return failure("Unregistered/invalid position field",f.key());
        Handle matched;for(auto h=node(s,nodes).first_child;h.source;h=node(s,h).next_sibling){std::int64_t node_id=-1;auto d=read_int64(s,member(s,h,"id"),node_id);if(!d)return source_error(d);if(node_id==id){if(matched.source)return failure("Ambiguous presentation node ID");matched=h;}}
        if(!matched.source)return failure("Presentation node is absent");if(!stage(matched,"position",update))return result;
    }
    for(auto it=presentation.node_presentations.begin();it!=presentation.node_presentations.end();++it){
        bool valid=false;const auto id=it.key().toLongLong(&valid);
        if(!valid||id<0||id>std::numeric_limits<NodeId>::max()||QString::number(id)!=it.key()||!it.value().isObject())return failure("Invalid presentation node ID");
        const auto update=it.value().toObject();for(auto field=update.begin();field!=update.end();++field)if(field.key()!="inline_parameters" && field.key()!="display_name" && field.key()!="scalar_inputs")return failure("Unregistered node presentation field",field.key());
        Source checked;const auto encoded=QJsonDocument(update).toJson(QJsonDocument::Compact);auto d=parse_project_source(view(encoded),checked);if(!d)return source_error(d);
        result=check_node_presentation(checked,checked.root());if(!result)return result;
        Handle matched;for(auto h=node(s,nodes).first_child;h.source;h=node(s,h).next_sibling){std::int64_t node_id=-1;d=read_int64(s,member(s,h,"id"),node_id);if(!d)return source_error(d);if(node_id==id){if(matched.source)return failure("Ambiguous presentation node ID");matched=h;}}
        if(!matched.source)return failure("Presentation node is absent");if(!stage(matched,"presentation",update))return result;
    }
    for(auto it=presentation.node_adapters.begin();it!=presentation.node_adapters.end();++it){
        bool valid=false;const auto id=it.key().toLongLong(&valid);
        if(!valid||id<0||id>std::numeric_limits<NodeId>::max()||QString::number(id)!=it.key()||!it.value().isObject())return failure("Invalid adapter node ID");
        const auto update=it.value().toObject();const auto encoded=QJsonDocument(update).toJson(QJsonDocument::Compact);QJsonObject checked;
        result=decode_numeric_node_adapter(encoded,checked);if(!result)return result;if(checked!=update)return failure("Unregistered adapter field");
        Handle matched;for(auto h=node(s,nodes).first_child;h.source;h=node(s,h).next_sibling){std::int64_t node_id=-1;auto d=read_int64(s,member(s,h,"id"),node_id);if(!d)return source_error(d);if(node_id==id){if(matched.source)return failure("Ambiguous adapter node ID");matched=h;}}
        if(!matched.source)return failure("Adapter node is absent");if(!stage(matched,"adapter",update))return result;
    }
    if(!presentation.editor_view.isEmpty()){
        for(auto f=presentation.editor_view.begin();f!=presentation.editor_view.end();++f)if((f.key()!="version"&&f.key()!="scale"&&f.key()!="center_x"&&f.key()!="center_y"&&f.key()!="node_width"&&f.key()!="port_spacing"&&f.key()!="port_radius"&&f.key()!="port_font")||!f.value().isDouble()||!std::isfinite(f.value().toDouble())||(f.key()=="version"&&f.value()!=1)||(f.key()=="scale"&&(f.value().toDouble()<0.2||f.value().toDouble()>3)))return failure("Unregistered/invalid editor-view field",f.key());
        auto ui=member(s,root,"ui"),editor=member(s,ui,"editor_view");
        if(editor.source){std::uint32_t version=1;result=edition(s,editor,"version",version,"ui.editor_view",true);if(!result||version!=1)return failure("Unsupported editor-view version");}
        if(ui.source){if(!stage(ui,"editor_view",presentation.editor_view))return result;}
        else if(!stage(root,"ui",QJsonObject{{"editor_view",presentation.editor_view}}))return result;
    }
    NumericProjectDocument candidate;auto d=stage_project_edits(s,edits,candidate.source);if(!d)return source_error(d);
    std::uint32_t schema=0;result=check_project(candidate.source,schema);if(!result)return result;
    candidate.active_path=source.active_path;candidate.disk_baseline=source.disk_baseline;out=std::move(candidate);return {};
}
NumericDocumentResult load_numeric_project(const QString& path,NumericProjectDocument& out,NumericProcessingGraph& graph) {
    const auto resolved=numeric_project_path(path);QFile file(resolved);
    if(!file.open(QIODevice::ReadOnly))return failure(file.errorString(),resolved);
    if(file.size()>16777216)return failure("Project source limit",resolved);
    const auto raw=file.readAll();NumericProjectDocument candidate;auto r=read_numeric_project(raw,candidate);if(!r)return r;
    NumericProcessingGraph draft;r=read_numeric_draft(candidate,draft);if(!r)return r;
    candidate.active_path=resolved;candidate.disk_baseline=raw;out=std::move(candidate);graph=std::move(draft);return {};
}
NumericDocumentResult save_numeric_project(NumericProjectDocument& source,const QString& path) {
    const auto resolved=numeric_project_path(path.isEmpty()?source.active_path:path);
    std::uint32_t schema=0;auto r=check_project(source.source,schema);if(!r)return r;
    if(schema!=2)return failure("Unsupported document version","schema_version");
    QByteArray existing;const bool exists=QFileInfo::exists(resolved);
    if(exists){QFile file(resolved);if(!file.open(QIODevice::ReadOnly))return failure(file.errorString(),resolved);if(file.size()>16777216)return failure("Destination source limit",resolved);existing=file.readAll();
        NumericProjectDocument destination;r=read_numeric_project(existing,destination);if(!r)return r;
        if((resolved==source.active_path && existing!=source.disk_baseline) || (source.active_path.isEmpty()&&path.isEmpty()))return failure("SourceChanged: explicit reload or SaveAs required",resolved);
    }else if(resolved==source.active_path && !source.disk_baseline.isEmpty())return failure("SourceChanged: original file was removed",resolved);
    const auto directory=QFileInfo(resolved).absoluteDir();if(!directory.exists() && !directory.mkpath("."))return failure("Cannot create project directory",resolved);
    QSaveFile file(resolved);file.setDirectWriteFallback(false);if(!file.open(QIODevice::WriteOnly))return failure(file.errorString(),resolved);
    const auto candidate=bytes(source.source.bytes());if(file.write(candidate)!=candidate.size())return failure(file.errorString(),resolved);
    // Best-effort change check, not cross-process CAS. No silent reload/reapply.
    if(exists){QFile before_commit(resolved);if(!before_commit.open(QIODevice::ReadOnly) || before_commit.readAll()!=existing)return failure("SourceChanged during save",resolved);}
    if(!file.commit())return failure(file.errorString(),resolved);
    source.active_path=resolved;source.disk_baseline=candidate;return {};
}
} // namespace desktop
