#include "analyzer/analyze/audio_window.h"
#include <algorithm>
AudioWindow::AudioWindow(size_t size) : window(size, 0), linear(size, 0) {}
void AudioWindow::append_data(const std::vector<float> &data) { append_data(data.data(), data.size()); }
void AudioWindow::append_data(const float *data, size_t count) {
    if (!data || !count || window.empty()) return;
    if (count >= window.size()) {
        std::copy(data + count - window.size(), data + count, window.begin()); cursor = 0;
        return;
    }
    const auto first = std::min(count, window.size() - cursor);
    std::copy_n(data, first, window.begin() + cursor);
    std::copy_n(data + first, count - first, window.begin());
    cursor = (cursor + count) % window.size();
}
void AudioWindow::clear() { std::fill(window.begin(), window.end(), 0); cursor = 0; }
const std::vector<float> &AudioWindow::get_window() const {
    std::copy(window.begin() + cursor, window.end(), linear.begin());
    std::copy(window.begin(), window.begin() + cursor, linear.end() - cursor);
    return linear;
}
size_t AudioWindow::size() const { return window.size(); }
