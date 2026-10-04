add_library(dsp STATIC)

target_include_directories(dsp PUBLIC common/include)
target_include_directories(dsp PUBLIC common/include_sim)
target_include_directories(dsp PUBLIC fft/include)

target_include_directories(dsp PUBLIC windows/blackman/include)
target_include_directories(dsp PUBLIC windows/blackman_harris/include)
target_include_directories(dsp PUBLIC windows/blackman_nuttall/include)
target_include_directories(dsp PUBLIC windows/flat_top/include)
target_include_directories(dsp PUBLIC windows/hann/include)
target_include_directories(dsp PUBLIC windows/nuttall/include)

target_sources(dsp PRIVATE
        common/misc/dsps_pwroftwo.cpp
        fft/float/dsps_fft2r_fc32_ansi.c
        fft/float/dsps_fft4r_fc32_ansi.c
        fft/float/dsps_fft2r_bitrev_tables_fc32.c
        fft/float/dsps_fft4r_bitrev_tables_fc32.c

        fft/float/dsps_fht2r_fc32_ansi.c
        fft/float/dsps_fht4r_fc32_ansi.c

        windows/hann/float/dsps_wind_hann_f32.c
        windows/blackman/float/dsps_wind_blackman_f32.c
        windows/blackman_harris/float/dsps_wind_blackman_harris_f32.c
        windows/blackman_nuttall/float/dsps_wind_blackman_nuttall_f32.c
        windows/nuttall/float/dsps_wind_nuttall_f32.c
        windows/flat_top/float/dsps_wind_flat_top_f32.c
)
