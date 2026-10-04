#ifndef QT_VISUALIZATION_FFT_H
#define QT_VISUALIZATION_FFT_H

#include <cstdio>
#include <vector>
#include "dsp_core/dsp.h"

class FFT {
public:
    typedef enum {SUCCESS, NOT_INIT, INCORRECT_CONFIGS} fft_status_t;
    typedef enum {FFT2R, FFT4R, FHT2R, FHT4R} fft_type_t;
protected:
    int _samples_size = 0;
    dsp_core::FFT _transform;
    fft_type_t _type = FFT2R;
public:

    fft_status_t init(int samples_size, fft_type_t type = FFT2R);

    fft_status_t run(std::vector<float> &samples, std::vector<float> &amplitudes);
};

#endif //QT_VISUALIZATION_FFT_H
