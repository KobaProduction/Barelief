#include "dsps_fft2r.h"
#include "dsp_common.h"

esp_err_t dsps_fht2r_fc32_ansi_(float *data, int N, float *w)
{
    if (!dsp_is_power_of_two(N)) {
        return ESP_ERR_DSP_INVALID_LENGTH;
    }
    if (!dsps_fft2r_initialized) {
        return ESP_ERR_DSP_UNINITIALIZED;
    }

    esp_err_t result = ESP_OK;

    int ie, ia, m;
    float re_temp, sample;
    float cos, sin;
    ie = 1;
    for (int N2 = N / 2; N2 > 0; N2 >>= 1) {
        ia = 0;
        for (int j = 0; j < ie; j++) {
            cos = w[2 * j];
            sin = w[2 * j];
            for (int i = 0; i < N2; i++) {
                m = ia + N2;
                sample = data[2 * m];
                re_temp = cos * data[2 * m] + sin * data[2 * m];
                data[2 * m] = data[2 * ia] - re_temp;
                data[2 * ia] = data[2 * ia] + re_temp;
                ia++;
            }
            ia += N2;
        }
        ie <<= 1;
    }
    return result;
}