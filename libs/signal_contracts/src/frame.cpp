#include "signal_contracts/frame.h"
#include <cmath>
#include <limits>
namespace signal_contracts {
bool compatible(const PortContract &a, const PortContract &b) noexcept { return a == b; }
PortContract contract(const FrameView &f) noexcept {
    const auto &m=f.metadata;
    return {f.type,m.sample_rate,m.fft_size,m.bands,m.history_capacity,
        int(f.type)<=3?f.left.size():f.type==FrameType::History?f.history.bands:f.values.size(),
        m.frequency_step,m.bark_weighted};
}
Status validate(const FrameView &f) noexcept {
    const auto &m=f.metadata; const int t=int(f.type);
    const bool has_history_descriptor=!f.history.storage.empty() || f.history.bands ||
        f.history.frames || f.history.capacity || f.history.oldest || !f.history.newest_override.empty();
    if(t<0 || t>6 || m.sample_rate>768000 || m.fft_size>65536 || m.bands>4096 ||
       m.history_capacity>2000 || m.stream_id.size()>256 || !std::isfinite(m.frequency_step) ||
       m.frequency_step<0 || !std::isfinite(m.peak_hz) || !std::isfinite(m.magnitude)) return Status::invalid_argument;
    if(!m.available) return !m.sequence && f.left.empty() && f.right.empty() && f.values.empty() &&
        !has_history_descriptor?Status::unavailable:Status::invalid_argument;
    if(!m.sequence || !m.sample_rate) return Status::invalid_argument;
    auto finite=[](auto values){for(float v:values) if(!std::isfinite(v))return false;return true;};
    if(t<=3) {
        if(f.left.empty() || f.left.size()!=f.right.size() || !f.values.empty() ||
           has_history_descriptor || !finite(f.left) || !finite(f.right))return Status::invalid_argument;
        if(t && m.fft_size && f.left.size() != (t==3?m.fft_size/2:m.fft_size))return Status::invalid_argument;
    } else if(t<=5) {
        if(f.values.empty() || !f.left.empty() || !f.right.empty() || has_history_descriptor ||
           !finite(f.values) || (t==5 && m.bands && f.values.size()!=m.bands) ||
           (t==4 && m.fft_size && f.values.size()!=m.fft_size/2))return Status::invalid_argument;
    } else {
        const auto &h=f.history;
        if(!f.left.empty() || !f.right.empty() || !f.values.empty() || !h.bands || !h.frames ||
           !h.capacity || h.frames>h.capacity || h.oldest>=h.capacity ||
           h.bands>std::numeric_limits<std::size_t>::max()/h.capacity || h.storage.size()!=h.bands*h.capacity ||
           (!h.newest_override.empty() && h.newest_override.size()!=h.bands) ||
           (m.bands && m.bands!=h.bands) || (m.history_capacity && h.capacity>m.history_capacity))return Status::invalid_argument;
        for(std::size_t i=0;i<h.frames;++i) if(!finite(h.row(i))) return Status::invalid_argument;
    }
    return Status::ok;
}
}
