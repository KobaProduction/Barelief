#include "analyzer/analyze/fft.h"

FFT::fft_status_t FFT::init(int samples_size, fft_type_t type) {
    _samples_size = 0;
    if (samples_size < 4 || samples_size > 4096 ||
        (samples_size & (samples_size - 1)) != 0 ||
        (type != FFT2R && type != FFT4R && type != FHT2R && type != FHT4R)) return INCORRECT_CONFIGS;
    if ((type == FFT4R || type == FHT4R) && (samples_size & 0x55555555) == 0) return INCORRECT_CONFIGS;
    if (!_transform.prepare(samples_size, type == FFT4R || type == FHT4R)) return NOT_INIT;
    _samples_size = samples_size; _type = type;
    return SUCCESS;
}
FFT::fft_status_t FFT::run(std::vector<float> &samples, std::vector<float> &amplitudes) {
    if (!_samples_size) return NOT_INIT;
    return _transform.process(samples, amplitudes, _type == FHT2R || _type == FHT4R)
        ? SUCCESS : INCORRECT_CONFIGS;
}
