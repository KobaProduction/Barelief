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
оптимизацию. `PORTABLE_DSP_MAX_FFT_SIZE` выбирает capacity 512/1024/2048/4096.
Kernel FFT поддерживает power-of-two размеры от 4; ограничения runtime-графа
могут быть строже. Mel принимает до capacity/2 bins и до 50 bands.

`workspace_bytes()` сообщает retained instance capacity. Старый `estimate_memory`
в `dsp.h` оценивает определённый stereo processing set; его результат нельзя
принимать за полный бюджет mono-графа, allocator, объектов, stack и runtime.
Сборка без exceptions/RTTI не меняет платформенную политику нехватки heap.

## Численный смысл

FFT публикует bins `[0,N/2)`, включая DC и исключая Nyquist, с magnitude
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

`PcmResampler` поддерживает Fs 8–192 kHz и отношения 1/8…8. Для разных Fs
используются causal 128-tap Hann-windowed sinc, 128 fractional phases и cutoff
`0.45 * min(input_rate, output_rate)`. Фильтр задерживает содержимое на 63 входных
отсчёта; runtime учитывает задержку отдельно от delivery time. При равных Fs
действует точный identity без filter delay. Это явная конверсия сетки отсчётов,
а не ASRC или синхронизация независимых устройств.
