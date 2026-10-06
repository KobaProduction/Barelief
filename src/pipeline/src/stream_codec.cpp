#include "pipeline/stream_codec.h"
#include <bit>
#include <cmath>
#include <stdexcept>
namespace pipeline::stream {
namespace {
void put(std::vector<uint8_t> &b, uint64_t n, int bytes) {
    for (int i = 0; i < bytes; ++i)
        b.push_back(uint8_t(n >> (8 * i)));
}
uint64_t get(std::span<const uint8_t> b, size_t &p, int n) {
    uint64_t v = 0;
    for (int i = 0; i < n; ++i)
        v |= uint64_t(b[p++]) << (8 * i);
    return v;
}
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
    if (!valid(f))
        throw std::invalid_argument("Invalid typed frame");
    std::vector<uint8_t> b;
    b.reserve(payload_size(f));
    put(b, 2, 2);
    put(b, int(f.type), 1);
    put(b, (f.discontinuity ? 1 : 0) | (f.bark_weighted ? 2 : 0) | (f.available ? 0 : 4), 1);
    for (auto v : {f.epoch, f.producer_epoch, f.generation, f.sequence, f.source_sequence, f.graph_revision,
                   std::bit_cast<uint64_t>(f.timestamp_ns)})
        put(b, v, 8);
    for (auto v : {f.sample_rate, f.fft_size, f.bands, f.history_capacity})
        put(b, v, 4);
    for (float v : {f.frequency_step, f.peak_hz, f.magnitude})
        put(b, std::bit_cast<uint32_t>(v), 4);
    const uint32_t columns = !f.available              ? 0
                             : int(f.type) <= 3        ? f.left.size()
                             : f.type == Type::History ? f.history[0].size()
                                                       : f.values.size();
    put(b, columns, 4);
    put(b, !f.available ? 0 : f.type == Type::History ? f.history.size() : 1, 4);
    put(b, f.stream_id.size(), 4);
    put(b, 0, 4);
    put(b, 0, 8);
    b.insert(b.end(), f.stream_id.begin(), f.stream_id.end());
    auto floats = [&](const auto &v) {
        for (float x : v)
            put(b, std::bit_cast<uint32_t>(x), 4);
    };
    if (!f.available)
        return b;
    if (int(f.type) <= 3) {
        floats(f.left);
        floats(f.right);
    } else if (f.type != Type::History)
        floats(f.values);
    else
        for (auto &r : f.history)
            floats(r);
    return b;
}
bool decode(std::span<const uint8_t> b, Frame &f, std::string &error) {
    auto fail = [&](const char *s) {
        error = s;
        return false;
    };
    if (b.size() < Header || b.size() > MaxFrameBytes)
        return fail("Invalid frame size");
    size_t p = 0;
    if (get(b, p, 2) != 2)
        return fail("Unsupported stream version");
    Frame x;
    x.type = Type(get(b, p, 1));
    auto flags = get(b, p, 1);
    if (flags > 7)
        return fail("Invalid flags");
    x.available = !(flags & 4);
    x.discontinuity = flags & 1;
    x.bark_weighted = flags & 2;
    x.epoch = get(b, p, 8);
    x.producer_epoch = get(b, p, 8);
    x.generation = get(b, p, 8);
    x.sequence = get(b, p, 8);
    x.source_sequence = get(b, p, 8);
    x.graph_revision = get(b, p, 8);
    x.timestamp_ns = std::bit_cast<int64_t>(get(b, p, 8));
    x.sample_rate = get(b, p, 4);
    x.fft_size = get(b, p, 4);
    x.bands = get(b, p, 4);
    x.history_capacity = get(b, p, 4);
    x.frequency_step = std::bit_cast<float>(uint32_t(get(b, p, 4)));
    x.peak_hz = std::bit_cast<float>(uint32_t(get(b, p, 4)));
    x.magnitude = std::bit_cast<float>(uint32_t(get(b, p, 4)));
    auto columns = get(b, p, 4), rows = get(b, p, 4), id = get(b, p, 4);
    if (get(b, p, 4) || get(b, p, 8))
        return fail("Reserved fields");
    int t = int(x.type);
    if (columns > (MaxFrameBytes - Header) / 4 || rows > (MaxFrameBytes - Header) / 4)
        return fail("Oversized dimensions");
    uint64_t count = columns * rows * (t <= 3 ? 2 : 1);
    if (t < 0 || t > 6 || (x.available && (!columns || !rows || (t != 6 && rows != 1))) ||
        (!x.available && (columns || rows || x.sequence)) || id > 256 || count > (MaxFrameBytes - Header) / 4 ||
        b.size() != Header + id + count * 4)
        return fail("Invalid dimensions/type/length");
    x.stream_id.assign(reinterpret_cast<const char *>(b.data() + p), id);
    p += id;
    auto floats = [&](auto &v) {
        v.resize(columns);
        for (float &y : v)
            y = std::bit_cast<float>(uint32_t(get(b, p, 4)));
    };
    if (!x.available) {
        if (!valid(x))
            return fail("Invalid unavailable marker");
        f = std::move(x);
        error.clear();
        return true;
    }
    if (t <= 3) {
        floats(x.left);
        floats(x.right);
    } else if (t <= 5)
        floats(x.values);
    else {
        x.history.resize(rows);
        for (auto &r : x.history)
            floats(r);
    }
    if (!valid(x))
        return fail("Invalid frame values");
    f = std::move(x);
    error.clear();
    return true;
}
} // namespace pipeline::stream
