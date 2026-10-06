#include "analyzer/analyze/spectrogram.h"

#include <algorithm>

Spectrogram::Spectrogram(size_t history_size)
        : history_size_(history_size > 0 ? history_size : 1),
          channels_(0),
          write_index_(0),
          filled_count_(0)
{}


bool Spectrogram::push(const std::vector<float>& spectrum) {
    if (spectrum.empty()) {
        return false;
    }

    // Если число каналов изменилось — полный сброс
    if (spectrum.size() != channels_) {
        reset(spectrum.size());
    }

    for (size_t ch = 0; ch < channels_; ++ch) {
        data_[ch][write_index_] = spectrum[ch];
    }

    write_index_ = (write_index_ + 1) % history_size_;
    if (filled_count_ < history_size_) {
        ++filled_count_;
    }

    return true;
}

bool Spectrogram::set_history_size(size_t new_size) {
    if (new_size == 0 || new_size == history_size_) {
        return new_size != 0;
    }

    // Сначала разворачиваем текущие данные в линейный порядок (старые→новые)
    const size_t old_filled = filled_count_;
    std::vector<std::vector<float>> linear(channels_, std::vector<float>(history_size_, 0.0f));

    for (size_t ch = 0; ch < channels_; ++ch) {
        unwrap_channel(ch, linear[ch]);
    }

    history_size_ = new_size;
    write_index_  = 0;
    filled_count_ = 0;

    // Пересоздаём буферы под новый размер истории, заполняем нулями
    data_.assign(channels_, std::vector<float>(history_size_, 0.0f));

    const size_t keep = std::min(old_filled, new_size);
    for (size_t ch = 0; ch < channels_; ++ch) {
        std::copy(linear[ch].end() - keep, linear[ch].end(), data_[ch].begin());
    }
    filled_count_ = keep;
    write_index_ = keep % new_size;

    return true;
}

std::vector<float> Spectrogram::get_channel_history(const size_t channel_index) const {
    if (channel_index >= channels_) {
        return {};
    }

    std::vector result(history_size_, 0.0f);
    unwrap_channel(channel_index, result);
    return result;
}

std::vector<std::vector<float>> Spectrogram::get_spectrogram() const {
    std::vector<std::vector<float>> result(channels_);
    for (size_t ch = 0; ch < channels_; ++ch) {
        result[ch] = get_channel_history(ch);
    }
    return result;
}

std::vector<float> Spectrogram::get_channel_history_reversed(const size_t channel_index) const {
    std::vector<float> result;
    if (channel_index >= channels_) {
        return result;
    }
    result = get_channel_history(channel_index);
    std::ranges::reverse(result);
    return result;
}

std::vector<std::vector<float>> Spectrogram::get_spectrogram_reversed() const {
    std::vector<std::vector<float>> result(channels_);
    for (size_t ch = 0; ch < channels_; ++ch) {
        result[ch] = get_channel_history_reversed(ch);
    }
    return result;
}


size_t Spectrogram::history_size() const { return history_size_; }
size_t Spectrogram::channels()     const { return channels_; }
size_t Spectrogram::filled_count() const { return filled_count_; }
bool        Spectrogram::is_full()      const { return filled_count_ == history_size_; }


std::vector<std::vector<float>> Spectrogram::get_vector_spectrogram() const {
    std::vector<std::vector<float>> result;
    result.resize(channels_, {});
    for (size_t ch = 0; ch < channels_; ch++) {
        result[ch] = get_channel_history_reversed(ch);
    }
    return result;
}


void Spectrogram::reset(size_t new_channels) {
    channels_     = new_channels;
    write_index_  = 0;
    filled_count_ = 0;
    data_.assign(channels_, std::vector<float>(history_size_, 0.0f));
}

void Spectrogram::unwrap_channel(size_t channel_index, std::vector<float>& output) const {
    // Буфер не полный: данные лежат с позиции 0 до filled_count_ - 1
    // в хронологическом порядке, остаток — нули.
    if (filled_count_ < history_size_) {
        const size_t zeros = history_size_ - filled_count_;
        for (size_t i = 0; i < zeros; ++i) {
            output[i] = 0.0f;
        }
        for (size_t i = 0; i < filled_count_; ++i) {
            output[zeros + i] = data_[channel_index][i];
        }
        return;
    }

    // Буфер полный: write_index_ указывает на самый старый элемент
    for (size_t i = 0; i < history_size_; ++i) {
        output[i] = data_[channel_index][(write_index_ + i) % history_size_];
    }
}
void Spectrogram::clear() { reset(channels_); }
