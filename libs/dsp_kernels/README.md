# Численные операции DSP

[Переносимое ядро](../../docs/architecture/portable-core.md) · [SDK guide](../../docs/reference/embedded-sdk.md)

C++20 target `dsp_kernels` предоставляет численные операции поверх
`signal_contracts`, без Qt, JSON, domain, устройств и сети. Installed target —
`PortableDSP::dsp_kernels`. `portable_dsp` остаётся forwarding CMake target,
а `dsp_core/dsp.h` — forwarding header; отдельной реализации за ними нет.

| Публичный заголовок | Назначение |
| --- | --- |
| [dsp.h](include/dsp_kernels/dsp.h) | Оконные функции, Bark weights, подготовленные `FFT` и `MelFilterBank` |
| [oscillator_bank.h](include/dsp_kernels/oscillator_bank.h) | До 128 осцилляторов с частотой, амплитудой, фазой и линейными attack/release |
| [pcm_resampler.h](include/dsp_kernels/pcm_resampler.h) | Подготовленная потоковая конверсия Fs Float32 PCM |
| [numeric_convert.h](include/dsp_kernels/numeric_convert.h) | Преобразования primitive dtype и storage с явной numerical policy |
| [numeric_reduce.h](include/dsp_kernels/numeric_reduce.h) | Числовые редукции |
| [kernel_support.h](include/dsp_kernels/kernel_support.h) | Проверка поддержанных контрактов и потребности workspace |

Prepare выделяет instance workspace и инициализирует shared backend tables.
Processing не выделяет память и не берёт mutex инициализации таблиц. Один
instance принадлежит одному processing owner. Нельзя вызывать vendor init/deinit
параллельно операциям или освобождать таблицы, пока существуют prepared plans.
Недостаточные внешние таблицы отвергаются, а не перестраиваются скрыто.

## Backend и подключение

`DSP_KERNELS_BACKEND=ANSI` по умолчанию использует bundled ESP-DSP ANSI sources
с host simulation headers. `ESP_DSP` требует уже существующий внешний target
`idf::esp-dsp` либо имя из `DSP_KERNELS_ESP_DSP_TARGET`; установка SDK не выполняется.
`compiled_backend()` сообщает выбранную интеграцию, а не измеренную аппаратную
оптимизацию. `PORTABLE_DSP_MAX_FFT_SIZE` selects compiled capacity; desktop supports up to16384, while the embedded device profile can remain4096.
Kernel FFT поддерживает power-of-two размеры от 4; ограничения runtime-графа
могут быть строже. Mel принимает до capacity/2 bins и до 50 bands.

`workspace_bytes()` сообщает retained instance capacity. Старый `estimate_memory`
в `dsp.h` оценивает определённый stereo processing set; его результат нельзя
принимать за полный бюджет mono-графа, allocator, объектов, stack и runtime.
Сборка без exceptions/RTTI не меняет платформенную политику нехватки heap.

## Численный смысл

MagnitudeSpectrum публикует bins `[0,N/2)`, включая DC и исключая Nyquist, с magnitude
`abs(DFT)/N`, без one-sided doubling. Нечисленные входы/результаты отвергаются.
Window gain не компенсируется автоматически.

Mel использует непрерывные треугольники в Hertz, границы равномерны в Mel:

```text
f[i] = 700 * ((1 + fs/1400)^(i/(M+1)) - 1), i=0..M+1
w[m,k] = max(0, min((k*fs/N-f[m])/(f[m+1]-f[m]),
                   (f[m+2]-k*fs/N)/(f[m+2]-f[m+1])))
out[m] = sum_k input_magnitude[k] * w[m,k]
```

Стабильный идентификатор `MelEnergy` означает здесь взвешенную линейную
magnitude: нет возведения в квадрат, dB, area normalization или window
compensation. Удвоение magnitude удваивает результат. Номер полосы не равен Hz;
сравнение разных FFT sizes и band counts не является калиброванным PSD.

PreparedResampler uses the explicitly selected policy and backend port.
Existing/Compact FIR share the kernel with B=64/16 and nominal input delay
ceil(B*max(Fs_in,Fs_out)/Fs_out). Taps and phase count depend on the ratio.
Equal rates use an exact identity without delay. Content delay is separate from
output timestamps. See ../../docs/reference/resampling.md for modes and EOF drain.

Raw PreparedRfft returns interleaved Re/Im bins 0..N/2, including DC/Nyquist,
without division by N. PreparedSpectrumScaling/Projection apply explicitly
selected raw/window-scaled magnitude, peak/RMS amplitude, power spectrum,
PSD or bin power and sums C=sum(w), E=sum(w*w).
Rectangular C=E=N. Logarithmic display belongs to the audio presenter.
The stable coherent_gain/power_gain parameter names carry C/E, not C/N and E/N.
See ../../docs/reference/spectral-projections.md for formulas, units, legacy
aliases, zero padding and the independently executable SciPy oracle.

MelFilterBank shares prepared boundaries for canonical and explicit Hz grids.
spectral_weights.h owns common Mel/Bark/triangle math. sample_cadence.h shares
an integer phase accumulator between live and offline window deadlines.

FrequencyProfile declares channel mean/select, Fs/N/window, C/E, full DC/Nyquist
layout, representation and resampler policy. C/E=0 derives sums from the declared
window/N, without upstream discovery. Runtime consumes typed gains arrays.
Profile JSON and file decoding belong to desktop adapters.
