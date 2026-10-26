#include <core/diagnostic_saves.h>
#include <desktop/numeric_project_document.h>
#include <project_storage/numeric_array_codec.h>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QSaveFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStringView>
#include <QSet>
#include <QElapsedTimer>
#include <QDebug>
#include <QCache>
#include <QMap>
#include <QUuid>
#include <bit>
#include <charconv>
#include <limits>
#include <optional>
#include <type_traits>
#include <cmath>
#include <algorithm>
#include <array>
#include <span>
#include <utility>

namespace desktop {
namespace {
class DocumentPhaseTrace {
    const char* phase;
    QElapsedTimer timer;
public:
    explicit DocumentPhaseTrace(const char* name):phase(name) {
        if(core::diagnostics::trace_saves())timer.start();
    }
    ~DocumentPhaseTrace() {
        if(timer.isValid())qInfo().noquote()<<"BARELIEF_SAVE_PHASE"<<phase<<"ms="<<timer.nsecsElapsed()/1e6;
    }
};
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
NumericDocumentResult read_bounded_file(QFile& file,QByteArray& out,const QString& path,const char* limit_message) {
    constexpr qint64 limit=16777216;
    if(file.size()>limit)return failure(limit_message,path);
    auto candidate=file.read(limit+1);
    if(candidate.size()>limit)return failure(limit_message,path);
    if(file.error()!=QFileDevice::NoError)return failure(file.errorString(),path);
    out=std::move(candidate);
    return {};
}
NumericDocumentResult unsupported_document_version(std::uint32_t actual) {
    return failure(QStringLiteral("Unsupported document version: found %1, expected 4 (graph 5, processing 4); original file was not changed").arg(actual),"schema_version");
}
NumericDocumentResult source_error(Diagnostic d,QString path={}) { return failure("JSON source error "+QString::number(static_cast<int>(d.code))+" at "+QString::number(d.offset),path); }
NodeView node(const Source& s,Handle h) { NodeView v; (void)s.view(h,v); return v; }
Handle member(const Source& s,Handle h,std::string_view key) { Handle out; (void)find_unique_member(s,h,key,out); return out; }
template<class T> struct Names;
#define NAMES(T,...) template<> struct Names<T>{static constexpr std::string_view values[]={__VA_ARGS__};}
NAMES(DType,"Int8","UInt8","Int16","UInt16","Int32","UInt32","Int64","UInt64","Float32","Float64");
NAMES(Rank,"Scalar","Vector","Matrix");
NAMES(Semantic,"Scalar","PCM","Window","Windowed","SpectrumMagnitude","MelEnergy","History","Numeric");
NAMES(Axis,"None","Sample","Bin","Band","Time","Declared","Component");
NAMES(Quantity,"Unspecified","Amplitude","SpectrumMagnitude","MelEnergy","FilledFrameCount","Packet","SpectrumComplex","SpectrumReal","SpectrumImaginary","SpectrumPhase","SpectrumPeakAmplitude","SpectrumPower","SpectrumPsd","SpectrumAmplitudeDb","SpectrumPowerDb","SpectrumPsdDb","BroadbandEnergy","BroadbandPower","BroadbandRms","BroadbandMaximum","SpectralFlux","NoteSalience","SpectrumRmsAmplitude","SpectrumBinPower","SpectrumWindowScaledMagnitude","BroadbandBinPowerSum","BroadbandBinPowerMean","BroadbandBinPowerRms","BroadbandBinPowerMaximum","BroadbandBinPowerFlux");
NAMES(NumericUnit,"Unspecified","NormalizedAmplitude","RawAmplitude","Frames","Hertz","Seconds","Values","Radians","SquaredAmplitude","SquaredAmplitudePerHertz","Decibels");
NAMES(StorageEncoding,"Native","FixedLittleEndian","FixedBigEndian","PackedSigned24LE","PackedSigned24BE");
NAMES(BitPlacement,"LeastSignificant","MostSignificant");
NAMES(ChannelLayout,"Planar","Interleaved");
NAMES(Normalization,"None","SignedUnit","UnsignedUnit","FloatUnit");
NAMES(CoordinateMode,"None","Regular","ExplicitFloat64LE");
NAMES(Rounding,"TowardZero","NearestTiesEven"); NAMES(Overflow,"Reject","Saturate");
NAMES(PrecisionLoss,"RejectInexact","AllowDeclared"); NAMES(NonFinite,"Reject","ReplaceFinite","ClampFinite");
NAMES(ScaleMode,"Identity","IntegerAffine","DyadicAffine","PcmNormalize","PcmDenormalize","Binary64Affine");
NAMES(ConfigurationRole,"sample_clock","frequency_grid");
NAMES(MixerConsumption,"clocked_fifo","source_aligned");
NAMES(MixerMode,"weighted_sum","weighted_mean","weighted_rms","max_absolute");
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
template<class T> QJsonValue encode_enum(Mapper& m,T value) {
    const auto index=static_cast<std::size_t>(value);
    if(index>=std::size(Names<T>::values)){m.fail("Invalid enum");return {};}
    return text(Names<T>::values[index]);
}
template<class T> QJsonValue encode_float_bits(T value) {
    if constexpr(sizeof(T)==4)
        return QJsonObject{{"dtype","float32"},{"bits",QString::number(std::bit_cast<std::uint32_t>(value),16).rightJustified(8,'0')}};
    else
        return QJsonObject{{"dtype","float64"},{"bits",QString::number(std::bit_cast<std::uint64_t>(value),16).rightJustified(16,'0')}};
}
template<class T> QJsonValue encode_finite_float(Mapper& m,T value) {
    if(!std::isfinite(value)){m.fail("Nonfinite floating point parameter");return {};}
    return encode_float_bits(value);
}
template<class T> QJsonValue encode_integer(T value) {
    if constexpr(sizeof(T)!=8)return static_cast<int>(value);
    else if constexpr(std::is_signed_v<T>)
        return QJsonObject{{"dtype","int64"},{"decimal",QString::number(static_cast<qlonglong>(value))}};
    else return QJsonObject{{"dtype","uint64"},{"decimal",QString::number(static_cast<qulonglong>(value))}};
}
template<class T> QJsonValue encode_object(Mapper& m,const T& value) {
    auto copy=value;
    Mapper child{nullptr,{}, {},m.result,m.path,m.kind};
    map(child,copy);
    return child.output;
}
template<class T> QJsonValue encode(Mapper& m,const T& value) {
    if constexpr(std::is_same_v<T,bool>)return value;
    else if constexpr(std::is_same_v<T,std::string>)return text(value);
    else if constexpr(std::is_enum_v<T>)return encode_enum(m,value);
    else if constexpr(std::is_floating_point_v<T>)return encode_finite_float(m,value);
    else if constexpr(std::is_integral_v<T>)return encode_integer(value);
    else return encode_object(m,value);
}
template<> QJsonValue encode(Mapper&,const std::uint32_t& v) { return QJsonValue(static_cast<qint64>(v)); }
template<> QJsonValue encode(Mapper& m,const NumericLiteral& value) {
    if(validate_literal(value)!=Status::ok){m.fail("Invalid numeric literal");return {};}
    return std::visit([&](const auto& v)->QJsonValue {
        using T=std::decay_t<decltype(v)>;
        if constexpr(std::is_integral_v<T>)return encode(m,v);
        else return encode_float_bits(v);
    },value);
}
template<class T> QJsonValue encode(Mapper& m,const std::vector<T>& value) { QJsonArray a; for(const auto& v:value)a.append(encode(m,v)); return a; }
template<class T,std::size_t N> QJsonValue encode(Mapper& m,const std::array<T,N>& value) { QJsonArray a; for(const auto& v:value)a.append(encode(m,v)); return a; }

bool canonical_decimal(std::string_view payload) {
    if(payload.empty() || payload.front()=='+' || payload=="-0")return false;
    if(payload.front()=='0')return payload.size()==1;
    if(payload.front()=='-')return payload.size()>1 && payload[1]!='0';
    return true;
}
template<class T> void decimal_literal(Mapper& m,std::string_view payload,NumericLiteral& out) {
    T value=0;
    const auto end=payload.data()+payload.size();
    const auto parsed=std::from_chars(payload.data(),end,value);
    if(parsed.ec!=std::errc{} || parsed.ptr!=end)
        m.fail(std::is_signed_v<T>?"Int64 range/grammar":"UInt64 range/grammar");
    else out=value;
}
void integer_literal(Mapper& m,std::string_view dtype,std::string_view payload,NumericLiteral& out) {
    if(!canonical_decimal(payload)){m.fail("Noncanonical decimal");return;}
    if(dtype=="int64")decimal_literal<std::int64_t>(m,payload,out);
    else decimal_literal<std::uint64_t>(m,payload,out);
}
std::uint64_t hexadecimal_literal(Mapper& m,std::string_view payload) {
    std::uint64_t value=0;
    for(char c:payload) {
        const bool decimal=c>='0' && c<='9';
        const bool hex_letter=c>='a' && c<='f';
        if(!decimal && !hex_letter){m.fail("Noncanonical hex");return 0;}
        value=value*16u+static_cast<unsigned>(decimal?c-'0':c-'a'+10);
    }
    return value;
}
void floating_literal(Mapper& m,std::string_view dtype,std::string_view payload,NumericLiteral& out) {
    const auto width=dtype=="float32"?8u:dtype=="float64"?16u:0u;
    if(!width || payload.size()!=width){m.fail("Float literal tag/width");return;}
    const auto bits=hexadecimal_literal(m,payload);
    if(!*m.result)return;
    if(width==8)out=std::bit_cast<float>(static_cast<std::uint32_t>(bits));
    else out=std::bit_cast<double>(bits);
}
bool validate_literal_object(Mapper& m,Handle h) {
    if(node(*m.source,h).kind!=NodeKind::Object){m.fail("Expected tagged literal");return false;}
    constexpr std::string_view known[]={"dtype","decimal","bits"};
    const auto diagnostic=validate_known_keys(*m.source,h,known);
    if(!diagnostic){*m.result=source_error(diagnostic,m.path);return false;}
    return true;
}
void literal(Mapper& parent,Handle h,NumericLiteral& out) {
    Mapper m{parent.source,h,{},parent.result,parent.path,parent.kind};
    if(!validate_literal_object(m,h))return;
    std::string dtype;
    m.field("dtype",dtype);
    const bool integer=dtype=="int64" || dtype=="uint64";
    if(member(*m.source,h,integer?"bits":"decimal").source){m.fail("Mutually exclusive literal payloads");return;}
    std::string payload;
    m.field(integer?"decimal":"bits",payload);
    if(!*m.result)return;
    if(integer)integer_literal(m,dtype,payload,out);
    else floating_literal(m,dtype,payload,out);
}
void decode_string(Mapper& m,Handle h,std::string& value) {
    const auto v=node(*m.source,h);
    if(v.kind!=NodeKind::String)m.fail("Expected string");
    else value.assign(v.decoded);
}
void decode_boolean(Mapper& m,Handle h,bool& value) {
    const auto v=node(*m.source,h);
    if(v.kind!=NodeKind::Boolean)m.fail("Expected boolean");
    else value=v.raw=="true";
}
template<class T> void decode_enum(Mapper& m,Handle h,T& value) {
    std::string name;
    decode_string(m,h,name);
    if(!*m.result)return;
    for(std::size_t i=0;i<std::size(Names<T>::values);++i) {
        if(name==Names<T>::values[i]){value=static_cast<T>(i);return;}
    }
    m.fail("Unknown enum identifier");
}
template<class T> void decode_finite_float(Mapper& m,Handle h,T& value) {
    NumericLiteral n;
    literal(m,h,n);
    if(!*m.result)return;
    if(const auto* p=std::get_if<T>(&n);p && std::isfinite(*p))value=*p;
    else m.fail("Finite floating point width/tag mismatch");
}
template<class T> void decode_tagged_integer(Mapper& m,Handle h,T& value) {
    NumericLiteral n;
    literal(m,h,n);
    if(!*m.result)return;
    if(const auto* p=std::get_if<T>(&n))value=*p;
    else m.fail("Integer width/tag mismatch");
}
template<class T> void decode_bounded_integer(Mapper& m,Handle h,T& value) {
    std::int64_t n=0;
    const auto diagnostic=read_int64(*m.source,h,n);
    if(!diagnostic || n<static_cast<std::int64_t>(std::numeric_limits<T>::min())
        || n>static_cast<std::int64_t>(std::numeric_limits<T>::max()))m.fail("Bounded integer range/grammar");
    else value=static_cast<T>(n);
}
template<class T> void decode_integer(Mapper& m,Handle h,T& value) {
    if constexpr(sizeof(T)==8)decode_tagged_integer(m,h,value);
    else decode_bounded_integer(m,h,value);
}
template<class T> void decode_object(Mapper& m,Handle h,T& value) {
    if(node(*m.source,h).kind!=NodeKind::Object){m.fail("Expected object");return;}
    Mapper child{m.source,h,{},m.result,m.path,m.kind};
    map(child,value);
}
template<class T> void decode(Mapper& m,Handle h,T& value) {
    if(!*m.result)return;
    if constexpr(std::is_same_v<T,std::string>)decode_string(m,h,value);
    else if constexpr(std::is_same_v<T,bool>)decode_boolean(m,h,value);
    else if constexpr(std::is_enum_v<T>)decode_enum(m,h,value);
    else if constexpr(std::is_floating_point_v<T>)decode_finite_float(m,h,value);
    else if constexpr(std::is_integral_v<T>)decode_integer(m,h,value);
    else decode_object(m,h,value);
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
void structured_contract(Mapper& m,StructuredContract& value,std::string& schema,std::string& encoding) {
    m.field("schema",schema);
    m.field("version",value.version);
    m.field("encoding",encoding);
    m.field("payload_bytes",value.payload_bytes);
    if(m.reading()){value.schema=schema;value.encoding=encoding;}
}
void encode_contract_fields(Mapper& m,DataContract& data) {
    if(auto* value=std::get_if<NumericContract>(&data)) {
        std::vector<std::uint32_t> channels(value->channel_order.begin(),value->channel_order.end());
        std::string clock(value->clock.domain);
        numeric_contract(m,*value,channels,clock);
    } else {
        auto& packet=std::get<StructuredContract>(data);
        std::string schema(packet.schema),encoding(packet.encoding);
        structured_contract(m,packet,schema,encoding);
    }
}
template<> QJsonValue encode(Mapper& parent,const OwnedProcessingContract& owned) {
    Mapper m{nullptr,{}, {},parent.result,parent.path,parent.kind};
    if(!owned){m.fail("Missing owned contract");return {};}
    auto data=owned.view();std::string type=std::holds_alternative<NumericContract>(data)?"numeric":"structured";m.field("type",type);
    encode_contract_fields(m,data);
    if(validate_contract(data,frame_limits)!=Status::ok)m.fail("Invalid contract");return m.output;
}
struct ContractReadFields {
    std::vector<std::uint32_t> channels;
    std::string clock,schema,encoding;
    DataContract read(Mapper& m,std::string_view type) {
        if(type=="numeric") {
            NumericContract value;
            numeric_contract(m,value,channels,clock);
            return value;
        }
        if(type=="structured") {
            StructuredContract value;
            structured_contract(m,value,schema,encoding);
            return value;
        }
        m.fail("Unknown contract category");
        return {};
    }
};
template<> void decode(Mapper& parent,Handle h,OwnedProcessingContract& owned) {
    Mapper m{parent.source,h,{},parent.result,parent.path,parent.kind};
    std::string type;
    m.field("type",type);
    ContractReadFields fields;
    const auto data=fields.read(m,type);
    if(*m.result && own_processing_contract(data,frame_limits,16777216,owned)!=Status::ok)m.fail("Invalid/bounded contract");
}
void map(Mapper& m,signal_contracts::OscillatorParameters& v) {FIELD(id);FIELD(enabled);FIELD(frequency_hz);FIELD(amplitude);FIELD(phase_radians);FIELD(attack_seconds);FIELD(release_seconds);}
void map(Mapper& m,MixerInputWeight& v) {FIELD(port);FIELD(weight);}
void map(Mapper& m,ProcessingPort& v) {FIELD(id);FIELD(contract);} // Direction comes from the containing field.
template<> QJsonValue encode(Mapper& m,const NumericArray& value) {
    auto payload=encode_array_generator_values(value.values);
    if(payload.empty()){m.fail("Invalid numeric array");return {};}
    return QJsonObject{{"type","numeric_array"},{"values",text(payload)}};
}
void decode_numeric_array(Mapper& m,Handle h,ProcessingParameterValue& value) {
    Mapper child{m.source,h,{},m.result,m.path,m.kind};
    std::string payload;child.field("values",payload);
    if(!*m.result)return;
    NumericArray candidate;
    if(parse_numeric_array(payload,candidate)!=Status::ok){m.fail("Invalid numeric array payload");return;}
    value=std::move(candidate);
}
bool is_numeric_array(Mapper& m,Handle h) {
    const auto type=member(*m.source,h,"type");
    return type.source && node(*m.source,type).decoded=="numeric_array";
}
template<> QJsonValue encode(Mapper& m,const ProcessingParameterValue& v) {return std::visit([&](const auto& x){return encode(m,x);},v);}
template<> void decode(Mapper& m,Handle h,ProcessingParameterValue& v) {
    const auto k=node(*m.source,h).kind;
    if(k==NodeKind::Boolean){bool x=false;decode(m,h,x);v=x;}
    else if(k==NodeKind::String){std::string x;decode(m,h,x);v=std::move(x);}
    else if(k==NodeKind::Object && is_numeric_array(m,h))decode_numeric_array(m,h,v);
    else {NumericLiteral x;decode(m,h,x);v=x;}
}
bool read_parameter_array_payload(Mapper& m,ProcessingParameter& v) {
    const auto kind=domain::node_kind(m.kind);
    const auto* descriptor=kind?parameter_descriptor(*kind,v.key):nullptr;
    const auto value=member(*m.source,m.handle,"value");
    if(!descriptor || descriptor->value_kind!=ParameterValueKind::NumericArray
        || node(*m.source,value).kind!=NodeKind::String)return false;
    std::string payload;m.field("value",payload);
    if(!*m.result)return true;
    NumericArray decoded;
    if(!std::string_view(payload).starts_with("f32le-base64:")
        || parse_numeric_array(payload,decoded)!=Status::ok) {m.fail("Invalid numeric array payload");return true;}
    v.value=std::move(decoded);
    return true;
}
void map(Mapper& m,ProcessingParameter& v) {
    FIELD(key);
    if(m.reading() && read_parameter_array_payload(m,v))return;
    FIELD(value);
}
bool registered(std::string_view kind,std::string_view key) {
    auto k=node_kind(kind);if(!k)return false;auto* d=parameter_descriptor(*k,key);
    return d && d->classification==ParameterClassification::Processing && key!="enabled";
}
bool validate_parameter_keys(Mapper& m,const std::vector<ProcessingParameter>& values) {
    QSet<QString> seen;
    for(const auto& value:values) {
        const auto key=text(value.key);
        if(!registered(m.kind,value.key) || seen.contains(key)) {
            m.fail("Unregistered/duplicate typed parameter", ".values");
            return false;
        }
        seen.insert(key);
    }
    return true;
}
void read_parameter_value(Mapper& m,Handle handle,QSet<QString>& seen,
    std::vector<ProcessingParameter>& values) {
    Mapper child{m.source,handle,{},m.result,m.path+".values",m.kind};
    std::string key;
    child.field("key",key);
    if(!*m.result)return;
    if(seen.contains(text(key))){m.fail("Duplicate parameter key");return;}
    seen.insert(text(key));
    if(!registered(m.kind,key))return;
    ProcessingParameter candidate;
    decode(child,handle,candidate);
    if(*m.result)values.push_back(std::move(candidate));
}
void read_parameter_values(Mapper& m,std::vector<ProcessingParameter>& values) {
    const auto array=m.get("values");
    const auto value=node(*m.source,array);
    if(value.kind!=NodeKind::Array){m.fail("Expected parameter array");return;}
    QSet<QString> seen;
    for(auto handle=value.first_child;handle.source && *m.result;handle=node(*m.source,handle).next_sibling)
        read_parameter_value(m,handle,seen,values);
}
void parameter_values(Mapper& m,std::vector<ProcessingParameter>& values) {
    if(m.reading())read_parameter_values(m,values);
    else if(validate_parameter_keys(m,values))m.field("values",values);
}
void parameter_fields(Mapper& m,EndpointProcessingParameters& value) {parameter_values(m,value.values);}
void parameter_fields(Mapper& m,TransformProcessingParameters& value) {parameter_values(m,value.values);}
void parameter_fields(Mapper& m,ConverterProcessingParameters& value) {m.field("policy",value.policy);}
void parameter_fields(Mapper& m,RouteProcessingParameters& value) {
    m.field("outputs",value.outputs);
    m.field("selected",value.selected);
    m.field("open",value.open);
    m.field("channels_per_stream",value.channels_per_stream);
}
void parameter_fields(Mapper& m,ReduceProcessingParameters& value) {
    m.field("operation",value.operation);
    m.field("channels",value.channels);
    m.field("rows",value.rows);
    m.field("selected_channels",value.selected_channels);
    m.field("precision_loss",value.precision_loss);
    m.field("rounding",value.rounding);
}
void parameter_fields(Mapper& m,HistoryProcessingParameters& value) {m.field("capacity",value.capacity);}
void parameter_fields(Mapper& m,SignalGeneratorProcessingParameters& value) {
    m.field("oscillators",value.oscillators);
    if(value.oscillators.empty() || value.oscillators.size()>signal_contracts::max_oscillators)m.fail("Oscillator count");
}
void parameter_fields(Mapper& m,SampleClockProcessingParameters& value) {
    m.field("sample_rate",value.sample_rate);
    m.field("domain",value.domain);
}
void parameter_fields(Mapper& m,MixerProcessingParameters& value) {
    m.field("mode",value.mode);
    m.field("weights",value.weights);
    m.field("fifo_milliseconds",value.fifo_milliseconds);
    m.field("consumption",value.consumption);
}
template<class T> void map_parameter_category(Mapper& m,ProcessingParameters& parameters) {
    auto candidate=m.reading()?T{}:std::get<T>(parameters);
    parameter_fields(m,candidate);
    if(*m.result)parameters=std::move(candidate);
}
struct ParameterCategory {
    const char* name;
    bool (*matches)(const ProcessingParameters&);
    void (*map)(Mapper&,ProcessingParameters&);
};
template<class T> ParameterCategory parameter_category(const char* name) {
    return {name,[](const ProcessingParameters& value){return std::holds_alternative<T>(value);},
        &map_parameter_category<T>};
}
const auto parameter_categories=std::array{
    parameter_category<EndpointProcessingParameters>("endpoint"),
    parameter_category<TransformProcessingParameters>("transform"),
    parameter_category<ConverterProcessingParameters>("converter"),
    parameter_category<RouteProcessingParameters>("route"),
    parameter_category<ReduceProcessingParameters>("reduce"),
    parameter_category<HistoryProcessingParameters>("history"),
    parameter_category<SignalGeneratorProcessingParameters>("generator"),
    parameter_category<SampleClockProcessingParameters>("clock"),
    parameter_category<MixerProcessingParameters>("mixer")};
std::string parameter_category_name(const ProcessingParameters& value) {
    const auto found=std::find_if(parameter_categories.begin(),parameter_categories.end(),
        [&](const auto& category){return category.matches(value);});
    return found==parameter_categories.end()?std::string{}:found->name;
}
void parameters(Mapper& m,ProcessingParameters& value) {
    std::string type=m.reading()?std::string{}:parameter_category_name(value);
    m.field("type",type);
    if(!*m.result)return;
    const auto found=std::find_if(parameter_categories.begin(),parameter_categories.end(),
        [&](const auto& category){return type==category.name;});
    if(found==parameter_categories.end()){m.fail("Unknown parameter category");return;}
    found->map(m,value);
}
template<> QJsonValue encode(Mapper& parent,const ProcessingParameters& v) {auto copy=v;Mapper m{nullptr,{}, {},parent.result,parent.path,parent.kind};parameters(m,copy);return m.output;}
template<> void decode(Mapper& parent,Handle h,ProcessingParameters& v) {Mapper m{parent.source,h,{},parent.result,parent.path,parent.kind};parameters(m,v);}
void validate_port_ids(Mapper& m,const std::vector<ProcessingPort>& ports) {
    QSet<int> ids;
    for(const auto& port:ports) {
        if(ids.contains(port.id)){m.fail("Duplicate port ID");return;}
        ids.insert(port.id);
    }
}
void node_ports(Mapper& m,ConfiguredProcessingNode& v) {
    m.field("inputs",v.inputs);
    m.field("outputs",v.outputs);
    for(auto& port:v.inputs)port.direction=PortDirection::Input;
    for(auto& port:v.outputs)port.direction=PortDirection::Output;
    validate_port_ids(m,v.inputs);
    validate_port_ids(m,v.outputs);
}
void map(Mapper& m,ConfiguredProcessingNode& v) {
    if(!m.reading() && (v.inputs.size()>64 || v.outputs.size()>64 || v.kind.size()>1024)){m.fail("Node limits");return;}
    FIELD(id);FIELD(kind);FIELD(enabled);m.kind=v.kind;
    if(v.id<0 || (v.kind!="Converter" && !node_kind(v.kind))){m.fail("Invalid node identity/kind");return;}
    FIELD(parameters);
    node_ports(m,v);
}
void map(Mapper& m,Edge& v) {m.field("out_id",v.from);m.field("out_index",v.output_port);m.field("in_id",v.to);m.field("in_index",v.input_port);
    if(!m.reading() || member(*m.source,m.handle,"conversion").source)m.field("conversion",v.conversion);
    if(v.conversion>3)m.fail("Invalid edge conversion policy");
}
void map(Mapper& m,ConfigurationBinding& v) {m.field("from",v.from);m.field("to",v.to);m.field("output",v.output);m.field("input",v.input);}
void validate_graph_node_ids(Mapper& m,const NumericProcessingGraph& graph) {
    QSet<int> ids;
    for(const auto& node:graph.nodes) {
        if(ids.contains(node.id)){m.fail("Duplicate node ID");return;}
        ids.insert(node.id);
    }
}
QString edge_identity(const Edge& edge) {
    return QString::number(edge.from)+":"+QString::number(edge.output_port)+":"
        +QString::number(edge.to)+":"+QString::number(edge.input_port);
}
void validate_graph_edge_ids(Mapper& m,const NumericProcessingGraph& graph) {
    QSet<QString> identities;
    for(const auto& edge:graph.edges) {
        const auto key=edge_identity(edge);
        if(identities.contains(key)){m.fail("Duplicate edge identity");return;}
        identities.insert(key);
    }
}
void graph_execution_interval(Mapper& m,NumericProcessingGraph& graph) {
    if(!m.reading() || member(*m.source,m.handle,"execution_interval").source)
        m.field("execution_interval",graph.execution_interval);
    if(!domain::valid_execution_interval(graph.execution_interval)) {
        m.path+=".execution_interval";
        m.fail("Invalid execution interval");
    }
}
void graph_header(Mapper& m,NumericProcessingGraph& graph) {
    m.field("version",graph.versions.graph);
    m.field("processing_version",graph.versions.processing);
    if(graph.versions.graph!=5 || graph.versions.processing!=4){m.fail("Unsupported graph/processing version");return;}
    m.field("graph_id",graph.graph_id);
    graph_execution_interval(m,graph);
}
void map(Mapper& m,NumericProcessingGraph& v) {
    if(!m.reading() && (v.nodes.size()>4096 || v.edges.size()>65536 || v.graph_id.size()>1024)){m.fail("Graph limits");return;}
    graph_header(m,v);
    if(!*m.result)return;
    FIELD(nodes);m.field("connections",v.edges);FIELD(configuration_bindings);
    if(v.nodes.size()>4096 || v.edges.size()>65536 || v.graph_id.empty()){m.fail("Graph bounds/identity");return;}
    validate_graph_node_ids(m,v);
    validate_graph_edge_ids(m,v);
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
NumericDocumentResult check_adapter_strings(const Source& s,Handle h) {
    for(auto key:{"provider_uuid","stream_id","host","device_identity"}){
        const auto field=member(s,h,key);if(!field.source)continue;const auto value=node(s,field);
        if(value.kind!=NodeKind::String||value.decoded.empty()||value.decoded.size()>256)return failure("Invalid adapter string","node.adapter."+QString(key));
        if(std::string_view(key)=="provider_uuid" && QUuid(text(value.decoded)).isNull())return failure("Invalid provider UUID","node.adapter.provider_uuid");
    }
    return {};
}
NumericDocumentResult check_adapter_port(const Source& s,Handle h) {
    const auto port=member(s,h,"port");
    if(!port.source)return {};
    std::uint64_t value=0;
    const auto diagnostic=read_uint64(s,port,value);
    if(!diagnostic || value<1 || value>65535)return failure("Invalid adapter port","node.adapter.port");
    return {};
}
NumericDocumentResult check_endpoint_graph(const Source& s,Handle h) {
    const auto graph=member(s,h,"graph_id");
    if(!graph.source)return {};
    const auto value=node(s,graph);
    if(value.kind!=NodeKind::String || value.decoded.empty() || value.decoded.size()>256)
        return failure("Invalid endpoint graph ID","node.adapter.remote_endpoint.graph_id");
    return {};
}
NumericDocumentResult check_endpoint_direction(const Source& s,Handle h) {
    const auto direction=member(s,h,"direction");
    if(!direction.source)return {};
    const auto value=node(s,direction);
    if(value.kind!=NodeKind::String || (value.decoded!="input" && value.decoded!="output"))
        return failure("Invalid endpoint direction","node.adapter.remote_endpoint.direction");
    return {};
}
NumericDocumentResult check_endpoint_integer(const Source& s,Handle h,std::string_view key,std::uint64_t maximum) {
    const auto field=member(s,h,key);
    if(!field.source)return {};
    std::uint64_t value=0;
    const auto diagnostic=read_uint64(s,field,value);
    if(!diagnostic || value>maximum)return failure("Invalid endpoint integer","node.adapter.remote_endpoint."+text(key));
    return {};
}
NumericDocumentResult check_remote_endpoint(const Source& s,Handle h) {
    const auto endpoint=member(s,h,"remote_endpoint");
    if(!endpoint.source)return {};
    auto result=object_keys(s,endpoint,{"graph_id","node_id","direction","port_index"},"node.adapter.remote_endpoint");
    if(result)result=check_endpoint_graph(s,endpoint);
    if(result)result=check_endpoint_direction(s,endpoint);
    if(result)result=check_endpoint_integer(s,endpoint,"node_id",std::numeric_limits<NodeId>::max());
    if(result)result=check_endpoint_integer(s,endpoint,"port_index",65535);
    return result;
}
NumericDocumentResult check_node_adapter(const Source& s,Handle h) {
    auto result=object_keys(s,h,{"provider_uuid","stream_id","host","port","device_identity","remote_endpoint"},"node.adapter");
    if(result)result=check_adapter_strings(s,h);
    if(result)result=check_adapter_port(s,h);
    if(result)result=check_remote_endpoint(s,h);
    return result;
}
NumericDocumentResult check_node_presentation(const Source&,Handle,QJsonObject* out=nullptr);
bool valid_display_text(const QString& value) {
    return value.size()<=256 && std::none_of(value.begin(),value.end(),[](QChar character){return character.category()==QChar::Other_Control;});
}
class NodePresentationReader {
    const Source& source;
    Handle root;
    QJsonObject candidate;
    NumericDocumentResult label(Handle entry,QJsonObject& labels) const {
        const auto field=node(source,entry);const auto key=text(field.key);
        const int prefix=key.startsWith("input")?5:key.startsWith("output")?6:0;
        bool valid=false;const auto id=QStringView(key).mid(prefix).toUInt(&valid);
        if(!prefix || !valid || id>65535 || QString::number(id)!=key.mid(prefix) || field.kind!=NodeKind::String)return failure("Invalid port label");
        const auto value=text(field.decoded);
        if(value.size()>256)return failure("Port label limit");
        if(!valid_display_text(value))return failure("Invalid port label text");
        labels[key]=value;return {};
    }
    NumericDocumentResult label_list(Handle handle,QJsonObject& values) const {
        unsigned count=0;
        for(auto entry=node(source,handle).first_child;entry.source;entry=node(source,entry).next_sibling){
            if(++count>512)return failure("Invalid port label");
            const auto result=label(entry,values);if(!result)return result;
        }
        return {};
    }
    NumericDocumentResult labels() {
        const auto handle=member(source,root,"port_labels");if(!handle.source)return {};
        if(node(source,handle).kind!=NodeKind::Object)return failure("Expected port labels object");
        QJsonObject values;
        const auto result=label_list(handle,values);
        if(result)candidate["port_labels"]=values;
        return result;
    }
    NumericDocumentResult boolean(Handle handle,const char* key,const char* message,QString path={}) {
        if(!handle.source)return {};
        const auto value=node(source,handle);
        if(value.kind!=NodeKind::Boolean)return failure(message,path);
        candidate[key]=value.raw=="true";return {};
    }
    NumericDocumentResult stream(Handle entry,QJsonArray& values) const {
        const auto value=node(source,entry);
        if(values.size()>=64 || value.kind!=NodeKind::String)return failure("Invalid stream name");
        const auto name=text(value.decoded);
        if(name.size()>256)return failure("Stream name limit");
        if(!valid_display_text(name))return failure("Invalid stream name");
        values.append(name);
        return {};
    }
    NumericDocumentResult streams() {
        const auto handle=member(source,root,"stream_names");if(!handle.source)return {};
        if(node(source,handle).kind!=NodeKind::Array)return failure("Expected stream names array");
        QJsonArray values;
        for(auto entry=node(source,handle).first_child;entry.source;entry=node(source,entry).next_sibling){
            const auto result=stream(entry,values);
            if(!result)return result;
        }
        candidate["stream_names"]=values;return {};
    }
    NumericDocumentResult scalar_enabled(Handle entry,QJsonObject& input) const {
        const auto enabled=member(source,entry,"enabled");
        if(!enabled.source)return {};
        const auto value=node(source,enabled);
        if(value.kind!=NodeKind::Boolean)return failure("Expected scalar input enabled boolean");
        input["enabled"]=value.raw=="true";
        return {};
    }
    NumericDocumentResult scalar(Handle entry,unsigned count,QJsonObject& values) const {
        const auto key=text(node(source,entry).key);bool valid=false;const auto id=key.toUInt(&valid);
        if(count>domain::max_variadic_inputs || !valid || id>65535 || QString::number(id)!=key)return failure("Invalid scalar input ID");
        QJsonObject input;
        auto result=check_node_presentation(source,entry,&input);
        if(result)result=scalar_enabled(entry,input);
        if(!result)return result;
        values[key]=input;return {};
    }
    NumericDocumentResult scalars() {
        const auto handle=member(source,root,"scalar_inputs");if(!handle.source)return {};
        if(node(source,handle).kind!=NodeKind::Object)return failure("Expected scalar input settings object");
        QJsonObject values;unsigned count=0;
        for(auto entry=node(source,handle).first_child;entry.source;entry=node(source,entry).next_sibling){const auto result=scalar(entry,++count,values);if(!result)return result;}
        candidate["scalar_inputs"]=values;return {};
    }
    NumericDocumentResult display_name() {
        const auto handle=member(source,root,"display_name");if(!handle.source)return {};
        const auto value=node(source,handle);
        if(value.kind!=NodeKind::String)return failure("Expected display name string","node.presentation.display_name");
        const auto name=text(value.decoded);
        if(name.size()>256)return failure("Display name limit","node.presentation.display_name");
        if(!valid_display_text(name))return failure("Invalid display name","node.presentation.display_name");
        candidate["display_name"]=name;return {};
    }
    NumericDocumentResult inline_key(Handle item,QJsonArray& keys,QSet<QString>& seen) const {
        const auto value=node(source,item);
        if(keys.size()>=128 || value.kind!=NodeKind::String || value.decoded.empty() || value.decoded.size()>1024)
            return failure("Invalid/bounded inline parameter key","node.presentation.inline_parameters");
        const auto name=text(value.decoded);
        if(seen.contains(name))return failure("Duplicate inline parameter key","node.presentation.inline_parameters");
        seen.insert(name);
        keys.append(name);
        return {};
    }
    NumericDocumentResult inline_keys() {
        const auto handle=member(source,root,"inline_parameters");if(!handle.source)return {};
        if(node(source,handle).kind!=NodeKind::Array)return failure("Expected inline parameter array","node.presentation.inline_parameters");
        QJsonArray keys;QSet<QString> seen;
        for(auto item=node(source,handle).first_child;item.source;item=node(source,item).next_sibling){
            const auto result=inline_key(item,keys,seen);
            if(!result)return result;
        }
        candidate["inline_parameters"]=keys;return {};
    }
    NumericDocumentResult port_id(Handle item,QJsonArray& ids,QSet<int>& seen) const {
        std::uint64_t id=0;
        if(ids.size()>=128 || !read_uint64(source,item,id) || id>65535 || seen.contains(int(id)))
            return failure("Invalid port order");
        seen.insert(int(id));
        ids.append(int(id));
        return {};
    }
    NumericDocumentResult port_order(const char* name) {
        const auto handle=member(source,root,name);if(!handle.source)return {};
        if(node(source,handle).kind!=NodeKind::Array)return failure("Expected port order array");
        QJsonArray ids;QSet<int> seen;
        for(auto item=node(source,handle).first_child;item.source;item=node(source,item).next_sibling){
            const auto result=port_id(item,ids,seen);
            if(!result)return result;
        }
        candidate[name]=ids;return {};
    }
    NumericDocumentResult read_lists() {
        auto result=streams();if(result)result=scalars();if(result)result=display_name();if(result)result=inline_keys();
        if(result)result=port_order("input_port_order");if(result)result=port_order("output_port_order");return result;
    }
public:
    NodePresentationReader(const Source& input,Handle handle):source(input),root(handle){}
    NumericDocumentResult read(QJsonObject* out) {
        auto result=object_keys(source,root,{"inline_parameters","input_port_order","output_port_order","port_labels","display_name","scalar_inputs","stream_names","show_dc_outputs"},"node.presentation");
        if(result)result=labels();
        if(result)result=boolean(member(source,root,"show_dc_outputs"),"show_dc_outputs","Expected DC visibility boolean","node.presentation.show_dc_outputs");
        if(result)result=read_lists();
        if(result && out)*out=std::move(candidate);return result;
    }
};
NumericDocumentResult check_node_presentation(const Source& source,Handle handle,QJsonObject* out) {
    return NodePresentationReader(source,handle).read(out);
}
NumericDocumentResult check_graph_versions(const Source& source,Handle graph,QString path,std::uint32_t schema) {
    std::uint32_t graph_version=0,processing=0;
    auto result=edition(source,graph,"version",graph_version,path);
    if(!result)return result;
    result=edition(source,graph,"processing_version",processing,path);
    if(!result)return result;
    if(graph_version==5 && processing==4 && schema==4)return {};
    return failure(QStringLiteral("Unsupported graph/processing version combination: found document %1, graph %2, processing %3; expected 4/5/4")
        .arg(schema).arg(graph_version).arg(processing),path);
}
NumericDocumentResult graph_representation(const Source& source,Handle graph,QString path,bool& visual) {
    bool canonical=false;
    const auto nodes=member(source,graph,"nodes");
    for(auto child=node(source,nodes).first_child;child.source;child=node(source,child).next_sibling) {
        if(member(source,child,"kind").source)canonical=true;
        else if(member(source,child,"internal-data").source)visual=true;
        else return failure("Unknown preset node representation",path);
    }
    if(canonical && visual)return failure("Mixed preset graph representations",path);
    return {};
}
using NodeExtensionValidator=NumericDocumentResult (*)(const Source&,Handle);
NumericDocumentResult check_node_extension(const Source& source,Handle node_handle,const char* key,
    const QString& path,NodeExtensionValidator validate) {
    Handle extension;
    const auto found=find_unique_member(source,node_handle,key,extension);
    if(found.code==Error::AmbiguousKey)return source_error(found,path+".nodes."+key);
    return extension.source?validate(source,extension):NumericDocumentResult{};
}
NumericDocumentResult check_graph_extensions(const Source& source,Handle graph,const QString& path) {
    const auto nodes=member(source,graph,"nodes");
    for(auto child=node(source,nodes).first_child;child.source;child=node(source,child).next_sibling) {
        auto result=check_node_extension(source,child,"presentation",path,
            [](const Source& input,Handle handle){return check_node_presentation(input,handle);});
        if(!result)return result;
        result=check_node_extension(source,child,"adapter",path,&check_node_adapter);
        if(!result)return result;
    }
    return {};
}
bool parameter_value_matches_kind(const ProcessingParameterValue& value,ParameterValueKind kind) {
    switch(kind) {
    case ParameterValueKind::Boolean:return std::holds_alternative<bool>(value);
    case ParameterValueKind::Integer:case ParameterValueKind::Number:
        return std::holds_alternative<NumericLiteral>(value);
    case ParameterValueKind::String:case ParameterValueKind::WindowFunction:
        return std::holds_alternative<std::string>(value);
    case ParameterValueKind::NumericArray:return std::holds_alternative<NumericArray>(value);
    case ParameterValueKind::FrameType:return false;
    }
    return false;
}
const std::vector<ProcessingParameter>* node_parameter_values(const ConfiguredProcessingNode& configured) {
        const auto* endpoint=std::get_if<EndpointProcessingParameters>(&configured.parameters);
        const auto* transform=std::get_if<TransformProcessingParameters>(&configured.parameters);
        if(!endpoint && !transform)return nullptr;
        return endpoint?&endpoint->values:&transform->values;
}
NumericDocumentResult check_node_parameter_types(const ConfiguredProcessingNode& configured,const QString& path) {
        const auto kind=domain::node_kind(configured.kind);
        if(!kind)return {};
        const auto* values=node_parameter_values(configured);
        if(!values)return {};
        for(const auto& parameter:*values) {
            const auto* descriptor=parameter_descriptor(*kind,parameter.key);
            if(!descriptor || descriptor->classification!=ParameterClassification::Processing)continue;
            if(!parameter_value_matches_kind(parameter.value,descriptor->value_kind))
                return failure("Parameter value type does not match registered descriptor",
                    path+".nodes."+QString::number(configured.id)+".parameters."+text(parameter.key));
        }
    return {};
}
NumericDocumentResult check_registered_parameter_types(const NumericProcessingGraph& graph,const QString& path) {
    for(const auto& configured:graph.nodes){
        const auto result=check_node_parameter_types(configured,path);
        if(!result)return result;
    }
    return {};
}
NumericDocumentResult check_numeric_graph(const Source& source,Handle graph,const QString& path) {
    NumericProcessingGraph known;
    const auto result=read_snapshot(bytes(node(source,graph).raw),known);
    if(!result)return result;
    const auto typed=check_registered_parameter_types(known,path);
    return typed?check_graph_extensions(source,graph,path):typed;
}
NumericDocumentResult validate_graph(const Source& source,Handle graph,const QString& path,std::uint32_t schema,bool allow_visual) {
    auto result=object_keys(source,graph,{"version","processing_version","graph_id","execution_interval","nodes","connections","configuration_bindings"},path);
    if(!result)return result;
    result=check_graph_versions(source,graph,path,schema);
    if(!result)return result;
    bool visual=false;
    if(allow_visual)result=graph_representation(source,graph,path,visual);
    if(!result)return result;
    if(visual)return check_visual_graph(source,graph);
    return check_numeric_graph(source,graph,path);
}
NumericDocumentResult check_graph(const Source& source,Handle graph,QString path,std::uint32_t schema,bool allow_visual=false) {
    // Only exact immutable bytes with the same admission policy share a result.
    // Failures remain uncached so diagnostics retain the caller's source path.
    // The byte budget bounds retention even for large profile/preset graphs.
    thread_local QCache<QByteArray,bool> validated(8*1024*1024);
    auto key=bytes(node(source,graph).raw);
    key.prepend(allow_visual?'v':'n');key.prepend(QByteArray::number(schema)+':');
    if(validated.contains(key))return {};
    auto result=validate_graph(source,graph,path,schema,allow_visual);
    if(result && key.size()<=validated.maxCost()) {
        const auto cost=key.size()+qsizetype(sizeof(bool));
        validated.insert(key,new bool(true),cost);
    }
    return result;
}
bool registered_display_member(std::string_view context,std::string_view key);
NumericDocumentResult check_display_dictionary(const Source& source,Handle handle,const QString& path) {
    if(node(source,handle).kind!=NodeKind::Object)return failure("Expected preset display object",path);
    QSet<QString> names;
    for(auto entry=node(source,handle).first_child;entry.source;entry=node(source,entry).next_sibling){
        const auto name=text(node(source,entry).key);
        if(names.contains(name))return failure("Ambiguous display entry",path);
        names.insert(name);
    }
    return {};
}
NumericDocumentResult check_graph_view_visibility(const Source& source,Handle handle,const QString& path) {
    for(auto entry=node(source,handle).first_child;entry.source;entry=node(source,entry).next_sibling){
        if(!registered_display_member("graph_views",node(source,entry).key))continue;
        const auto result=object_keys(source,entry,{"open"},path+"."+text(node(source,entry).key));if(!result)return result;
        const auto open=member(source,entry,"open");
        if(open.source && node(source,open).kind!=NodeKind::Boolean)return failure("Expected graph visibility boolean",path+".open");
    }
    return {};
}
NumericDocumentResult check_workspace_layout_version(const Source& source,Handle handle,const QString& path) {
    const auto layout=member(source,handle,"layout_version");
    if(!layout.source)return {};
    std::uint64_t value=0;
    const auto diagnostic=read_uint64(source,layout,value);
    if(!diagnostic || value>std::numeric_limits<int>::max())return failure("Invalid workspace layout version",path+".layout_version");
    return {};
}
NumericDocumentResult check_workspace_version(const Source& source,Handle handle,const QString& path) {
    auto result=object_keys(source,handle,{"version","layout_version","backend","geometry","ads_state","visibility"},path);if(!result)return result;
    if(member(source,handle,"version").source){
        std::uint32_t value=0;result=edition(source,handle,"version",value,path);if(!result)return result;
        if(value!=1)return failure("Unsupported workspace version",path);
    }
    return check_workspace_layout_version(source,handle,path);
}
bool known_workspace_visibility(std::string_view key) {
    if(key=="dataFlow" || registered_display_member("graph_views",key))return true;
    return key.starts_with("nodeView_") && registered_display_member("graph_views",key.substr(9));
}
NumericDocumentResult check_visibility_entry(const Source& source,Handle entry,const QString& path,QSet<QString>& seen) {
    const auto value=node(source,entry);
    if(!known_workspace_visibility(value.key))return {};
    const auto name=text(value.key);
    if(seen.contains(name) || value.kind!=NodeKind::Boolean)return failure("Invalid workspace visibility boolean",path+".visibility."+name);
    seen.insert(name);
    return {};
}
NumericDocumentResult check_workspace_visibility(const Source& source,Handle handle,const QString& path) {
    const auto visibility=member(source,handle,"visibility");if(!visibility.source)return {};
    if(node(source,visibility).kind!=NodeKind::Object)return failure("Expected workspace visibility object",path+".visibility");
    QSet<QString> seen;
    for(auto entry=node(source,visibility).first_child;entry.source;entry=node(source,entry).next_sibling){
        const auto result=check_visibility_entry(source,entry,path,seen);
        if(!result)return result;
    }
    return {};
}
NumericDocumentResult check_workspace(const Source& source,Handle handle,const QString& path) {
    const auto result=check_workspace_version(source,handle,path);if(!result)return result;
    for(auto key:{"backend","geometry","ads_state"}){
        const auto value=member(source,handle,key);
        if(value.source && node(source,value).kind!=NodeKind::String)return failure("Expected workspace string",path+"."+key);
    }
    return check_workspace_visibility(source,handle,path);
}
NumericDocumentResult check_preset_display(const Source& source,Handle handle,const QString& path,const char* key) {
    const auto field=member(source,handle,key);
    if(!field.source)return {};
    auto result=check_display_dictionary(source,field,path+"."+key);
    if(!result)return result;
    if(std::string_view(key)=="graph_views")return check_graph_view_visibility(source,field,path+".graph_views");
    if(std::string_view(key)=="workspace")return check_workspace(source,field,path+".workspace");
    return {};
}
NumericDocumentResult check_preset_displays(const Source& source,Handle handle,const QString& path) {
    for(auto key:{"graph_views","graph_rendering","workspace"}){
        const auto result=check_preset_display(source,handle,path,key);
        if(!result)return result;
    }
    return {};
}
NumericDocumentResult check_preset_presentation(const Source& source,Handle handle,QString path) {
    auto result=object_keys(source,handle,{"version","graph_views","graph_rendering","workspace","show_raw_samples","show_amplitudes","show_test_amplitudes","show_spectrogram","show_window_signal","show_windowed_signal","show_window_function","show_data_flow"},path);if(!result)return result;
    std::uint32_t version=0;result=edition(source,handle,"version",version,path);if(!result)return result;
    if(version!=1)return failure("Unsupported preset presentation version",path);
    for(auto key:{"show_raw_samples","show_amplitudes","show_test_amplitudes","show_spectrogram","show_window_signal","show_windowed_signal","show_window_function","show_data_flow"}){
        const auto field=member(source,handle,key);
        if(field.source && node(source,field).kind!=NodeKind::Boolean)return failure("Expected display visibility boolean",path+"."+key);
    }
    return check_preset_displays(source,handle,path);
}
NumericDocumentResult check_preset_identity(const Source& source,Handle preset,const QString& path,QSet<QString>& ids) {
    std::uint32_t version=0;
    const auto result=edition(source,preset,"version",version,path);
    if(!result || version!=1)return failure("Unsupported preset version",path);
    const auto id=member(source,preset,"id");
    const auto value=node(source,id);
    if(!id.source || value.kind!=NodeKind::String || value.decoded.empty() || ids.contains(text(value.decoded)))
        return failure("Invalid/ambiguous preset ID",path);
    ids.insert(text(value.decoded));
    return {};
}
NumericDocumentResult check_preset(const Source& source,Handle preset,const QString& path,std::uint32_t schema,QSet<QString>& ids) {
    auto result=object_keys(source,preset,{"version","id","name","graph","view","presentation"},path);
    if(result)result=check_preset_identity(source,preset,path,ids);
    if(!result)return result;
    const auto graph=member(source,preset,"graph");
    if(graph.source)result=check_graph(source,graph,path+".graph",schema,true);
    if(!result)return result;
    const auto presentation=member(source,preset,"presentation");
    return presentation.source?check_preset_presentation(source,presentation,path+".presentation"):NumericDocumentResult{};
}
NumericDocumentResult check_presets(const Source& source,Handle handle,QString path,std::uint32_t schema) {
    if(node(source,handle).kind!=NodeKind::Array)return failure("Expected preset array",path);
    QSet<QString> ids;
    for(auto preset=node(source,handle).first_child;preset.source;preset=node(source,preset).next_sibling) {
        const auto result=check_preset(source,preset,path,schema,ids);
        if(!result)return result;
    }
    return {};
}
class ProjectSchemaValidator {
    const Source& source;
    std::uint32_t& schema;
    NumericDocumentResult optional_presets(Handle owner,std::string_view key,const QString& path) const {
        const auto presets=member(source,owner,key);
        return presets.source?check_presets(source,presets,path,schema):NumericDocumentResult{};
    }
    NumericDocumentResult owner_graphs(Handle owner,const QString& path) const {
        auto result=object_keys(source,owner,{"pipeline_graph","scene_graph","presets"},path);
        if(!result)return result;
        for(auto key:{"pipeline_graph","scene_graph"}) {
            const auto graph=member(source,owner,key);
            if(graph.source)result=check_graph(source,graph,path+"."+key,schema);
            if(!result)return result;
        }
        return optional_presets(owner,"presets",path+".presets");
    }
    NumericDocumentResult reject_standalone_graph() const {
        Handle nodes;
        const auto found=find_unique_member(source,source.root(),"nodes",nodes);
        if(found.code==Error::AmbiguousKey)return source_error(found,"nodes");
        return nodes.source?failure("Standalone graph is not a numeric project","nodes"):NumericDocumentResult{};
    }
    NumericDocumentResult user_interface() const {
        const auto ui=member(source,source.root(),"ui");
        if(!ui.source)return {};
        const auto result=object_keys(source,ui,{"pipeline_presets"},"ui");
        return result?optional_presets(ui,"pipeline_presets","ui.pipeline_presets"):result;
    }
    NumericDocumentResult devices() const {
        const auto handle=member(source,source.root(),"devices");
        if(!handle.source)return {};
        const auto value=node(source,handle);
        if(value.kind!=NodeKind::Object && value.kind!=NodeKind::Array)return failure("Expected devices","devices");
        for(auto device=value.first_child;device.source;device=node(source,device).next_sibling) {
            const auto result=owner_graphs(device,"devices");
            if(!result)return result;
        }
        return {};
    }
    NumericDocumentResult header() const {
        auto result=object_keys(source,source.root(),{"schema_version","pipeline_graph","scene_graph","ui","presets","devices"},"project");
        if(!result)return result;
        schema=0;
        result=edition(source,source.root(),"schema_version",schema,"project");
        if(!result)return result;
        return schema==4?NumericDocumentResult{}:unsupported_document_version(schema);
    }
public:
    ProjectSchemaValidator(const Source& input,std::uint32_t& version):source(input),schema(version){}
    NumericDocumentResult validate() const {
        auto result=header();
        if(result)result=owner_graphs(source.root(),"project");
        if(result)result=reject_standalone_graph();
        if(result)result=user_interface();
        return result?devices():result;
    }
};
NumericDocumentResult check_project(const Source& source,std::uint32_t& schema) {
    const DocumentPhaseTrace trace("validate_project");
    return ProjectSchemaValidator(source,schema).validate();
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
QString scalar_identity(const Source& source,Handle handle,std::string_view key) {
    const auto value=node(source,member(source,handle,key));
    return value.kind==NodeKind::String?text(value.decoded):text(value.raw);
}
QString node_identity(const Source& source,Handle handle) {
    auto kind=scalar_identity(source,handle,"kind");
    if(kind.isEmpty()) {
        const auto internal=member(source,handle,"internal-data");
        kind=text(node(source,member(source,internal,"model-name")).decoded);
    }
    return scalar_identity(source,handle,"id")+"/"+kind;
}
QString connection_identity(const Source& source,Handle handle) {
    const auto scalar=[&](std::string_view key){return scalar_identity(source,handle,key);};
    if(member(source,handle,"outNodeId").source)
        return scalar("outNodeId")+":"+scalar("outPortIndex")+":"+scalar("inNodeId")+":"+scalar("inPortIndex");
    return scalar("out_id")+":"+scalar("out_index")+":"+scalar("in_id")+":"+scalar("in_index");
}
struct IdentityField {std::string_view array,key,prefix;};
constexpr IdentityField identity_fields[]{
    {"values","key",{}},{"weights","port",{}},{"inputs","id",{}},{"outputs","id",{}},
    {"settings_ids","id",{}},{"pipeline_presets","id","id:"},{"presets","id","id:"}};
QString identity(const Source& source,Handle handle,std::string_view array) {
    if(array=="nodes")return node_identity(source,handle);
    if(array=="connections")return connection_identity(source,handle);
    if(array=="oscillators") {
        const auto id=member(source,handle,"id");
        return text(node(source,member(source,id,"decimal")).decoded);
    }
    for(const auto& field:identity_fields)
        if(field.array==array)return text(field.prefix)+scalar_identity(source,handle,field.key);
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
bool identity_integer(const QJsonValue& value,qint64 maximum) {
    return value.isDouble() && value.toInteger(-1)>=0 && value.toInteger(-1)<=maximum
        && value.toDouble()==static_cast<double>(value.toInteger(-1));
}
bool registered_node_view(std::string_view key) {
    // Node-view identity is the existing hex-encoded [device,graph,node,instance?].
    // Other dictionary keys remain opaque, including future extension records.
    if(key.size()>1024)return false;const auto encoded=bytes(key),decoded=QByteArray::fromHex(encoded);if(decoded.toHex()!=encoded)return false;
    const auto identity=QJsonDocument::fromJson(decoded).array();
    return (identity.size()==3 || identity.size()==4) && identity[0].isString() && identity[1].isString()
        && identity_integer(identity[2],std::numeric_limits<NodeId>::max())
        && (identity.size()==3 || identity_integer(identity[3],std::numeric_limits<qint64>::max()));
}
constexpr std::string_view adapter_display_members[]{"provider_uuid","stream_id","host","port","device_identity","remote_endpoint"};
constexpr std::string_view endpoint_display_members[]{"graph_id","node_id","direction","port_index"};
constexpr std::string_view presentation_display_members[]{"version","graph_views","graph_rendering","workspace","show_raw_samples","show_amplitudes","show_test_amplitudes","show_spectrogram","show_window_signal","show_windowed_signal","show_window_function","show_data_flow"};
constexpr std::string_view rendering_display_members[]{"version","defaults","color_presets","gui_fps_mode","gui_target_fps"};
constexpr std::string_view workspace_display_members[]{"version","layout_version","backend","geometry","ads_state","visibility"};
constexpr std::string_view graph_display_members[]{"rawSamples","fft","mel","spectrogram","windowSignal","windowedSignal","windowFunction"};
struct DisplaySchema {std::string_view context;std::span<const std::string_view> members;};
constexpr DisplaySchema display_schemas[]{
    {"adapter",adapter_display_members},{"remote_endpoint",endpoint_display_members},
    {"presentation",presentation_display_members},{"graph_rendering",rendering_display_members},
    {"workspace",workspace_display_members},{"graph_views",graph_display_members}};
bool registered_display_member(std::string_view context,std::string_view key) {
    const auto schema=std::find_if(std::begin(display_schemas),std::end(display_schemas),
        [=](const auto& value){return value.context==context;});
    if(schema==std::end(display_schemas))return false;
    if(std::find(schema->members.begin(),schema->members.end(),key)!=schema->members.end())return true;
    return context=="graph_views" && registered_node_view(key);
}
void append_json_item(QByteArray& output,bool& first,const QByteArray& item) {
    if(!first)output+=',';
    first=false;
    output+=item;
}
bool same_settings_projection(const Source& old,Handle old_handle,const Source& fresh,Handle new_handle) {
    bool complete=true,replacement_complete=true;
    const auto original=settings_projection(old,old_handle,complete,true);
    const auto replacement=settings_projection(fresh,new_handle,replacement_complete,true);
    return complete && replacement_complete && original==replacement;
}
std::string_view merge_display_scope(std::string_view context,std::string_view scope,std::string_view key) {
    if((context=="pipeline_presets" || context=="presets") && key=="presentation")return "presentation";
    if((context=="nodes" || context=="internal-data") && key=="adapter")return "adapter";
    if(scope=="presentation" && (key=="graph_views" || key=="graph_rendering" || key=="workspace"))return key;
    if(scope=="adapter" && key=="remote_endpoint")return key;
    return {};
}
bool retired_literal_payload(std::string_view context,std::string_view key) {
    constexpr std::array<std::string_view,10> contexts{"numerator","nan_replacement","negative_inf_replacement",
        "positive_inf_replacement","value","capacity","minimum","maximum","denominator","alignment"};
    return (key=="bits" || key=="decimal") && std::find(contexts.begin(),contexts.end(),context)!=contexts.end();
}
QByteArray opaque_remote_endpoint(const Source& source,Handle endpoint) {
    QByteArray output="{";
    bool first=true;
    for(auto child=node(source,endpoint).first_child;child.source;child=node(source,child).next_sibling) {
        const auto value=node(source,child);
        if(registered_display_member("remote_endpoint",value.key))continue;
        append_json_item(output,first,member_prefix(source,endpoint,child)+bytes(value.raw));
    }
    return first?QByteArray{}:output+'}';
}
QByteArray merge(const Source&,Handle,const Source&,Handle,std::string_view,NumericDocumentResult&,
    std::string kind={},bool preserve_equal=false,std::string_view display_scope={});
struct MergeScope {
    const Source& original;
    const Source& replacement;
    NumericDocumentResult& result;
    std::string_view context;
    std::string kind;
    bool preserve_equal;
    std::string_view display_scope;
    QByteArray child(Handle before,Handle after,std::string_view key,std::string_view display) const {
        return merge(original,before,replacement,after,key,result,kind,preserve_equal,display);
    }
};
class ObjectMerge {
    MergeScope scope;
    Handle before,after;
    QByteArray output="{";
    bool first=true;
    QSet<QString> processed;
    void append_replacement(Handle child,Handle replacement) {
        const auto value=node(scope.original,child);
        Handle unique;
        const auto found=find_unique_member(scope.original,before,value.key,unique);
        if(!found){scope.result=source_error(found);return;}
        processed.insert(text(value.key));
        const auto display=merge_display_scope(scope.context,scope.display_scope,value.key);
        append_json_item(output,first,member_prefix(scope.original,before,child)+scope.child(child,replacement,value.key,display));
    }
    void append_removed(Handle child) {
        const auto value=node(scope.original,child);
        if(scope.context=="scalar_inputs" || scope.context=="port_labels")return;
        if(scope.display_scope=="adapter" && value.key=="remote_endpoint") {
            const auto opaque=opaque_remote_endpoint(scope.original,child);
            if(!opaque.isEmpty())append_json_item(output,first,member_prefix(scope.original,before,child)+opaque);
            return;
        }
        if(!scope.display_scope.empty() && registered_display_member(scope.display_scope,value.key))return;
        if(retired_literal_payload(scope.context,value.key))return;
        append_json_item(output,first,member_prefix(scope.original,before,child)+bytes(value.raw));
    }
    void append_original(Handle child) {
        Handle replacement;
        const auto found=find_unique_member(scope.replacement,after,node(scope.original,child).key,replacement);
        if(found.code==Error::AmbiguousKey){scope.result=source_error(found);return;}
        if(replacement.source)append_replacement(child,replacement);
        else append_removed(child);
    }
    void append_new(Handle child) {
        const auto value=node(scope.replacement,child);
        if(processed.contains(text(value.key)))return;
        auto quoted=QJsonDocument(QJsonArray{text(value.key)}).toJson(QJsonDocument::Compact);
        append_json_item(output,first,quoted.mid(1,quoted.size()-2)+":"+bytes(value.raw));
    }
  public:
    ObjectMerge(MergeScope value,Handle original,Handle replacement):scope(std::move(value)),before(original),after(replacement) {}
    QByteArray run() {
        if(scope.context=="nodes")scope.kind.assign(node(scope.replacement,member(scope.replacement,after,"kind")).decoded);
        for(auto child=node(scope.original,before).first_child;child.source && scope.result;child=node(scope.original,child).next_sibling)
            append_original(child);
        for(auto child=node(scope.replacement,after).first_child;child.source && scope.result;child=node(scope.replacement,child).next_sibling)
            append_new(child);
        return output+'}';
    }
};
bool identified_array(std::string_view context) {
    constexpr std::array<std::string_view,10> contexts{"nodes","values","inputs","outputs","connections",
        "pipeline_presets","presets","settings_ids","oscillators","weights"};
    return std::find(contexts.begin(),contexts.end(),context)!=contexts.end();
}
class ArrayMerge {
    MergeScope scope;
    Handle before,after,old_child;
    bool identified,first=true;
    QByteArray output="[";
    QSet<QString> retained;
    Handle match_identity(const QString& id) {
        if(retained.contains(id)){scope.result=failure("Duplicate replacement identity",text(scope.context));return {};}
        Handle match;
        for(auto child=node(scope.original,before).first_child;child.source;child=node(scope.original,child).next_sibling) {
            if(identity(scope.original,child,scope.context)!=id)continue;
            if(match.source){scope.result=failure("Ambiguous source identity",text(scope.context));return {};}
            match=child;
        }
        retained.insert(id);
        return match;
    }
    Handle positional_match() {
        const auto match=old_child;
        if(old_child.source)old_child=node(scope.original,old_child).next_sibling;
        return match;
    }
    void append_replacement(Handle child) {
        const auto id=identity(scope.replacement,child,scope.context);
        const auto match=identified?match_identity(id):positional_match();
        if(!scope.result)return;
        const auto value=match.source?scope.child(match,child,scope.context,scope.display_scope):bytes(node(scope.replacement,child).raw);
        append_json_item(output,first,value);
    }
    void append_opaque_parameters() {
        for(auto child=node(scope.original,before).first_child;child.source && scope.result;child=node(scope.original,child).next_sibling) {
            const auto key=node(scope.original,member(scope.original,child,"key"));
            if(retained.contains(text(key.decoded)) || registered(scope.kind,key.decoded))continue;
            append_json_item(output,first,bytes(node(scope.original,child).raw));
        }
    }
  public:
    ArrayMerge(MergeScope value,Handle original,Handle replacement):scope(std::move(value)),before(original),after(replacement),
        old_child(node(scope.original,before).first_child),identified(identified_array(scope.context)) {}
    QByteArray run() {
        for(auto child=node(scope.replacement,after).first_child;child.source && scope.result;child=node(scope.replacement,child).next_sibling)
            append_replacement(child);
        if(scope.context=="values")append_opaque_parameters();
        return output+']';
    }
};
QByteArray merge_embedded_node(std::string_view before,std::string_view after,NumericDocumentResult& result) {
    Source original,replacement;
    auto parsed=parse_project_source(before,original);
    if(!parsed){result=source_error(parsed,"preset.numeric_processing");return {};}
    parsed=parse_project_source(after,replacement);
    if(!parsed){result=source_error(parsed,"preset.numeric_processing");return {};}
    const auto retained=merge(original,original.root(),replacement,replacement.root(),"node",result,{},true);
    if(!result)return {};
    const auto quoted=QJsonDocument(QJsonArray{QString::fromUtf8(retained)}).toJson(QJsonDocument::Compact);
    return quoted.mid(1,quoted.size()-2);
}
std::optional<QByteArray> merge_numeric_array_payload(const Source& old,Handle old_h,const Source& fresh,Handle new_h,
                                                    std::string_view context) {
    const auto a=node(old,old_h),b=node(fresh,new_h);
    if(context=="value" && a.kind==NodeKind::String && a.decoded.starts_with("f32le-base64:")
        && b.kind==NodeKind::Object) {
        const auto type=node(fresh,member(fresh,new_h,"type"));
        const auto payload=node(fresh,member(fresh,new_h,"values"));
        if(type.decoded=="numeric_array" && payload.kind==NodeKind::String) {
            const auto quoted=QJsonDocument(QJsonArray{text(payload.decoded)}).toJson(QJsonDocument::Compact);
            return quoted.mid(1,quoted.size()-2);
        }
    }
    return {};
}
std::optional<QByteArray> retained_merge_value(const Source& old,Handle old_h,const Source& fresh,Handle new_h,
        std::string_view context,bool preserve_equal) {
    if(preserve_equal && same_settings_projection(old,old_h,fresh,new_h))return bytes(node(old,old_h).raw);
    return merge_numeric_array_payload(old,old_h,fresh,new_h,context);
}
QByteArray merge(const Source& old,Handle old_h,const Source& fresh,Handle new_h,
                 std::string_view context,NumericDocumentResult& result,std::string kind,bool preserve_equal,std::string_view display_scope) {
    const auto a=node(old,old_h),b=node(fresh,new_h);
    if(const auto retained=retained_merge_value(old,old_h,fresh,new_h,context,preserve_equal))return *retained;
    if(a.kind!=b.kind)return bytes(b.raw);
    if(preserve_equal && context=="numeric_processing" && b.kind==NodeKind::String)
        return merge_embedded_node(a.decoded,b.decoded,result);
    if(b.kind==NodeKind::Object)return ObjectMerge({old,fresh,result,context,kind,preserve_equal,display_scope},old_h,new_h).run();
    if(b.kind==NodeKind::Array)return ArrayMerge({old,fresh,result,context,kind,preserve_equal,display_scope},old_h,new_h).run();
    return a.raw==b.raw?bytes(a.raw):bytes(b.raw);
}
} // namespace
QString numeric_project_path(const QString& explicit_path) {
    const auto path=explicit_path.isEmpty()?QDir(QCoreApplication::applicationDirPath()).filePath("config.json"):explicit_path;
    return QFileInfo(QDir::cleanPath(QDir::fromNativeSeparators(path))).absoluteFilePath();
}
NumericDocumentResult encode_numeric_node(const ConfiguredProcessingNode& v,QByteArray& out) {return write_snapshot(v,out);}
NumericDocumentResult decode_numeric_node(QByteArrayView bytes,ConfiguredProcessingNode& out) {return read_snapshot(bytes,out);}
NumericDocumentResult encode_numeric_graph(const NumericProcessingGraph& v,QByteArray& out) {return write_snapshot(v,out);}
NumericDocumentResult decode_numeric_graph(QByteArrayView bytes,NumericProcessingGraph& out) {return read_snapshot(bytes,out);}
NumericDocumentResult decode_numeric_editor_graph(QByteArrayView input,NumericProcessingGraph& out) {
    Source source;auto diagnostic=parse_project_source(view(input),source);if(!diagnostic)return source_error(diagnostic);
    const auto graph=source.root();auto result=check_graph(source,graph,"snapshot",4,true);if(!result)return result;
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
class GraphReplacement {
    const NumericProjectDocument& source;
    const NumericProcessingGraph& graph;
    std::string_view section;
    NumericProjectDocument candidate;
    Source fresh;
    QByteArray snapshot,replacement,insertion;
    Handle root,old;
    NumericDocumentResult prepare() {
        // The document edition belongs to the source owner; graph admission
        // validates only graph and processing semantics.
        auto result=encode_numeric_graph(graph,snapshot);
        std::uint32_t schema=0;
        if(result)result=check_project(source.source,schema);
        if(!result)return result;
        const auto diagnostic=parse_project_source(view(snapshot),fresh);
        return diagnostic?NumericDocumentResult{}:source_error(diagnostic);
    }
    NumericDocumentResult replacement_bytes() {
        root=source.source.root();
        old=member(source.source,root,section);
        replacement=snapshot;
        if(!old.source)return {};
        const auto id=member(source.source,old,"graph_id");
        if(id.source && node(source.source,id).decoded!=graph.graph_id)
            return failure("Graph identity changed; explicit new project required",text(section)+".graph_id");
        NumericDocumentResult result;
        replacement=merge(source.source,old,fresh,fresh.root(),"graph",result);
        return result;
    }
    DocumentEdit edit() {
        if(old.source)return {EditKind::Replace,old,{}, {},node(source.source,old).raw,view(replacement)};
        auto quoted=QJsonDocument(QJsonArray{text(section)}).toJson(QJsonDocument::Compact);
        quoted=quoted.mid(1,quoted.size()-2);
        insertion=quoted+":"+replacement;
        return {EditKind::Insert,{},root,{},node(source.source,root).raw,view(insertion)};
    }
    NumericDocumentResult publish(NumericProjectDocument& out) {
        const auto change=edit();
        const auto diagnostic=stage_project_edits(source.source,std::span(&change,1),candidate.source);
        if(!diagnostic)return source_error(diagnostic,text(section));
        std::uint32_t schema=0;
        const auto result=check_project(candidate.source,schema);
        if(!result)return result;
        candidate.active_path=source.active_path;
        candidate.disk_baseline=source.disk_baseline;
        out=std::move(candidate);
        return {};
    }
public:
    GraphReplacement(const NumericProjectDocument& document,const NumericProcessingGraph& value,std::string_view key)
        :source(document),graph(value),section(key){}
    NumericDocumentResult stage(NumericProjectDocument& out) {
        auto result=prepare();
        if(result)result=replacement_bytes();
        return result?publish(out):result;
    }
};
NumericDocumentResult stage_project_graph(const NumericProjectDocument& source,const NumericProcessingGraph& graph,std::string_view section,NumericProjectDocument& out) {
    return GraphReplacement(source,graph,section).stage(out);
}
}
NumericDocumentResult stage_numeric_draft(const NumericProjectDocument& source,const NumericProcessingGraph& graph,NumericProjectDocument& out) {
    const DocumentPhaseTrace trace("stage_draft");
    return stage_project_graph(source,graph,"scene_graph",out);
}
NumericDocumentResult create_numeric_project(const NumericProcessingGraph& graph,const NumericDraftPresentation& presentation,NumericProjectDocument& out) {
    NumericProjectDocument candidate;auto result=read_numeric_project("{\"schema_version\":4}",candidate);if(!result)return result;
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
        QJsonObject{{"version",1},{"id","builtin-test-signal"},{"name","Signal generator"},
            {"graph",QJsonDocument::fromJson(snapshot).object()},{"presentation",display}}
    }}}}};
    return create_numeric_project(graph,presentation,out);
}
namespace {
NumericDocumentResult read_presentation_number(const Source& source,Handle object,const char* key,QJsonObject& out) {
    Handle handle;
    const auto found=find_unique_member(source,object,key,handle);
    if(found.code==Error::AmbiguousKey)return source_error(found);
    if(!handle.source)return {};
    NumericDocumentResult result;
    const auto value=snapshot_leaf(source,handle,result);
    if(!value.isDouble() || !std::isfinite(value.toDouble()))return failure("Invalid presentation number",key);
    out[key]=value;
    return result;
}
NumericDocumentResult read_presentation_fields(const Source& source,Handle object,std::initializer_list<const char*> keys,QJsonObject& out) {
    if(node(source,object).kind!=NodeKind::Object)return failure("Malformed presentation object");
    for(auto key:keys) {
        const auto result=read_presentation_number(source,object,key,out);
        if(!result)return result;
    }
    return {};
}
class DraftPresentationReader {
    const Source& source;
    NumericDraftPresentation candidate;
    NumericDocumentResult position(Handle owner,const QString& key) {
        const auto handle=member(source,owner,"position");
        if(!handle.source)return {};
        QJsonObject values;
        const auto result=read_presentation_fields(source,handle,{"x","y"},values);
        if(result)candidate.positions[key]=values;
        return result;
    }
    NumericDocumentResult presentation(Handle owner,const QString& key) {
        const auto handle=member(source,owner,"presentation");
        if(!handle.source)return {};
        QJsonObject known;
        const auto result=check_node_presentation(source,handle,&known);
        if(result)candidate.node_presentations[key]=known;
        return result;
    }
    NumericDocumentResult adapter(Handle owner,const QString& key) {
        const auto handle=member(source,owner,"adapter");
        if(!handle.source)return {};
        QJsonObject known;
        const auto result=decode_numeric_node_adapter(bytes(node(source,handle).raw),known);
        if(result)candidate.node_adapters[key]=known;
        return result;
    }
    NumericDocumentResult read_node(Handle handle) {
        std::int64_t id=0;
        const auto diagnostic=read_int64(source,member(source,handle,"id"),id);
        if(!diagnostic)return source_error(diagnostic);
        const auto key=QString::number(id);
        auto result=position(handle,key);
        if(result)result=presentation(handle,key);
        return result?adapter(handle,key):result;
    }
    NumericDocumentResult nodes() {
        const auto graph=member(source,source.root(),"scene_graph");
        const auto array=member(source,graph,"nodes");
        for(auto handle=node(source,array).first_child;handle.source;handle=node(source,handle).next_sibling) {
            const auto result=read_node(handle);
            if(!result)return result;
        }
        return {};
    }
    NumericDocumentResult editor() {
        const auto ui=member(source,source.root(),"ui");
        const auto handle=member(source,ui,"editor_view");
        if(!handle.source)return {};
        return read_presentation_fields(source,handle,{"version","scale","center_x","center_y","node_width","port_spacing","port_radius","port_font"},candidate.editor_view);
    }
public:
    explicit DraftPresentationReader(const Source& input):source(input){}
    NumericDocumentResult read(NumericDraftPresentation& out) {
        std::uint32_t schema=0;
        auto result=check_project(source,schema);
        if(result)result=nodes();
        if(result)result=editor();
        if(result)out=std::move(candidate);
        return result;
    }
};
}
NumericDocumentResult read_numeric_presentation(const NumericProjectDocument& source,NumericDraftPresentation& out) {
    return DraftPresentationReader(source.source).read(out);
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
void settings_object_member(const Source& source,Handle child,bool preset_context,QSet<QString>& keys,QJsonObject& object,bool& complete) {
    const auto key=text(node(source,child).key);
    if(keys.contains(key)){object.remove(key);complete=false;return;}
    keys.insert(key);
    bool valid=true;
    const auto value=settings_projection(source,child,valid,preset_context);
    if(!value.isUndefined())object[key]=value;
    if(!valid)complete=false;
}
QJsonValue settings_object(const Source& source,Handle handle,bool& complete,bool preset_context) {
    QJsonObject object;
    QSet<QString> keys;
    for(auto child=node(source,handle).first_child;child.source;child=node(source,child).next_sibling)
        settings_object_member(source,child,preset_context,keys,object,complete);
    return object;
}
bool partial_settings_array(std::string_view key,bool preset_context) {
    if(key=="pipeline_presets")return true;
    if(!preset_context)return false;
    constexpr std::string_view partial[]{"nodes","connections","values","inputs","outputs"};
    return std::find(std::begin(partial),std::end(partial),key)!=std::end(partial);
}
QJsonValue settings_array(const Source& source,Handle handle,bool& complete,bool preset_context) {
    const auto input=node(source,handle);
    const bool partial=partial_settings_array(input.key,preset_context);
    QJsonArray array;
    for(auto child=input.first_child;child.source;child=node(source,child).next_sibling) {
        bool valid=true;
        const auto value=settings_projection(source,child,valid,preset_context || input.key=="pipeline_presets");
        if(!valid) {
            complete=false;
            if(!partial || value.isUndefined())return QJsonValue(QJsonValue::Undefined);
        }
        array.append(value);
    }
    return array;
}
bool exact_settings_integer(const Source& source,Handle handle) {
    const auto value=node(source,handle);
    if(value.kind!=NodeKind::Number || value.raw.find_first_of(".eE")!=std::string_view::npos)return true;
    std::int64_t integer=0;
    const auto diagnostic=read_int64(source,handle,integer);
    return diagnostic && integer<=9007199254740991LL && integer>=-9007199254740991LL;
}
QJsonValue settings_scalar(const Source& source,Handle handle,bool& complete) {
    if(!exact_settings_integer(source,handle)){complete=false;return QJsonValue(QJsonValue::Undefined);}
    NumericDocumentResult result;
    const auto value=snapshot_leaf(source,handle,result);
    if(!result || (value.isDouble() && !std::isfinite(value.toDouble()))) {
        complete=false;
        return QJsonValue(QJsonValue::Undefined);
    }
    return value;
}
QJsonValue settings_projection(const Source& source,Handle handle,bool& complete,bool preset_context) {
    const auto value=node(source,handle);
    if(value.kind==NodeKind::Object)return settings_object(source,handle,complete,preset_context);
    if(value.kind==NodeKind::Array)return settings_array(source,handle,complete,preset_context);
    return settings_scalar(source,handle,complete);
}
class VisualGraphReader {
    const Source& source;
    Handle graph;
    bool materialize;
    NumericProcessingGraph candidate;
    QSet<qint64> node_ids;
    QSet<QString> edge_ids;
    NumericDocumentResult header() {
        const auto id=node(source,member(source,graph,"graph_id"));
        if(id.kind!=NodeKind::String || id.decoded.empty() || id.decoded.size()>1024)return failure("Invalid preset graph identity");
        candidate.graph_id.assign(id.decoded);
        const auto interval=member(source,graph,"execution_interval");if(!interval.source)return {};
        NumericDocumentResult result;Mapper mapper{&source,graph,{},&result,"preset.execution_interval",{}};
        decode(mapper,interval,candidate.execution_interval);if(!result)return result;
        return domain::valid_execution_interval(candidate.execution_interval)?NumericDocumentResult{}:failure("Invalid execution interval");
    }
    NumericDocumentResult node_extensions(Handle internal) const {
        auto result=object_keys(source,internal,{"model-name","numeric_processing","presentation","adapter"},"preset.node.internal-data");if(!result)return result;
        const auto presentation=member(source,internal,"presentation");if(presentation.source)result=check_node_presentation(source,presentation);if(!result)return result;
        const auto adapter=member(source,internal,"adapter");if(adapter.source)result=check_node_adapter(source,adapter);return result;
    }
    NumericDocumentResult typed_node(Handle internal,std::int64_t id,ConfiguredProcessingNode& decoded) const {
        auto result=node_extensions(internal);if(!result)return result;
        const auto typed=member(source,internal,"numeric_processing");
        if(!typed.source)return failure("Preset typed node snapshot missing");
        if(node(source,typed).kind!=NodeKind::String)return failure("Invalid preset typed node snapshot");
        result=decode_numeric_node(bytes(node(source,typed).decoded),decoded);if(!result)return result;
        if(decoded.id!=id || node(source,member(source,internal,"model-name")).decoded!=decoded.kind)return failure("Preset typed/visual node identity mismatch");
        return {};
    }
    NumericDocumentResult read_node(Handle handle) {
        auto result=object_keys(source,handle,{"id","internal-data","position"},"preset.node");if(!result)return result;
        std::int64_t id=-1;const auto diagnostic=read_int64(source,member(source,handle,"id"),id);
        if(!diagnostic || id<0 || id>std::numeric_limits<domain::NodeId>::max() || node_ids.contains(id))return failure("Invalid/ambiguous preset node ID");
        node_ids.insert(id);ConfiguredProcessingNode decoded;
        result=typed_node(member(source,handle,"internal-data"),id,decoded);
        if(result && materialize)candidate.nodes.push_back(std::move(decoded));return result;
    }
    NumericDocumentResult nodes(Handle array) {
        std::size_t count=0;
        for(auto entry=node(source,array).first_child;entry.source;entry=node(source,entry).next_sibling){
            if(++count>4096)return failure("Preset node limit");
            const auto result=read_node(entry);if(!result)return result;
        }
        return {};
    }
    NumericDocumentResult edge_option(Handle handle,const char* key,std::uint64_t maximum,const char* message) const {
        const auto option=member(source,handle,key);
        if(!option.source)return {};
        std::uint64_t value=0;
        return read_uint64(source,option,value) && value<=maximum?NumericDocumentResult{}:failure(message);
    }
    NumericDocumentResult edge_options(Handle handle) const {
        const auto result=edge_option(handle,"conversion",3,"Invalid edge conversion policy");
        return result?edge_option(handle,"configuration",1,"Invalid configuration connection"):result;
    }
    NumericDocumentResult edge_fields(Handle handle) const {
        const auto result=object_keys(source,handle,{"outNodeId","outPortIndex","inNodeId","inPortIndex","conversion","configuration"},"preset.edge");if(!result)return result;
        for(auto field:{"outNodeId","outPortIndex","inNodeId","inPortIndex"}){
            std::uint64_t number=0;const auto diagnostic=read_uint64(source,member(source,handle,field),number);
            const bool port=std::string_view(field)=="outPortIndex" || std::string_view(field)=="inPortIndex";
            if(!diagnostic || number>(port?65535u:static_cast<std::uint64_t>(std::numeric_limits<domain::NodeId>::max())))return failure("Invalid preset edge identity");
        }
        return edge_options(handle);
    }
    NumericDocumentResult decode_edge(Handle handle,Edge& edge) const {
        std::uint64_t value=0;
        (void)read_uint64(source,member(source,handle,"outNodeId"),value);edge.from=static_cast<NodeId>(value);
        (void)read_uint64(source,member(source,handle,"inNodeId"),value);edge.to=static_cast<NodeId>(value);
        (void)read_uint64(source,member(source,handle,"outPortIndex"),value);edge.output_port=static_cast<std::uint16_t>(value);
        (void)read_uint64(source,member(source,handle,"inPortIndex"),value);edge.input_port=static_cast<std::uint16_t>(value);
        const auto policy=member(source,handle,"conversion");if(!policy.source)return {};
        if(!read_uint64(source,policy,value) || value>3)return failure("Invalid edge conversion policy");
        edge.conversion=std::uint8_t(value);return {};
    }
    NumericDocumentResult append_configuration(Handle configuration,const Edge& edge,bool endpoints) {
        std::uint64_t role=0;
        if(!read_uint64(source,configuration,role) || role>1 || !endpoints)return failure("Invalid configuration connection");
        candidate.configuration_bindings.push_back({edge.from,edge.to,ConfigurationRole(role),ConfigurationRole(role)});return {};
    }
    NumericDocumentResult append_edge(Handle handle) {
        Edge edge;const auto result=decode_edge(handle,edge);if(!result)return result;
        const auto from=std::find_if(candidate.nodes.begin(),candidate.nodes.end(),[&](const auto& n){return n.id==edge.from;});
        const auto to=std::find_if(candidate.nodes.begin(),candidate.nodes.end(),[&](const auto& n){return n.id==edge.to;});
        const auto configuration=member(source,handle,"configuration");
        if(configuration.source)return append_configuration(configuration,edge,from!=candidate.nodes.end() && to!=candidate.nodes.end());
        if(from==candidate.nodes.end() || to==candidate.nodes.end() || edge.output_port>=from->outputs.size() || edge.input_port>=to->inputs.size())return failure("Invalid visual port index");
        edge.output_port=from->outputs[edge.output_port].id;edge.input_port=to->inputs[edge.input_port].id;candidate.edges.push_back(edge);return {};
    }
    NumericDocumentResult edge(Handle handle) {
        const auto result=edge_fields(handle);if(!result)return result;
        const auto key=identity(source,handle,"connections");if(edge_ids.contains(key))return failure("Duplicate preset edge identity");
        edge_ids.insert(key);return materialize?append_edge(handle):NumericDocumentResult{};
    }
    NumericDocumentResult edges(Handle array) {
        std::size_t count=0;
        for(auto entry=node(source,array).first_child;entry.source;entry=node(source,entry).next_sibling){
            if(++count>65536)return failure("Preset edge limit");
            const auto result=edge(entry);if(!result)return result;
        }
        return {};
    }
public:
    VisualGraphReader(const Source& input,Handle handle,bool output):source(input),graph(handle),materialize(output){}
    NumericDocumentResult read(NumericProcessingGraph* output) {
        auto result=header();if(!result)return result;
        const auto node_array=member(source,graph,"nodes"),edge_array=member(source,graph,"connections");
        if(node(source,node_array).kind!=NodeKind::Array || node(source,edge_array).kind!=NodeKind::Array)return failure("Preset graph arrays missing");
        result=nodes(node_array);if(result)result=edges(edge_array);
        if(result && output)*output=std::move(candidate);return result;
    }
};
NumericDocumentResult check_visual_graph(const Source& source,Handle graph,NumericProcessingGraph* out) {
    return VisualGraphReader(source,graph,out!=nullptr).read(out);
}
NumericDocumentResult validate_preset_replacement(const Source& source,Handle array) {
    auto result=check_presets(source,array,"ui.pipeline_presets",4);if(!result)return result;
    std::size_t count=0;
    for(auto p=node(source,array).first_child;p.source;p=node(source,p).next_sibling){
        if(++count>4096)return failure("Preset count limit","ui.pipeline_presets");
        auto name=member(source,p,"name");if(name.source&&node(source,name).kind!=NodeKind::String)return failure("Invalid preset name");
    }
    return {};
}
}
namespace {
NumericDocumentResult application_settings_section(const Source& source,const char* key,QJsonObject& out) {
    Handle handle;
    const auto found=find_unique_member(source,source.root(),key,handle);
    if(found.code==Error::AmbiguousKey)return source_error(found,key);
    if(!handle.source)return {};
    if(node(source,handle).kind!=NodeKind::Object)return failure("Malformed application settings",key);
    bool complete=true;
    out[key]=settings_projection(source,handle,complete);
    return {};
}
void project_settings_field(const Source& source,Handle owner,std::string_view key,QJsonObject& out) {
    const auto field=member(source,owner,key);
    if(!field.source)return;
    bool complete=true;
    const auto value=settings_projection(source,field,complete);
    if(!value.isUndefined())out[text(key)]=value;
}
}
NumericDocumentResult read_numeric_application_settings(const NumericProjectDocument& source,QJsonObject& out) {
    std::uint32_t schema=0;
    auto result=check_project(source.source,schema);
    if(!result)return result;
    QJsonObject candidate;
    for(auto key:{"core","ui","wled_ar_server"}) {
        result=application_settings_section(source.source,key,candidate);
        if(!result)return result;
    }
    out=std::move(candidate);
    return {};
}
NumericDocumentResult decode_numeric_preset_presentation(QByteArrayView input,QJsonObject& out) {
    Source source;auto diagnostic=parse_project_source(view(input),source);if(!diagnostic)return source_error(diagnostic);
    auto result=check_preset_presentation(source,source.root(),"preset.presentation");if(!result)return result;
    QJsonObject candidate;
    for(auto key:presentation_display_members)project_settings_field(source,source.root(),key,candidate);
    out=std::move(candidate);return {};
}
namespace {
QJsonObject project_remote_endpoint(const Source& source,Handle handle,NumericDocumentResult& result) {
    QJsonObject endpoint;
    for(auto key:endpoint_display_members) {
        const auto child=member(source,handle,key);
        if(child.source)endpoint[text(key)]=snapshot_leaf(source,child,result);
    }
    return endpoint;
}
void project_adapter_field(const Source& source,std::string_view key,QJsonObject& candidate,NumericDocumentResult& result) {
    const auto field=member(source,source.root(),key);
    if(!field.source)return;
    if(key!="remote_endpoint") {
        candidate[text(key)]=snapshot_leaf(source,field,result);
        return;
    }
    const auto endpoint=project_remote_endpoint(source,field,result);
    if(!endpoint.isEmpty() || !node(source,field).first_child.source)candidate[text(key)]=endpoint;
}
}
NumericDocumentResult decode_numeric_node_adapter(QByteArrayView input,QJsonObject& out) {
    Source source;auto diagnostic=parse_project_source(view(input),source);if(!diagnostic)return source_error(diagnostic);
    auto result=check_node_adapter(source,source.root());if(!result)return result;QJsonObject candidate;
    for(auto key:adapter_display_members)project_adapter_field(source,key,candidate,result);
    if(result)out=std::move(candidate);return result;
}
namespace {
NumericDocumentResult project_network_integer(const Source& source,Handle owner,const char* key,double minimum,double maximum,QJsonObject& out) {
    const auto handle=member(source,owner,key);if(!handle.source)return {};
    NumericDocumentResult result;
    const auto value=snapshot_leaf(source,handle,result);
    if(!result)return result;
    const auto number=value.toDouble();
    if(!value.isDouble() || !std::isfinite(number) || std::floor(number)!=number || number<minimum || number>maximum)
        return failure("Invalid serial integer",QStringLiteral("network.serial.")+key);
    out[key]=value;return {};
}
NumericDocumentResult project_serial_name(const Source& source,Handle serial,QJsonObject& candidate) {
    const auto port=node(source,member(source,serial,"port_name"));
    if(port.kind!=NodeKind::String || text(port.decoded).trimmed().isEmpty())
        return failure("Expected serial port name","network.serial.port_name");
    candidate["port_name"]=text(port.decoded);return {};
}
NumericDocumentResult project_serial_fields(const Source& source,Handle serial,QJsonObject& candidate) {
    auto result=project_serial_name(source,serial,candidate);if(!result)return result;
    result=project_network_integer(source,serial,"baud_rate",1,std::numeric_limits<qint32>::max(),candidate);
    if(result)result=project_network_integer(source,serial,"input_node",0,std::numeric_limits<int>::max()-1,candidate);
    return result;
}
NumericDocumentResult project_network_serial(const Source& source,Handle network,QJsonObject& out) {
    const auto serial=member(source,network,"serial");
    if(!serial.source)return {};
    auto result=object_keys(source,serial,{"port_name","baud_rate","input_node"},"network.serial");
    if(!result)return result;
    QJsonObject candidate;
    result=project_serial_fields(source,serial,candidate);
    if(result)out["serial"]=candidate;
    return result;
}
NumericDocumentResult project_network_route(const Source& source,Handle route,QJsonObject& out) {
    auto result=object_keys(source,route,{"node_id"},"network.routes.node_id");
    if(!result)return result;
    std::int64_t id=0;
    const auto diagnostic=read_int64(source,member(source,route,"node_id"),id);
    if(!diagnostic)return source_error(diagnostic,"network.routes.node_id");
    if(id<0 || id>std::numeric_limits<domain::NodeId>::max())return failure("Invalid route node ID","network.routes.node_id");
    result=decode_numeric_node_adapter(bytes(node(source,route).raw),out);
    if(result)out["node_id"]=int(id);
    return result;
}
NumericDocumentResult project_route_array(const Source& source,const NodeView& array,std::size_t maximum,QJsonArray& candidate) {
    std::size_t count=0;
    for(auto route=array.first_child;route.source;route=node(source,route).next_sibling) {
        if(count++==maximum)return failure("Too many numeric routes","network.routes");
        QJsonObject projected;const auto result=project_network_route(source,route,projected);
        if(!result)return result;
        candidate.append(projected);
    }
    return {};
}
NumericDocumentResult project_network_routes(const Source& source,Handle network,std::size_t maximum,QJsonObject& out) {
    const auto routes=member(source,network,"routes");
    if(!routes.source)return {};
    const auto array=node(source,routes);
    if(array.kind!=NodeKind::Array)return failure("Expected route array","network.routes");
    QJsonArray candidate;const auto result=project_route_array(source,array,maximum,candidate);
    if(!result)return result;
    out["routes"]=candidate;return {};
}
}
namespace {
NumericDocumentResult project_network_candidate(const Source& source,Handle network,std::size_t maximum_routes,QJsonObject& candidate) {
    if(!network.source)return {};
    auto result=object_keys(source,network,{"serial","routes"},"network");
    if(result)result=project_network_serial(source,network,candidate);
    if(result)result=project_network_routes(source,network,maximum_routes,candidate);
    return result;
}
}
NumericDocumentResult read_numeric_network_settings(const NumericProjectDocument& document,std::size_t maximum_routes,QJsonObject& out) {
    const auto& source=document.source;std::uint32_t schema=0;
    auto result=check_project(source,schema);if(!result)return result;
    Handle network;const auto found=find_unique_member(source,source.root(),"network",network);
    if(!found && found.code!=Error::MissingMember)return source_error(found,"network");
    QJsonObject candidate;
    result=project_network_candidate(source,network,maximum_routes,candidate);
    if(result)out=std::move(candidate);return result;
}
namespace {
QByteArray json_value_bytes(const QJsonValue& value) {
    const auto encoded=QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);return encoded.mid(1,encoded.size()-2);
}
class PresentationEditBatch {
    const NumericProjectDocument& document;
    std::vector<DocumentEdit> edits;
    std::vector<QByteArray> payloads;
    std::size_t limit;
public:
    NumericDocumentResult result;
    const Source& source() const {return document.source;}
    PresentationEditBatch(const NumericProjectDocument& input,std::size_t maximum):document(input),limit(maximum) {
        payloads.reserve(maximum);edits.reserve(maximum);
    }
    bool fail(QString message,QString path={}){result=failure(std::move(message),std::move(path));return false;}
    bool diagnostic(Diagnostic value,QString path={}) {
        if(value)return true;
        result=source_error(value,std::move(path));return false;
    }
    bool find(Handle parent,std::string_view key,Handle& found,QString path={}) {
        const auto value=find_unique_member(source(),parent,key,found);
        if(value.code!=Error::AmbiguousKey)return true;
        result=source_error(value,std::move(path));return false;
    }
    bool append(Handle parent,std::string_view key,Handle old,QByteArray replacement,const QString& path={}) {
        if(edits.size()>=limit)return fail("Settings edit limit",path);
        if(old.source){payloads.push_back(std::move(replacement));edits.push_back({EditKind::Replace,old,{}, {},node(source(),old).raw,view(payloads.back())});}
        else {
            payloads.push_back(json_value_bytes(text(key))+":"+replacement);
            edits.push_back({EditKind::Insert,{},parent,{},node(source(),parent).raw,view(payloads.back())});
        }
        return true;
    }
    bool object(Handle parent,std::string_view key,const QJsonObject& update) {
        const auto known=QJsonDocument(update).toJson(QJsonDocument::Compact);Source fresh;
        if(!diagnostic(parse_project_source(view(known),fresh)))return false;
        Handle old;if(!find(parent,key,old))return false;
        if(old.source && node(source(),old).kind!=NodeKind::Object)return fail("Malformed presentation object",text(key));
        const auto replacement=old.source?merge(source(),old,fresh,fresh.root(),key,result,{},false,key=="adapter"?"adapter":""):known;
        return result && append(parent,key,old,replacement);
    }
    NumericDocumentResult commit(NumericProjectDocument& out) {
        NumericProjectDocument candidate;
        if(!diagnostic(stage_project_edits(source(),edits,candidate.source)))return result;
        std::uint32_t schema=0;result=check_project(candidate.source,schema);if(!result)return result;
        candidate.active_path=document.active_path;candidate.disk_baseline=document.disk_baseline;
        out=std::move(candidate);return {};
    }
};
bool settings_array_has_ids(const Source& source,Handle array) {
    bool any=false;
    for(auto child=node(source,array).first_child;child.source;child=node(source,child).next_sibling){
        Handle id;const auto result=find_unique_member(source,child,"id",id);
        if(!result || !id.source || (node(source,id).kind!=NodeKind::String && node(source,id).kind!=NodeKind::Number))return false;
        any=true;
    }
    return any;
}
class ApplicationSettingsDelta {
    PresentationEditBatch batch;
    bool validate_presets(const QJsonValue& value,Source& fresh,const QString& path) {
        if(!value.isArray())return batch.fail("Expected preset array",path);
        const auto encoded=QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact);
        if(!batch.diagnostic(parse_project_source(view(encoded),fresh),path))return false;
        batch.result=validate_preset_replacement(fresh,fresh.root());return bool(batch.result);
    }
    bool presets(Handle parent,Handle old,const QJsonValue& value,const QString& path) {
        if(old.source && node(batch.source(),old).kind!=NodeKind::Array)return batch.fail("Expected preset array",path);
        Source fresh;if(!validate_presets(value,fresh,path))return false;
        const auto replacement=old.source?merge(batch.source(),old,fresh,fresh.root(),"pipeline_presets",batch.result,{},true):bytes(fresh.bytes());
        return batch.result && batch.append(parent,"pipeline_presets",old,replacement,path);
    }
    bool array(Handle parent,std::string_view key,Handle old,const QJsonValue& value,const QString& path) {
        const auto encoded=QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact);Source fresh;
        if(!batch.diagnostic(parse_project_source(view(encoded),fresh),path))return false;
        const bool old_ids=settings_array_has_ids(batch.source(),old),new_ids=settings_array_has_ids(fresh,fresh.root());
        if(old_ids && !new_ids && node(fresh,fresh.root()).first_child.source)return batch.fail("Settings array identity missing",path);
        const auto replacement=merge(batch.source(),old,fresh,fresh.root(),old_ids || new_ids?"settings_ids":key,batch.result,{},true);
        return batch.result && batch.append(parent,key,old,replacement,path);
    }
    bool object(Handle parent,const QJsonObject& value,const QString& path) {
        for(auto field=value.begin();field!=value.end();++field){
            const auto key=field.key().toUtf8();if(!delta(parent,view(key),field.value(),path+"."+field.key()))return false;
        }
        return true;
    }
    bool existing(Handle parent,std::string_view key,Handle old,const QJsonValue& value,const QString& path) {
        bool complete=true;const auto projected=settings_projection(batch.source(),old,complete);
        if(complete && projected==value)return true;
        const auto kind=node(batch.source(),old).kind;
        if(kind==NodeKind::Object && value.isObject())return object(old,value.toObject(),path);
        if(complete && kind==NodeKind::Array && value.isArray())return array(parent,key,old,value,path);
        if(!complete || kind==NodeKind::Array || value.isArray() || kind==NodeKind::Object)return batch.fail("Unsupported structural/opaque settings edit",path);
        return batch.append(parent,key,old,json_value_bytes(value),path);
    }
    bool delta(Handle parent,std::string_view key,const QJsonValue& value,const QString& path) {
        Handle old;if(!batch.find(parent,key,old,path))return false;
        if(path=="ui.pipeline_presets")return presets(parent,old,value,path);
        return old.source?existing(parent,key,old,value,path):batch.append(parent,key,old,json_value_bytes(value),path);
    }
public:
    explicit ApplicationSettingsDelta(const NumericProjectDocument& source):batch(source,8192){}
    NumericDocumentResult stage(const QJsonObject& settings,NumericProjectDocument& out) {
        const auto ui=settings.value("ui").toObject();
        if(ui.contains("pipeline_presets")){Source checked;if(!validate_presets(ui.value("pipeline_presets"),checked,"ui.pipeline_presets"))return batch.result;}
        for(auto entry=settings.begin();entry!=settings.end();++entry){
            if((entry.key()!="core" && entry.key()!="ui" && entry.key()!="wled_ar_server") || !entry.value().isObject())return failure("Unregistered application settings section",entry.key());
            const auto key=entry.key().toUtf8();if(!delta(batch.source().root(),view(key),entry.value(),entry.key()))return batch.result;
        }
        return batch.commit(out);
    }
};
NumericDocumentResult validate_position_update(const QJsonObject& update) {
    for(auto field=update.begin();field!=update.end();++field)
        if((field.key()!="x" && field.key()!="y") || !field.value().isDouble() || !std::isfinite(field.value().toDouble()))return failure("Unregistered/invalid position field",field.key());
    return {};
}
NumericDocumentResult validate_presentation_update(const QJsonObject& update) {
    constexpr std::array<std::string_view,8> fields{"inline_parameters","display_name","scalar_inputs","stream_names","show_dc_outputs","input_port_order","output_port_order","port_labels"};
    for(auto field=update.begin();field!=update.end();++field){
        const auto name=field.key().toStdString();if(std::find(fields.begin(),fields.end(),name)==fields.end())return failure("Unregistered node presentation field",field.key());
    }
    Source checked;const auto encoded=QJsonDocument(update).toJson(QJsonDocument::Compact);
    const auto parsed=parse_project_source(view(encoded),checked);if(!parsed)return source_error(parsed);
    return check_node_presentation(checked,checked.root());
}
NumericDocumentResult validate_adapter_update(const QJsonObject& update) {
    QJsonObject checked;const auto encoded=QJsonDocument(update).toJson(QJsonDocument::Compact);
    const auto result=decode_numeric_node_adapter(encoded,checked);if(!result)return result;
    return checked==update?NumericDocumentResult{}:failure("Unregistered adapter field");
}
NumericDocumentResult validate_editor_update(const QJsonObject& update) {
    constexpr std::array<std::string_view,8> fields{"version","scale","center_x","center_y","node_width","port_spacing","port_radius","port_font"};
    for(auto field=update.begin();field!=update.end();++field){
        const auto key=field.key().toStdString();const auto& value=field.value();
        if(std::find(fields.begin(),fields.end(),key)==fields.end() || !value.isDouble() || !std::isfinite(value.toDouble()) || (key=="version" && value!=1) || (key=="scale" && (value.toDouble()<.2 || value.toDouble()>3)))return failure("Unregistered/invalid editor-view field",field.key());
    }
    return {};
}
class NodePresentationDelta {
    PresentationEditBatch batch;
    bool find_node(std::int64_t id,Handle& matched,bool adapter) {
        const auto graph=member(batch.source(),batch.source().root(),"scene_graph");const auto nodes=member(batch.source(),graph,"nodes");
        for(auto child=node(batch.source(),nodes).first_child;child.source;child=node(batch.source(),child).next_sibling){
            std::int64_t value=-1;
            if(!batch.diagnostic(read_int64(batch.source(),member(batch.source(),child,"id"),value)))return false;
            if(value!=id)continue;
            if(matched.source)return batch.fail(adapter?"Ambiguous adapter node ID":"Ambiguous presentation node ID");
            matched=child;
        }
        return matched.source || batch.fail(adapter?"Adapter node is absent":"Presentation node is absent");
    }
    bool update_nodes(const QJsonObject& updates,const char* key,NumericDocumentResult (*validate)(const QJsonObject&)) {
        const bool adapter=std::string_view(key)=="adapter";
        for(auto entry=updates.begin();entry!=updates.end();++entry){
            bool valid=false;const auto id=entry.key().toLongLong(&valid);
            if(!valid || id<0 || id>std::numeric_limits<NodeId>::max() || QString::number(id)!=entry.key() || !entry.value().isObject())return batch.fail(adapter?"Invalid adapter node ID":"Invalid presentation node ID");
            const auto update=entry.value().toObject();batch.result=validate(update);if(!batch.result)return false;
            Handle matched;if(!find_node(id,matched,adapter) || !batch.object(matched,key,update))return false;
        }
        return true;
    }
    bool editor_view(const QJsonObject& update) {
        if(update.isEmpty())return true;
        batch.result=validate_editor_update(update);if(!batch.result)return false;
        const auto root=batch.source().root(),ui=member(batch.source(),root,"ui"),editor=member(batch.source(),ui,"editor_view");
        if(editor.source){std::uint32_t version=1;batch.result=edition(batch.source(),editor,"version",version,"ui.editor_view",true);if(!batch.result || version!=1)return batch.fail("Unsupported editor-view version");}
        return ui.source?batch.object(ui,"editor_view",update):batch.object(root,"ui",QJsonObject{{"editor_view",update}});
    }
public:
    NodePresentationDelta(const NumericProjectDocument& source,std::size_t capacity):batch(source,capacity){}
    NumericDocumentResult stage(const NumericDraftPresentation& presentation,NumericProjectDocument& out) {
        if(!update_nodes(presentation.positions,"position",&validate_position_update) || !update_nodes(presentation.node_presentations,"presentation",&validate_presentation_update) || !update_nodes(presentation.node_adapters,"adapter",&validate_adapter_update) || !editor_view(presentation.editor_view))return batch.result;
        return batch.commit(out);
    }
};
class NodeSettingsDelta {
    PresentationEditBatch batch;
    QMap<qint64,Handle> index;
    bool index_nodes() {
        const auto& source=batch.source();
        const auto graph=member(source,source.root(),"scene_graph"),nodes=member(source,graph,"nodes");
        for(auto child=node(source,nodes).first_child;child.source;child=node(source,child).next_sibling) {
            std::int64_t id=-1;if(!batch.diagnostic(read_int64(source,member(source,child,"id"),id)))return false;
            if(index.contains(id))return batch.fail("Ambiguous node ID");
            index[id]=child;
        }
        return true;
    }
    bool replace(const ConfiguredProcessingNode& configured) {
        const auto found=index.value(configured.id);
        if(!found.source)return batch.fail("Updated node is absent");
        return replace_node(found,configured);
    }
    bool replace_node(Handle old,const ConfiguredProcessingNode& configured) {
        if(node(batch.source(),member(batch.source(),old,"kind")).decoded!=configured.kind)return batch.fail("Node kind changed; graph replacement required");
        QByteArray encoded;batch.result=encode_numeric_node(configured,encoded);if(!batch.result)return false;
        Source fresh;if(!batch.diagnostic(parse_project_source(view(encoded),fresh)))return false;
        const auto replacement=merge(batch.source(),old,fresh,fresh.root(),"nodes",batch.result);
        return batch.result && batch.append({}, {},old,replacement);
    }
public:
    explicit NodeSettingsDelta(const NumericProjectDocument& source,std::size_t count):batch(source,count){}
    NumericDocumentResult stage(std::span<const ConfiguredProcessingNode> nodes,NumericProjectDocument& out) {
        if(!index_nodes())return batch.result;
        for(const auto& configured:nodes)if(!replace(configured))return batch.result;
        return batch.commit(out);
    }
};
}
NumericDocumentResult stage_numeric_nodes(const NumericProjectDocument& source,std::span<const ConfiguredProcessingNode> nodes,NumericProjectDocument& out) {
    if(nodes.size()>8192)return failure("Node update limit");
    std::uint32_t schema=0;auto result=check_project(source.source,schema);if(!result)return result;
    if(nodes.empty() && &source==&out)return {};
    return NodeSettingsDelta(source,nodes.size()).stage(nodes,out);
}
namespace {
NumericDocumentResult stage_applied_presentation(const NumericProjectDocument& source,const NumericDraftPresentation& presentation,NumericProjectDocument& out) {
    NumericProjectDocument candidate;
    const auto result=stage_project_graph(source,*presentation.applied_graph,"pipeline_graph",candidate);
    if(!result)return result;
    auto remaining=presentation;
    remaining.applied_graph.reset();
    return stage_numeric_presentation(candidate,remaining,out);
}
NumericDocumentResult stage_application_presentation(const NumericProjectDocument& source,const NumericDraftPresentation& presentation,NumericProjectDocument& out) {
    NumericProjectDocument candidate;
    const auto result=ApplicationSettingsDelta(source).stage(presentation.application_settings,candidate);
    if(!result)return result;
    auto remaining=presentation;
    remaining.application_settings={};
    return stage_numeric_presentation(candidate,remaining,out);
}
}
NumericDocumentResult stage_numeric_presentation(const NumericProjectDocument& source,const NumericDraftPresentation& presentation,NumericProjectDocument& out) {
    const DocumentPhaseTrace trace("stage_presentation");
    if(presentation.applied_graph)return stage_applied_presentation(source,presentation,out);
    if(!presentation.application_settings.isEmpty())return stage_application_presentation(source,presentation,out);
    const auto count=std::size_t(presentation.positions.size())+std::size_t(presentation.node_presentations.size())+std::size_t(presentation.node_adapters.size());
    if(count>8192)return failure("Presentation edit limit");
    return NodePresentationDelta(source,count+1).stage(presentation,out);
}
namespace {
NumericDocumentResult load_project_source(const QString& path,NumericProjectDocument& out) {
    const auto resolved=numeric_project_path(path);QFile file(resolved);
    if(!file.open(QIODevice::ReadOnly))return failure(file.errorString(),resolved);
    QByteArray raw;
    auto result=read_bounded_file(file,raw,resolved,"Project source limit");
    if(!result)return result;
    NumericProjectDocument candidate;
    result=read_numeric_project(raw,candidate);
    if(!result)return result;
    candidate.active_path=resolved;
    candidate.disk_baseline=raw;
    out=std::move(candidate);
    return {};
}
}
NumericDocumentResult load_numeric_project(const QString& path,NumericProjectDocument& out,NumericProcessingGraph& graph) {
    NumericProjectDocument candidate;
    auto result=load_project_source(path,candidate);
    if(!result)return result;
    NumericProcessingGraph draft;
    result=read_numeric_draft(candidate,draft);
    if(!result)return result;
    out=std::move(candidate);
    graph=std::move(draft);
    return {};
}
NumericDocumentResult inspect_numeric_save_destination(const QString& path,NumericSaveConsent& out) {
    NumericSaveConsent candidate;candidate.path=numeric_project_path(path);candidate.existed=QFileInfo::exists(candidate.path);
    if(candidate.existed){QFile file(candidate.path);if(!file.open(QIODevice::ReadOnly))return failure(file.errorString(),candidate.path);
        const auto result=read_bounded_file(file,candidate.bytes,candidate.path,"Destination source limit");if(!result)return result;}
    out=std::move(candidate);return {};
}
namespace {
NumericDocumentResult validate_save_source(const NumericProjectDocument& source) {
    std::uint32_t schema=0;
    const auto result=check_project(source.source,schema);
    if(!result)return result;
    return schema==4?NumericDocumentResult{}:unsupported_document_version(schema);
}
class NumericSaveTransaction {
    NumericProjectDocument& source;
    const QString& requested;
    const NumericSaveConsent* consent;
    QString resolved;
    QByteArray existing;
    bool exists=false;
    NumericDocumentResult read_destination() {
        exists=QFileInfo::exists(resolved);
        if(!exists)return {};
        QFile file(resolved);
        if(!file.open(QIODevice::ReadOnly))return failure(file.errorString(),resolved);
        return read_bounded_file(file,existing,resolved,"Destination source limit");
    }
    NumericDocumentResult validate_unconfirmed_destination() const {
        if(!exists) {
            if(resolved==source.active_path && !source.disk_baseline.isEmpty())
                return failure("SourceChanged: original file was removed",resolved);
            return {};
        }
        NumericProjectDocument destination;
        const auto result=read_numeric_project(existing,destination);
        if(!result)return result;
        if((resolved==source.active_path && existing!=source.disk_baseline)
            || (source.active_path.isEmpty() && requested.isEmpty()))
            return failure("SourceChanged: explicit reload or SaveAs required",resolved);
        return {};
    }
    NumericDocumentResult validate_destination() const {
        if(!consent)return validate_unconfirmed_destination();
        if(consent->path!=resolved || consent->existed!=exists || consent->bytes!=existing)
            return failure("SourceChanged after overwrite confirmation",resolved);
        return {};
    }
    bool destination_unchanged() const {
        if(QFileInfo::exists(resolved)!=exists)return false;
        if(!exists)return true;
        QFile before_commit(resolved);
        if(!before_commit.open(QIODevice::ReadOnly))return false;
        return before_commit.read(existing.size()+1)==existing;
    }
    NumericDocumentResult commit() {
        const DocumentPhaseTrace trace("file_commit");
        QSaveFile file(resolved);
        file.setDirectWriteFallback(false);
        if(!file.open(QIODevice::WriteOnly))return failure(file.errorString(),resolved);
        const auto candidate=bytes(source.source.bytes());
        if(file.write(candidate)!=candidate.size())return failure(file.errorString(),resolved);
        // Best-effort change check, not cross-process CAS. No silent reload/reapply.
        if(!destination_unchanged())return failure("SourceChanged during save",resolved);
        if(!file.commit())return failure(file.errorString(),resolved);
        source.active_path=resolved;
        source.disk_baseline=candidate;
        return {};
    }
public:
    NumericSaveTransaction(NumericProjectDocument& document,const QString& path,const NumericSaveConsent* approval)
        :source(document),requested(path),consent(approval),resolved(numeric_project_path(path.isEmpty()?document.active_path:path)){}
    NumericDocumentResult save() {
        auto result=validate_save_source(source);
        if(result)result=read_destination();
        if(result)result=validate_destination();
        if(!result)return result;
        const auto directory=QFileInfo(resolved).absoluteDir();
        if(!directory.exists() && !directory.mkpath("."))return failure("Cannot create project directory",resolved);
        return commit();
    }
};
}
NumericDocumentResult save_numeric_project(NumericProjectDocument& source,const QString& path,const NumericSaveConsent* consent) {
    const DocumentPhaseTrace trace("atomic_file_save");
    return NumericSaveTransaction(source,path,consent).save();
}
} // namespace desktop
