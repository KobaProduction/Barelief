#include "dsps_fft4r.h"
#include "dsp_common.h"
#include "dsp_types.h"

esp_err_t dsps_fht4r_fc32_ansi_(float *data, int length, float *table, int table_size)
{
    if (0 == dsps_fft4r_initialized) {
        return ESP_ERR_DSP_UNINITIALIZED;
    }

    fc32_t bfly[4];
    int log2N = dsp_power_of_two(length);
    int log4N = log2N >> 1;
    if ((log2N & 0x01) != 0) {
        return ESP_ERR_DSP_INVALID_LENGTH;
    }

    int m = 2;
    int wind_step = table_size / length;
    while (1) {  ///radix 4
        if (log4N == 0) {
            break;
        }
        length = length >> 2;
        for (int j = 0; j < m; j += 2) { // j: which FFT of this step
            int start_index = j * (length << 1); // n: n-point FFT

            fc32_t *ptrc0 = (fc32_t *)data + start_index;
            fc32_t *ptrc1 = ptrc0 + length;
            fc32_t *ptrc2 = ptrc1 + length;
            fc32_t *ptrc3 = ptrc2 + length;

            fc32_t *winc0 = (fc32_t *)table;
            fc32_t *winc1 = winc0;
            fc32_t *winc2 = winc0;

            for (int k = 0; k < length; k++) {
                fc32_t in0 = *ptrc0;
                fc32_t in2 = *ptrc2;
                fc32_t in1 = *ptrc1;
                fc32_t in3 = *ptrc3;

                bfly[0].re = in0.re + in2.re + in1.re + in3.re;
                bfly[1].re = in0.re - in2.re + in1.re - in3.re;
                bfly[2].re = in0.re + in2.re - in1.re - in3.re;
                bfly[3].re = in0.re - in2.re - in1.re + in3.re;

//                bfly[0].re = in0.re + in2.re + in1.re + in3.re;
//                bfly[0].im = in0.im + in2.im + in1.im + in3.im;
//
//                bfly[1].re = in0.re - in2.re + in1.im - in3.im;
//                bfly[1].im = in0.im - in2.im - in1.re + in3.re;
//
//                bfly[2].re = in0.re + in2.re - in1.re - in3.re;
//                bfly[2].im = in0.im + in2.im - in1.im - in3.im;
//
//                bfly[3].re = in0.re - in2.re - in1.im + in3.im;
//                bfly[3].im = in0.im - in2.im + in1.re - in3.re;



                *ptrc0 = bfly[0];

                ptrc1->re = bfly[1].re * winc0->re + bfly[1].re * winc0->re;
                ptrc2->re = bfly[2].re * winc1->re + bfly[2].re * winc1->re;
                ptrc3->re = bfly[3].re * winc2->re + bfly[3].re * winc2->re;

//                ptrc1->re = bfly[1].re * winc0->re + bfly[1].im * winc0->im;
//                ptrc1->im = bfly[1].im * winc0->re - bfly[1].re * winc0->im;
//                ptrc2->re = bfly[2].re * winc1->re + bfly[2].im * winc1->im;
//                ptrc2->im = bfly[2].im * winc1->re - bfly[2].re * winc1->im;
//                ptrc3->re = bfly[3].re * winc2->re + bfly[3].im * winc2->im;
//                ptrc3->im = bfly[3].im * winc2->re - bfly[3].re * winc2->im;

                winc0 += 1 * wind_step;
                winc1 += 2 * wind_step;
                winc2 += 3 * wind_step;

                ptrc0++;
                ptrc1++;
                ptrc2++;
                ptrc3++;
            }
        }
        m = m << 2;
        wind_step = wind_step << 2;
        log4N--;
    }
    return ESP_OK;
}