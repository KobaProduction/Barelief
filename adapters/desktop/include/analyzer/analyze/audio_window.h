#ifndef ARD_AUDIO_WINDOW_H
#define ARD_AUDIO_WINDOW_H

#include <vector>
#include <cstdint>

class AudioWindow {
private:
    std::vector<float> window;
    mutable std::vector<float> linear;
    size_t cursor = 0;
public:
    explicit AudioWindow(size_t size);

    void append_data(const std::vector<float> &newData);
    void append_data(const float *data, size_t dataSize);
    void clear();
    const std::vector<float> &get_window() const;
    size_t size() const;

};

#endif //ARD_AUDIO_WINDOW_H
