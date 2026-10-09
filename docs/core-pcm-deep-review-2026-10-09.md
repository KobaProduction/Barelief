# Углублённое ревью PCM и цепочек преобразования

Это исходный отчёт до исправлений; текущее закрытие findings и результаты
проверок описаны в [отчёте исправлений](core-fixes-final-2026-10-09.md).

9 октября 2026 года. Продолжение [общего анализа](core-analysis-2026-10-09.md)
и [ревью непрерывности/архитектуры](core-continuous-architecture-review-2026-10-09.md).
Код не изменён; shared CMake/Ninja, GUI, аудиоустройства и firmware не запускались.

Итог трёх направлений ревью: **шесть новых семейств дефектов**. Четыре
воспроизведены на текущих portable APIs, одно — compiled clock arithmetic,
одно — extracted queue helper с source trace до DSP. Это разные уровни evidence;
ни один не выдаётся за многосуточный full-app или hardware прогон.

## Уточнённое требование

Core service действует от запуска до shutdown. Отключение модуля означает
пропуск его вычислительной работы; это не global Stop. Отсутствие сети прекращает
только отправку, сохраняя acquisition, DSP, накопление и время сигнала.
GUI запрашивает/рисует результаты с собственной частотой.

Расписание ядра задаётся временным интервалом исполнения, например 1/60 секунды
по умолчанию, а не зашитым divisor 60. Для дискретного сигнала сроки переводятся
в sample positions через его реальные часы и накопитель дробного остатка.
Величина «итераций в секунду» — обратная временному интервалу; она не является
sample rate или частотной осью FFT. Сохранённые кадры, координаты и History
привязаны к реальным timestamps, а не к числу вызовов renderer.

Текущий enabled=false означает **typed silence**, причём Window продолжает
накапливать PCM, History — добавлять строки/count. Это не requested skip mode.
Следует отдельно определить intake/scheduling state и вычисление операции:
пропуск вычисления не должен незаметно замораживать время и затем собирать окно
из старых и новых отсчётов.

Identity bypass возможен при совместимых входном и выходном контрактах.
PCM→Window, FFT, Spectrum→Mel и Vector→History меняют смысл/форму данных:
пересылка входных bytes под прежним output contract недопустима. Возможны
явное переподготовление маршрута с новым контрактом или declared skipped output
(typed silence/unavailable). Выбор для этих операций ещё не зафиксирован
пользователем; код реализации на этой неопределённости не строился.

## Карта проверенной цепочки

| Этап | Что проверено | Результат/ограничение |
|---|---|---|
| Capture/TestSignal → PCM | sample clock, timestamps, producer sequence, bounded buffering | Long-run TestSignal overflow; capture/receive дополнительно ревьюированы |
| PCM representation → Converter | signed/unsigned, packed24, endian, FloatUnit, staging | PcmNormalize и native DSP не согласованы |
| Routes/InputSelector | выбранный вход, clocks, discontinuity, known silence | Новое bypass требование отличается от действующего enabled |
| Window | rolling samples, hop phase, block boundaries, startup/gaps | Известные continuity defects; startup zero-padding требует явного описания |
| WindowFunction | собственный output, формула, window geometry | Не мутирует общий ingress при соблюдении synchronous lifetime |
| FFT | DFT normalization, Float32 intermediates, typed/plain API | Reproduced high-range overflow и success с NaN |
| Bark/Merge/Gain | реальные Hz, порядок операций, finite/overflow | Reproduced false overflow Mean/RMS; Merge metadata bug ранее |
| Mel | regular/explicit Hz, triangles, magnitude/energy | Необходим единый mathematical specification/oracle |
| Reduce/Converter | aggregate/transaction staging, precision policies | Есть независимые bit oracles; сохранять declared exactness |
| History/gather | ring/ordinal/coordinates/owning rows/masks | Известные batch silence/rebase/helper defects не закрыты |
| Delivery/DATA/remote → PCM | owning lifetime, coalescing, gaps, reconstruction | Loss-sensitive stream требует FIFO на обеих сторонах |

Это покрытие ревью и адресных probes, а не доказательство отсутствия остальных
дефектов для всех комбинаций графов, dtypes, strides, backends и устройств.

## Новые воспроизведённые дефекты

### P2 — PcmNormalize не соединяется с native DSP

`libs/dsp_kernels/src/numeric_convert.cpp:103–105` требует FloatUnit у результата
PcmNormalize. `libs/dataflow_runtime/src/runtime.cpp:471–472` требует None у
native DSP input, включая Window. Обычная identity conversion также запрещает
смену normalization (`numeric_convert.cpp:109`).

Fresh граф: Int16 PCM NetworkInput → Converter(PcmNormalize) → Float32 Window512
при 48 kHz. Выходной Window contract сохраняет FloatUnit, edge согласован.

```text
normalize only prepare0 bundle1
normalize->Window prepare2 bundle0
```

Status2=invalid_argument. Integer PCM normalization отдельно доступна, но
штатный анализ её результата не admitted. Особенно важно для I2S/device integer
PCM. Нужен единый canonical смысл normalized Float32 PCM и согласование всей
Window→WindowFunction→FFT цепочки. Spectrum normalization определять отдельно;
удаление всех checks или ручное переименование metadata не исправляет контракт.

### P2 — Merge Mean/RMS переполняют промежуточное значение

`runtime.cpp:1042–1045`: Mean считает `(left+right)*.5f`, RMS —
`hypot(left,right)/sqrt(2.f)`. При обеих stereo planes равных FLT_MAX правильный
результат обоих режимов равен FLT_MAX и представим. Input finite и admitted.

```text
merge0 status6 value=0 expected=3.40282e+38
merge1 status0 value=3.40282e+38 expected=3.40282e+38
merge2 status6 value=0 expected=3.40282e+38
```

Status6=overflow. Maximum подтверждает корректность входа/пути, Mean/RMS
переполняются до нормирования. Направление: устойчивый midpoint и scaled RMS,
с oracles на maxima, разные большие значения, zero и minimum subnormals.
Наивное деление каждого operand пополам устраняет один overflow, но может
потерять subnormal точность; проверять весь declared rounding contract.

### P2 — FFT слишком поздно нормирует и plain API возвращает success с NaN

`libs/dsp_kernels/src/dsp.cpp:60–68/:77–86` выполняет unscaled Float32 FFT,
а затем делит magnitude на N. Для constant FLT_MAX, N=512 правильный
нормированный DC равен FLT_MAX. Intermediate butterflies переполняются.

```text
fft prepare=1
plainFFT success=1 dc=nan finite=0 expected=3.40282e+38
typedFFT status=6 expectedDC=3.40282e+38
```

Plain API вообще не проверяет finite result; typed API возвращает overflow при
математически представимом normalized output. Нужны согласованные success
semantics и scaling/range policy. Рассмотреть power-of-two block scaling;
проверять большие сигналы, subnormals и все supported N/backend. Простое `/N`
на входе не принимается без проверки потери маленьких значений.

### P1 — TestSignal clock переполняется при непрерывном запуске

`adapters/desktop/platform/pipeline/src/test_signal.cpp:97/:118` умножает uint64
sample offset на 1 000 000 000 перед делением на sample rate. Unsigned wrap
наступает около 18 446 744 074 samples: 4,448 суток при 48 kHz и 26,688 часа
при 192 kHz. Это не предел самого int64 timestamp.

Для первого блока240 после границы при 48 kHz:

```text
offset=18446744160
correct elapsed ns=384307170000000
wrapped elapsed ns=1797717
```

Timestamp идёт назад без discontinuity, а deadline относительно прежнего origin
становится прошлым: генератор перестаёт ждать и начинает быстро производить
блоки. Подтверждены исходник, compiled C++ arithmetic probe и независимая
точная модель modulo2^64 arithmetic;
приложение несколько суток не запускалось. Capture miniaudio уже использует
quotient/remainder, что избегает этого конкретного раннего overflow.
Нужно checked преобразование sample cursor в duration и long-run virtual-clock
oracle без реального многосуточного ожидания.

### P3 — fill_silence может перезаписать собственные metadata через alias

`libs/signal_contracts/src/data_frame.cpp:259/:264` заполняет writable storage,
не проверяя его пересечение с borrowed описателем planes. Correct PCM Vector
из трёх Int64/UInt64 samples с PlaneLayout offset0/stride8 размещённым в тех же
24 bytes первоначально проходит validate_mutable (`:193`).

```text
Int64 before=0 fill=0 after=2 offset=0 stride=0
UInt64 before=0 fill=4 after=5 offset=9223372036854775808 stride=8
```

Int64 zeros разрушают stride, helper возвращает success с невалидным view.
UInt64 midpoint меняет offset уже первой записью, после чего дальнейшая запись
возвращает buffer_too_small, оставив partial mutation. Это воспроизведённый
дефект публичного защитного helper, не доказанная сетевая уязвимость: prepared
runtime выделяет payload и metadata раздельно.

До первой записи отвергать overlap с используемыми contract/view/planes/
channel IDs/clock text/coordinates/ring mask. Gather уже применяет такую защиту
(`numeric_frame_gather.cpp:136–143`). Oracle: rejection без изменения ни одного
byte; обычная тишина и unsigned midpoint остаются правильными.

## P1 — потери PCM в сетевой доставке на приёмнике

Sender latest-value coalescing из предыдущего отчёта остаётся: ordinary
mailbox объединяет PCM даже для network taps и помечает gap. При каждом
incoming256 discontinuity Window с hop800 не получает возможности накопить окно.

Дополнительно на **принимающей стороне**
`adapters/desktop/include/control/numeric_remote_wire.h:243–250` заменяет
queued ordinary frame того же endpoint. В `numeric_remote_endpoint.cpp:192`
Replaced игнорируется; callbacks вызываются после `control.poll` (`:199–205`).
Если один poll декодирует несколько непрерывных PCM frames, ранний исчезает,
а surviving frame сохраняет discontinuity=false. Router (`numeric_routing_adapter.cpp:100–102`)
передаёт stamps без изменения. Runtime допускает возросший sequence; Window
может склеить несмежные samples без gap и получить искажённый spectrum/history.

Compiled probe извлечённой **без изменений** enqueue_delivery с минимальными DTO
дал Appended, затем Replaced, queue_size1, kept_sequence102/timestamp512,
discontinuity0 и ноль overflow warnings. Это queue/helper evidence и отдельная
source-traced цепочка, не codec/TCP/hardware reproduction. Проверять
network FIFO на обеих сторонах: порядок/полнота samples, timestamps и actual
overflow marker. GUI latest-value и stream delivery нельзя делать общей
неявной политикой.

## Накопление, мутация и математическая спецификация

По source trace Window владеет rolling PCM, WindowFunction/FFT/Gain/Mel пишут
свои outputs. Conversion staging отделяет input scan от output scatter,
reduction публикует scalar после успешного aggregate. При disjoint scratch
и соблюдении synchronous callback lifetime это защищает соседние branches и
обеспечивает atomic conversion output при rejection. Это не обещание общей
transaction rollback всего графа.

Два fresh helper probes дополнительно подтвердили хорошие свойства: malformed
packed24 padding у последнего sample отвергается gather без изменения output,
payload и metadata; retain_frame точно сохраняет UInt64 PCM 0, UINT64_MAX и
9007199254740993 независимо от последующего изменения donor. Через double
эти значения в проверенном ownership пути не проходят.

Source сейчас проверяет AnyIeee; native DSP позже — FiniteOnly. NaN может попасть
в Window и быть отвергнут следующей операцией. Требуется явно определённая
native PCM ingress policy reject/replace; не считать любое событие source
автоматически допустимым для всех DSP kernels.

Первое Window1024 при hop800 содержит 224 ведущих нуля. Это принятая кодом
startup convention, не автоматически defect. Зафиксировать: zero-padding или
wait-full-window; timestamp означает end/center/start; поведение после gap,
bypass и смены interval. Нельзя определять эти смыслы по renderer.

Для block partition oracle один заранее созданный sample vector разбивается
на 1/256/4096/random chunks с согласованным sample-time. Sequence-dependent
tone amplitude в canonical Feed делает сигнал различным при разбиении и
не подходит для такой проверки.

DFT output conventions должны быть явными: текущий magnitude делится на N,
возвращается N/2 bins без Nyquist, окно меняет coherent gain; это не автоматически
amplitude spectrum или PSD. Real DFT имеет Hermitian symmetry, а нормирование
и включение Nyquist — отдельные соглашения.
[Первичная спецификация FFTW real DFT](https://fftw.org/fftw3_doc/The-1d-Real_002ddata-DFT.html).

В STFT временной шаг определяется hop×sampling interval, не render FPS.
Это подтверждает временную модель исполнения, которую следует выразить в
portable policy с реальными timestamps.
[Документация SciPy ShortTimeFFT](https://docs.scipy.org/doc/scipy/reference/generated/scipy.signal.ShortTimeFFT.html).

Regular Mel использует округлённые integer-bin triangles, explicit Hertz —
continuous Hz triangles. Magnitude/power, filter normalization, Nyquist support,
empty bands и частотный origin/step требуют единого независимого oracle.
Изменение значений ради ожидаемого вида картинки не является спецификацией.

## Что уже покрывают tests, и чего их PASS не доказывает

`portable_dsp_tests.cpp:68–90` имеет independent direct DFT для 4/16/64 и
максимальных radix sizes — FFT не «совсем не проверена». Но high-range case выше
не покрыт. Mel reference в этом тесте повторяет boundary formula implementation;
characterization использует те же FFT/Mel kernels. Это полезно для operation
order/no-allocation, но не независимое доказательство Mel mathematical policy.

`pcm_conversion_tests` содержит bit-level oracles packed24, signed/unsigned,
midpoint silence, saturation/rejection, subnormal rounding и atomic output.
`numeric_conversion_tests` покрывает 100 dtype pairs и alias/staging.
Они не проверяют admitted end-to-end normalizer→Window→FFT contract.
Runtime cadence test проверяет 60 windows за секунду при нескольких sample rates;
не проверяет настраиваемый интервал и состояние при его смене.

Capture trace: miniaudio выдаёт фактически requested Float32 stereo (resampling
и channel mixing могут происходить в device adapter до portable graph), SPSC
CaptureBuffer сохраняет interleaved PCM/timestamp/gap через release/acquire,
worker создаёт owning left/right. Engine собирает prepared blocks и синхронно
передаёт borrowed bytes executor. Callback retention предшествует следующему
decode; borrowed codec data напрямую в async queue не остаётся. Channel-order
или planar/interleaved corruption этим pass не обнаружены. Adapter copies здесь
явные и не входят в обещание zero-copy portable processing.

Полезные regression направления: whole-chain normalization, equal-extreme Merge,
scaled FFT, long-run clock, фиксированный PCM partition invariance, sender и
receiver burst FIFO, gaps/bypass/resume, History timestamps/ordinals и регулярные
против explicit coordinates. После integration — независимый review и bounded
host/MCU checks; historical PASS не закрывает новые условия.

## Evidence и воспроизводимость

Новые numerical/API материалы:
`C:/Users/Koba/AppData/Local/Temp/qt-core-review-deep/`:
probe.cpp/build.ps1/probe.exe/result.log и fft.cpp/fft.exe/fft.log.
Compiler MinGW g++13.1, C++20, O0, static. Runtime, domain, contracts/gather/pool,
DSP/convert/reduce/scalar/backend и WLED скомпилированы из текущих sources.
Единственный existing archive — vendor FFT `cmake-build-release/lib/libdsp.a`.
Stale runtime/domain/contracts/kernel archives не использованы.
Координатор прочитал logs и фактический build.ps1; shared build не выполнялся.

Helper evidence: `C:/Users/Koba/AppData/Local/Temp/qt-pcm-helper-audit-20261009/`
(probe.cpp/probe.log/source-hashes.json): fill alias, gather atomicity,
UInt64 ownership. Helpers скомпилированы из актуальных sources.
Clock/queue evidence:
`C:/Users/Koba/AppData/Local/Temp/qt-pcm-clock-review-5cb9e61577c043278d13460c0a5a2e7d/`
(clock_probe.cpp/.exe, receive_queue_probe.cpp/.exe), оба compile/probe exit0.

SHA256 проверенного снимка:

```text
runtime.cpp DCB5B72DB0A237E576F624EF3F56282760B04AFCFF9AD208820E56929719177A
dsp.cpp 3AF8363F019B855C8A1F3E002F37E8BA5217F9F73B188F8BD7196DDD8AC02B62
numeric_convert.cpp 760836367E84F47A54AE8C1F6D660AEE50891B4354E9E08D121356F2EC811F49
numeric_reduce.cpp E7DC71E754B50E737D610CA2A46743EB53B0CFD8DB6DF8A0C5F7168DFF326F6B
processing_contracts.cpp F585DCB7CA7533616B42AB628207D1D16559362ED6D9EE9ECD293B5DFD27649A
data_frame.cpp 571D0F12EBF15F1CB12F1194A60662A68AB40C5EBE11BDADDCEF326876677EA8
numeric_frame_gather.cpp 324EC57A48AD6492E8BCB96B35D9E359233B54573E186D1EE04310CC5E833DD0
test_signal.cpp 76F90179DA941787EEC1B0E68C5EA378102590632BE623F067DAFDE089AF4147
numeric_remote_wire.h D1AFF290D12ED3BCE6FCA95F420A07DD199E66B43527F76C56CCDA20AE5FE73A
numeric_remote_endpoint.cpp 7FF877409F803D611BF62657B41B9DC39203FF7CB641DCD8897B5B1E0418FEA9
```

Source-traced delivery и exact arithmetic clock model отделены от фактических
numerical probes. Полный suite, real TCP load, native GUI, WCET и hardware MCU
приёмка данным ревью не выполнялись.

После probes внешний чат изменил runtime.cpp: финальная сверка дала SHA256
`22DED5201E1574748A709FA64C00D0A2478E9FAAF68A40A97B0800A5EFEB9A9F`.
Координатор source-only подтвердил неизменность normalization guard :471–472,
Merge arithmetic :1042–1045 и Window fixed cadence :977–978. Остальные основные
numerical/helper/clock/wire files в финальной сверке сохранили hashes выше.
Dynamic probe evidence относится к DCB5... снимку, не к новой полной редакции.
