#include "pipeline/stream_codec.h"
#include <bit>
#include <cmath>
#include <stdexcept>
namespace pipeline::stream {
namespace {
constexpr size_t Header = 112;
} // namespace
size_t payload_size(const Frame &f) {
    if (!f.available)
        return Header + f.stream_id.size();
    size_t n = 0;
    if (int(f.type) <= 3)
        n = f.left.size() + f.right.size();
    else if (f.type != Type::History)
        n = f.values.size();
    else
        for (auto &r : f.history)
            n += r.size();
    return n * 4 + Header + f.stream_id.size();
}
bool valid(const Frame &f) {
    auto t = int(f.type);
    if (t < 0 || t > 6 || (f.available && (!f.sample_rate || !f.sequence)) || f.sample_rate > 768000 ||
        f.stream_id.size() > 256 || f.fft_size > 65536 || f.bands > 4096 || f.history_capacity > 2000 ||
        payload_size(f) > MaxFrameBytes)
        return false;
    if (!std::isfinite(f.frequency_step) || f.frequency_step < 0 || !std::isfinite(f.peak_hz) ||
        !std::isfinite(f.magnitude))
        return false;
    if (!f.available)
        return f.sequence == 0 && f.left.empty() && f.right.empty() && f.values.empty() && f.history.empty();
    auto finite = [](const auto &v) {
        for (float x : v)
            if (!std::isfinite(x))
                return false;
        return true;
    };
    if (!finite(f.left) || !finite(f.right) || !finite(f.values))
        return false;
    if (t <= 3) {
        if (f.left.empty() || f.left.size() != f.right.size())
            return false;
    } else if (t <= 5) {
        if (f.values.empty())
            return false;
    } else {
        if (f.history.empty() || f.history[0].empty())
            return false;
        for (auto &r : f.history)
            if (r.size() != f.history[0].size() || !finite(r))
                return false;
    }
    return true;
}
std::vector<uint8_t> encode(const Frame &f) {
    if (!valid(f)) throw std::invalid_argument("Invalid typed frame");
    signal_contracts::FrameView view;
    view.type=f.type;
    auto &m=view.metadata;
    m.sequence=f.sequence; m.generation=f.generation; m.epoch=f.epoch; m.graph_revision=f.graph_revision;
    m.source_sequence=f.source_sequence; m.producer_epoch=f.producer_epoch; m.timestamp_ns=f.timestamp_ns;
    m.sample_rate=f.sample_rate; m.fft_size=f.fft_size; m.bands=f.bands; m.history_capacity=f.history_capacity;
    m.frequency_step=f.frequency_step; m.peak_hz=f.peak_hz; m.magnitude=f.magnitude; m.stream_id=f.stream_id;
    m.discontinuity=f.discontinuity; m.bark_weighted=f.bark_weighted; m.available=f.available;
    view.left=f.left; view.right=f.right; view.values=f.values;
    std::vector<float> history;
    if(f.available && f.type==Type::History) {
        history.reserve(f.history.size()*f.history[0].size());
        for(const auto &row:f.history) history.insert(history.end(),row.begin(),row.end());
        view.history={history,f.history[0].size(),f.history.size(),f.history.size(),0};
    }
    std::vector<uint8_t> bytes(payload_size(f));
    if(!bdsp_protocol::encode(view,bytes)) throw std::invalid_argument("Invalid typed frame");
    return bytes;
}
bool decode(std::span<const uint8_t> bytes, Frame &out, std::string &error) {
    signal_contracts::FrameView view;
    auto result=bdsp_protocol::decode(bytes,{}, {},view);
    if(!result && result.status!=signal_contracts::Status::buffer_too_small) {
        error=bdsp_protocol::error_message(result.error); return false;
    }
    std::vector<float> samples(result.required_samples);
    std::string stream_id(result.required_id_bytes,'\0');
    result=bdsp_protocol::decode(bytes,samples,stream_id,view);
    if(!result){error=bdsp_protocol::error_message(result.error);return false;}
    Frame f; const auto &m=view.metadata;
    f.type=view.type;f.sequence=m.sequence;f.generation=m.generation;f.epoch=m.epoch;f.graph_revision=m.graph_revision;
    f.source_sequence=m.source_sequence;f.producer_epoch=m.producer_epoch;f.timestamp_ns=m.timestamp_ns;
    f.sample_rate=m.sample_rate;f.fft_size=m.fft_size;f.bands=m.bands;f.history_capacity=m.history_capacity;
    f.frequency_step=m.frequency_step;f.peak_hz=m.peak_hz;f.magnitude=m.magnitude;f.stream_id=std::move(stream_id);
    f.discontinuity=m.discontinuity;f.bark_weighted=m.bark_weighted;f.available=m.available;
    f.left.assign(view.left.begin(),view.left.end());f.right.assign(view.right.begin(),view.right.end());
    f.values.assign(view.values.begin(),view.values.end());
    for(size_t i=0;i<view.history.frames;++i) {
        const auto row=view.history.row(i); f.history.emplace_back(row.begin(),row.end());
    }
    out=std::move(f);error.clear();return true;
}
} // namespace pipeline::stream