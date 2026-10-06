#include <desktop/graph_document.h>
#include <QJsonArray>
#include <limits>
#include <cmath>
namespace desktop {
namespace {
bool integer(const QJsonValue& value, int& output) {
    if(!value.isDouble()) return false;
    const double n=value.toDouble();
    if(!std::isfinite(n) || std::floor(n)!=n || n<0 || n>=std::numeric_limits<int>::max()) return false;
    output=int(n); return true;
}
signal_contracts::FrameType frame_type(const QString& value) {
    using T=signal_contracts::FrameType;
    if(value=="window") return T::Window;
    if(value=="windowed") return T::Windowed;
    if(value=="stereo-spectrum") return T::StereoSpectrum;
    if(value=="spectrum") return T::Spectrum;
    if(value=="history") return T::History;
    if(value=="audio") return T::PCM;
    return T::Mel;
}
}
GraphDocument read_graph_document(const QJsonObject& input) {
    GraphDocument result; result.document=input;
    result.graph.graph_id=input.value("graph_id").toString("main").toStdString();
    if(!input.value("nodes").isArray() || !input.value("connections").isArray()) {
        result.error="Graph arrays missing"; return result;
    }
    for(const auto& value:input.value("nodes").toArray()) {
        const auto object=value.toObject(); auto p=object.value("internal-data").toObject();
        const auto kind=domain::node_kind(p.value("model-name").toString().toStdString());
        int id=-1;
        if(!kind || !integer(object.value("id"),id)) {result.error="Invalid node kind or ID";return result;}
        if((*kind==domain::NodeKind::NetworkInput || *kind==domain::NodeKind::NetworkOutput) &&
           !p.contains("data_type") && p.contains("type")) p["data_type"]=p.value("type");
        QStringList registered{"enabled"};
        using K=domain::NodeKind;
        switch(*kind) {
        case K::AudioSource: registered<<"source";break;
        case K::SpectrumMerge: registered<<"merge";break;
        case K::SpectrumGain: case K::WLED: registered<<"gain";break;
        case K::Mel: registered<<"bands";break;
        case K::Spectrogram: registered<<"history_size";break;
        case K::FFTView: registered<<"channel"<<"merge";break;
        case K::Window: registered<<"size";break;
        case K::WindowFunction: registered<<"function";break;
        case K::NetworkInput: case K::NetworkOutput: registered<<"data_type"<<"provider_uuid"<<"stream_id"<<"host"<<"port";break;
        case K::TestSignal: registered<<"tones"<<"sample_rate"<<"minimum_hz"<<"maximum_hz"<<"amplitude"<<"period_seconds"<<"sweep_depth";break;
        default: break;
        }
        // Validate external JSON representation before numeric conversion. Domain
        // validates semantic ranges afterward; conversion must never erase errors.
        const QStringList integers{"source","merge","channel","bands","history_size","size","tones","sample_rate","port"};
        const QStringList numbers{"gain","minimum_hz","maximum_hz","amplitude","period_seconds","sweep_depth"};
        const QStringList strings{"provider_uuid","stream_id","host","function","data_type"};
        for(const auto& key:integers) if(registered.contains(key) && p.contains(key)) {
            int parsed;
            if(!integer(p.value(key),parsed)) {result.error="Invalid integer: "+key;return result;}
        }
        for(const auto& key:numbers) if(registered.contains(key) && p.contains(key) && (!p.value(key).isDouble() || !std::isfinite(p.value(key).toDouble()))) {
            result.error="Invalid number: "+key;return result;
        }
        for(const auto& key:strings) if(registered.contains(key) && p.contains(key) && !p.value(key).isString()) {result.error="Invalid string: "+key;return result;}
        if(p.contains("enabled") && !p.value("enabled").isBool()) {result.error="Invalid enabled flag";return result;}
        if(registered.contains("function") && p.contains("function") && !QStringList{"hann","hamming","blackman","rectangular"}.contains(p.value("function").toString())) {
            result.error="Invalid window function";return result;
        }
        if(registered.contains("data_type") && p.contains("data_type") && !QStringList{"audio","window","windowed","stereo-spectrum","spectrum","mel","history"}.contains(p.value("data_type").toString())) {
            result.error="Invalid frame type";return result;
        }
        domain::NodeDefinition node{id,*kind,domain::default_parameters(*kind),p.value("enabled").toBool(true)};
        std::visit([&](auto& parameters) {
            using P=std::decay_t<decltype(parameters)>;
            if constexpr(std::is_same_v<P,domain::AudioSourceParameters>) parameters.source=domain::AudioSource(p.value("source").toInt(0));
            else if constexpr(std::is_same_v<P,domain::MergeParameters>) parameters.merge=domain::MergeMode(p.value("merge").toInt(2));
            else if constexpr(std::is_same_v<P,domain::GainParameters>) parameters.gain=p.value("gain").toDouble(1);
            else if constexpr(std::is_same_v<P,domain::MelParameters>) parameters.bands=p.value("bands").toInt(50);
            else if constexpr(std::is_same_v<P,domain::HistoryParameters>) parameters.history_size=p.value("history_size").toInt(400);
            else if constexpr(std::is_same_v<P,domain::FFTViewParameters>) {parameters.channel=domain::Channel(p.value("channel").toInt(0));parameters.merge=domain::MergeMode(p.value("merge").toInt(2));}
            else if constexpr(std::is_same_v<P,domain::WindowParameters>) parameters.size=p.value("size").toInt(4096);
            else if constexpr(std::is_same_v<P,domain::WindowFunctionParameters>) {
                const auto function=p.value("function").toString("hann");
                parameters.function=function=="hamming"?domain::WindowFunction::Hamming:function=="blackman"?domain::WindowFunction::Blackman:function=="rectangular"?domain::WindowFunction::Rectangular:domain::WindowFunction::Hann;
            } else if constexpr(std::is_same_v<P,domain::NetworkParameters>) {
                parameters.data_type=frame_type(p.value("data_type").toString("mel"));
                parameters.provider_uuid=p.value("provider_uuid").toString().toStdString();
                parameters.stream_id=p.value("stream_id").toString().toStdString();
                parameters.host=p.value("host").toString().toStdString(); parameters.port=p.value("port").toInt(8214);
            } else if constexpr(std::is_same_v<P,domain::TestSignalParameters>) {
                parameters.tones=p.value("tones").toInt(8);parameters.sample_rate=p.value("sample_rate").toInt(48000);
                parameters.minimum_hz=p.value("minimum_hz").toDouble(73);parameters.maximum_hz=p.value("maximum_hz").toDouble(13901);
                parameters.amplitude=p.value("amplitude").toDouble(.4);parameters.period_seconds=p.value("period_seconds").toDouble(6);
                parameters.sweep_depth=p.value("sweep_depth").toDouble(.18);
            }
        },node.parameters);
        result.graph.nodes.push_back(std::move(node));
    }
    for(const auto& value:input.value("connections").toArray()) {
        const auto edge=value.toObject();int from,to,out,in;
        if(!integer(edge.value("outNodeId"),from) || !integer(edge.value("inNodeId"),to) ||
           !integer(edge.value("outPortIndex"),out) || !integer(edge.value("inPortIndex"),in) || out>65535 || in>65535) {
            result.error="Invalid edge encoding";return result;
        }
        result.graph.edges.push_back({from,to,std::uint16_t(out),std::uint16_t(in)});
    }
    return result;
}
}
