# Спектральные представления и калибровки

`FFT` и `PreparedRfft` вычисляют ненормированное ДПФ уже оконного входа.
`RealFourier → AmplitudePhase` использует ту же подготовленную арифметику
проекций, что и `RFFT → SpectrumProjection`. Применение окна — отдельная
операция; выбор окна в SpectrumProjection задаёт калибровку и не умножает PCM.

Обозначения: `X[k]=DFT(x[n]w[n])`, `C=Σw`, `E=Σw²`, `Δf=Fs/N`.
Для одностороннего спектра `α=2` во внутренних бинах, `α=1` в DC/Nyquist.
Полный спектр использует `α=1`: сопряжённые частоты присутствуют раздельно.

| Прежнее название / ключ | Формула при компенсации | Название | Контракт / реализация |
| --- | --- | --- | --- |
| Magnitude, `magnitude`, без компенсации | `abs(X)` | Raw FFT magnitude | SpectrumMagnitude, RawAmplitude; реализовано |
| Magnitude с компенсацией; `window_scaled_magnitude` | `abs(X)/C` | Window-scaled magnitude | SpectrumWindowScaledMagnitude, RawAmplitude; реализовано |
| `peak_amplitude` | `α abs(X)/C` | Peak amplitude | SpectrumPeakAmplitude, RawAmplitude; сохранён смысл |
| `rms_amplitude` | `sqrt(α) abs(X)/C` | RMS amplitude | SpectrumRmsAmplitude, RawAmplitude; реализовано |
| Power, `power` | `α abs(X)²/C²` | Power spectrum | SpectrumPower, SquaredAmplitude; сохранён смысл |
| PSD, `psd` | `α abs(X)²/(Fs E)` | Power spectral density | SpectrumPsd, SquaredAmplitudePerHertz; сохранён смысл |
| `bin_power` | `α abs(X)²/(N E)` | Bin power | SpectrumBinPower, SquaredAmplitude; реализовано |

`Power` соответствует SciPy `scaling="spectrum"`, PSD — `scaling="density"`.
`ENBW=Fs E/C²`, поэтому `Power=PSD*ENBW`; `bin_power=PSD*Δf`.
Парсеваль даёт `Σbin_power = Σ(w²x²)/E`. Сумма Power отличается множителем
`N E/C²` и для неплоского окна не является мощностью всего исходного PCM.
Мощность бина не называется энергией: временная нормировка здесь не введена.
Для симметрического Hann длины M эквивалентная шумовая полоса равна
`1.5 Fs/(M−1)`, а периодический Hann даёт `1.5 Fs/M`. Исследовательский пример
использует фактические C/E, без подстановки периодической формулы.

RMS amplitude — корень из Power. Для изолированной биновой синусоиды,
когда сопряжённый образ и соседние компоненты не вносят вклад, это пиковая
амплитуда, делённая на sqrt(2). Для DC/Nyquist деления на sqrt(2) нет.
Вне бина, при утечке и коротком окне пик одного бина не равен точной амплитуде
всего сигнала. RMS amplitude — величина на бин, не RMS исходной музыки.

## Параметры и совместимость

Машинные ключи `coherent_gain` и `power_gain` сохраняются. Они передают
**суммы C и E**, а математические нормированные коэффициенты окна равны
`C/M` и `E/M`, где M — число коэффициентов окна. `WindowGain` и два
скалярных выхода WindowFunction публикуют суммы. `window_coherent_gain` и
`window_power_gain` возвращают нормированные средние; смешивать эти API нельзя.

`SpectrumProjection/magnitude` исторически означает сырой модуль.
`AmplitudePhase/magnitude` при `normalize=true` исторически означает `abs(X)/C`;
его значения сохранены, выход получает уточнённый Quantity.
Явный `window_scaled_magnitude` позволяет выбрать этот смысл в обеих схемах.
Для `AmplitudePhase` отключённая компенсация по-прежнему даёт `abs(X)` в
амплитудных режимах и `abs(X)²` в power/PSD/bin-power, без одностороннего
удвоения. Контракт тогда описывает сырой модуль либо его квадрат, а не PSD.

Для сохранённых потребителей `input_representation=magnitude` принимается
также Quantity калиброванного модуля. В NoteSalience его уже выполненная
компенсация выражается явным `coherent_gain=1`; скрытого извлечения C из
чужого кадра нет. Новый режим `window_scaled_magnitude` не делит на C повторно.
`rms_amplitude` преобразуется в тональную мощность возведением в квадрат;
`bin_power` — умножением на `ENBW/Δf`; PSD — на ENBW.
Все параметры этих преобразований задаются явно.

BroadbandFeatures принимает Power и BinPower без скрытого пересчёта.
Амплитудные величины он отклоняет: для мощности нужна явная другая проекция.
Его исторический ключ `sum_energy` сохранён как alias суммы входных значений
мощности; название интерфейса — «Сумма мощности бинов».
BroadbandRow/NoteSpectrum сохраняют размерности выбранного входа.
FrequencyNormalizer умножает линейные значения на заданные коэффициенты,
сохраняя Quantity/единицы. Алгоритм нормализатора музыкального корпуса
не изменяется; его текущая модель сканирования остаётся peak-amplitude.

Числовые ID SpectrumProjection 0…6 и Quantity 0…21 не изменены.
RMS, BinPower, WindowScaledMagnitude добавлены в конец: соответственно
7/8/9 и 22/23/24. JSON сохраняет прежние ключи/структуру. DATA-кодек
использует новые ID без перенумерации старых; неизвестный ID отклоняется.
Старый peer, не знающий новые величины, не обязан их принимать.

## Окна, дополнение нулями и пределы

Существующие Hann, Hamming, Blackman, Blackman–Harris, Bartlett и Flat Top
симметричны: знаменатель фазы M−1; Rectangular равен 1. Формулы сохранены.
Для сравнения с SciPy нужен `get_window(..., fftbins=False)` или точный
массив коэффициентов. Периодическое окно с знаменателем M — другое окно;
оно не подменяет существующее симметрическое. В DSP caller может передать
его фактические C/E явно, но отдельного выбора periodic в редакторе нет.

При M<N сначала применяют окно длины M, затем дополняют PCM и окно нулями
до N. C/E относятся к реально применённому окну, Δf — к N.
В PreparedSpectrumProjection передают WindowScaling с `samples=N` и этими
суммами; вычисление окна сразу длины N меняет задачу. Дополнение нулями
уплотняет сетку, не повышает физическое частотное разрешение.

Re/Im, проекции и результаты — Float32; промежуточные масштабы, квадраты
и суммы окна — double. Подготовка выделяет буферы, processing их не расширяет.
NaN/Inf, неверная геометрия и непредставимые результаты отклоняются;
PreparedSpectrumProjection и PreparedSpectrumPower сохраняют прежний выход
при отказе. При Float32-underflow очень малые значения могут стать нулём.
Калибровки, чьи промежуточные масштабы не представимы в double, не поддержаны.

## Воспроизводимая независимая проверка

После сборки `rfft_tests` запустите `tests/spectral_projection_scipy_tests.py`
в Python с NumPy/SciPy, задав `--binary` и собственный `--output`.
Экспорт C++ содержит точные Float32-сигнал, окно, оконный PCM и проекции.
Оракул использует независимый FFT NumPy и SciPy periodogram с
`detrend=False`, `return_onesided=True`, явным nfft и точным массивом окна.
Матрица включает 7 окон, Fs 32768/44100/48000, M=63/64, N=64/256,
тишину, DC, Nyquist, импульс, биновый/межбиновый тон, шум и смесь 7 тонов.
Проверяются ENBW, Δf, Парсеваль, RMS²=Power и оба состояния компенсации.
Порог сравнения — `8e-5*abs(reference)+4e-6*max(abs(reference))`;
для Re/Im абсолютный масштаб берётся по модулю комплексного спектра;
фаза сравнивается по кругу с точностью .001 рад в бинах выше −60 дБ
относительно пика. Для всех бинов отдельно требуется совпадение с atan2
фактических Re/Im до 5e-7 рад. У слабых бинов фаза чувствительна к абсолютной
ошибке FFT: малый допуск по амплитуде не обещает малого допуска по фазе.
Нулевой сигнал требует нулевого результата.

Первоисточники:

- [SciPy periodogram](https://docs.scipy.org/doc/scipy/reference/generated/scipy.signal.periodogram.html),
  [Welch](https://docs.scipy.org/doc/scipy/reference/generated/scipy.signal.welch.html): spectrum/density и ENBW.
- [MATLAB periodogram](https://www.mathworks.com/help/signal/ref/periodogram.html): power/psd и односторонний спектр.
- [SciPy ShortTimeFFT](https://docs.scipy.org/doc/scipy/reference/generated/scipy.signal.ShortTimeFFT.html): раздельные масштабы амплитуды/PSD.
- [SciPy Hann](https://docs.scipy.org/doc/scipy/reference/generated/scipy.signal.windows.hann.html): symmetric/periodic.
- [J. O. Smith, Spectral Audio Signal Processing](https://www.dsprelated.com/freebooks/sasp/): оконный спектральный анализ и дополнение нулями.
