# D2: numerical reduction / WLED codec handoff, slice 2

Дата: 2026-10-06 (Europe/Moscow). Статус: **D2 source frozen / author-tested, integration and independent review pending**.
GATE 1 принят; координатор разрешил реализацию D2 после decoder policy проверки.
HEAD `b18cfbf3c2ac62b4baf022de5348f5a29d46e2cc` подтверждён.
Dirty drafts не приняты автоматически. Этот отчёт не объявляет acceptance.

Прочитаны AGENTS.md, architecture-map.md, typed-node-routing.md,
implementation-plan.md, node-runtime-contracts.md, storage-control-contracts.md,
ProposedSlice2 в baseline-and-ownership.md и полный
[закреплённый Clean Architecture / DDD](https://github.com/ArthurKoba/ai-agent-workflow/blob/fdb39e72f330b5e6622895cc31ca8e7b73699629/skills/software-engineering/clean-architecture-ddd.md).
Implementation/review handoff обязан передавать эту ссылку и требование полного
чтения. Справочник не задаёт backlog; stable IDs/wire остаются прежними.

## Ownership и зависимости

| Владелец | Пути / обязанности |
| --- | --- |
| D2 | Этот документ; будущие `libs/dsp_kernels/include/dsp_kernels/scalar_reduce.h`, `libs/dsp_kernels/src/scalar_reduce.cpp`, `tests/scalar_reduce_tests.cpp` |
| D2 | Будущие `libs/wled_protocol/include/wled_protocol/wled_packet.h`, `libs/wled_protocol/src/wled_packet.cpp`, `tests/wled_protocol_tests.cpp`; старый `libs/bdsp_protocol/include/device_protocols/wled_packet.h` только extraction/forwarding |
| R2 | Runtime/frame validation/state/callbacks/prepared packet storage; выбирает reduction population и внедряет helper calls; D2 не пишет runtime.cpp или signal_contracts |
| T2 | Desktop/BDSP typed delivery, packet destination/counter/cadence/stale, owned async bytes, legacy bridge; D2 не пишет эти файлы |
| I | Все CMake/manifests/export/install/ESP source lists, roots, targets/test registrations; D2 передаёт список, не редактирует |

Оба helper headers/sources зависят только от стандартного C++20. Нет NodeKind,
FrameView, JSON, Qt, miniaudio, UDP/OS, ESP-IDF, BDSP, RTTI/exceptions и heap.
`dsp.h`/`dsp.cpp` менять не требуется: отдельный public header в existing
`dsp_kernels`, без новой reduction library. WLED wire принадлежит отдельному
`wled_protocol`; generic Signal/BDSP не разбирают его поля. Runtime зависит от
двух pure helpers; application/domain не зависят от device codec.

## Exact proposed numerical API

```cpp
namespace dsp_kernels {
enum class ScalarReduction { MaximumAbsolute, SignedMean, Rms };
enum class ScalarReduceStatus {
    Ok, InvalidOperation, EmptyInput, LimitExceeded,
    NonFiniteInput, NonFiniteResult
};
struct ScalarReduceLimits { std::size_t max_values; };
ScalarReduceStatus scalar_reduce(
    ScalarReduction operation,
    std::span<const float> first, std::span<const float> second,
    ScalarReduceLimits limits, float& output) noexcept;
}
```

Limits обязательны, без desktop defaults; ноль не допускает непустой population.
Checked `first.size()+second.size()` до сканирования; переполнение или превышение
max_values = LimitExceeded. Invalid enum = InvalidOperation. Пустые оба spans =
EmptyInput, не доступный scalar zero. Nonfinite input = NonFiniteInput.
На любой ошибке output побитно неизменён; inputs всегда read-only. NaN/Inf output
не публикуется. Возвращается finite float после double accumulation/финальной
проверки; maxabs=max(abs(x)), mean=sum(x)/N, RMS=sqrt(sum(double(x)*x)/N).
Никакой нормализации отрицательного mean и никакого усреднения channel means.
Порядок accumulation first→second фиксирован для characterization; exact numeric
bit equality не обещается между разными math backends, fixtures имеют tolerance.
O(N), O(1) scratch; обе формулы sum используют double, включая промежуточные
products. Физический span обязан ссылаться на доступные caller-owned элементы.

R2 явно отображает stable domain Reduction на этот enum (не static_cast ABI).
Stereo → left/right; vector/scalar → values/empty; History → newest logical row
с учётом newest_override/empty. Helper не проверяет совпадение stereo длины,
frame semantics или кольцевую geometry: это R2. Packet не допускается R2.
Нет History row → unavailable; known zero row → Ok с 0; первый scalar 0 доступен.
R2 задаёт max_values из actual prepared geometry/effective budget, учитывая оба
канала (не один channel limit), и переводит errors в runtime unavailable/status
без stale scalar. Ownership latest History/both channels согласован с R2.

## Exact proposed WLED API

```cpp
namespace wled_protocol {
inline constexpr std::size_t packet_size = 44;
inline constexpr std::size_t bin_count = 16;
inline constexpr std::size_t formatted_mel_count = 50;
inline constexpr std::size_t counter_offset = 17;
using WledPacket = std::array<std::uint8_t, packet_size>;
enum class Status {
    Ok, BufferTooSmall, InvalidSize, LimitExceeded, InvalidHeader,
    NonFiniteInput, InvalidArgument, NonFiniteResult, InvalidField
};
struct Limits {
    std::size_t max_mel_values;
    std::size_t max_packet_bytes;
};
struct Result { Status status; std::size_t used; std::size_t required; };
struct DecodedPacket {
    std::array<std::uint8_t, 2> pressure;
    float sample_raw, sample_smoothed;
    std::uint8_t sample_peak; // wire byte; >0 means detected, preserve value
    std::uint8_t counter;
    std::array<std::uint8_t, bin_count> bins;
    std::uint16_t zero_crossing_count;
    float magnitude, peak_hz;
};
Result encode_bins(std::span<const float> bins, float peak_hz,
    float magnitude, std::uint8_t counter,
    std::span<std::uint8_t> destination, Limits limits) noexcept;
Result encode_mel(std::span<const float> mel, float peak_hz,
    float magnitude, float gain, std::uint8_t counter,
    std::span<std::uint8_t> destination, Limits limits) noexcept;
Result silence(std::uint8_t counter,
    std::span<std::uint8_t> destination, Limits limits) noexcept;
Result decode(std::span<const std::uint8_t> source,
    DecodedPacket& output, Limits limits) noexcept;
Status validate_packet(std::span<const std::uint8_t> source,
    Limits limits) noexcept;
Status write_counter(std::span<std::uint8_t> owned_packet,
    std::uint8_t counter, Limits limits) noexcept;
namespace legacy {
WledPacket encode_wled(std::span<const float> mel, float peak_hz,
    float magnitude, float gain, std::uint8_t counter) noexcept;
}
}
```

Все limits обязательны. max_packet_bytes<44 = LimitExceeded, без defaults SDK.
Encode destination>=44; пишет только первые44, suffix остаётся неизменным.
Недостаточная ёмкость = BufferTooSmall. Decode/validate/write_counter требуют
ровно44; truncated или trailing bytes = InvalidSize. На encode/decode успехе
used=44, required=44; на ошибке used=0, required=44. Decode сначала валидирует
локальный DTO, только затем commits output; destination/DTO на ошибке неизменны.
Encode также собирает fixed local packet до commit; source/destination overlap
не требует heap и не приводит к partial mutation. write_counter полностью
валидирует packet до единственного изменения byte17; остальные43 bytes сохранены.
validate/decode read-only и не модифицируют producer payload или node params.

encode_bins — явно legacy sanitation API: bins.size()<=16, missing bins=0;
finite clamp [0,1], nonfinite→0; peak_hz/magnitude nonfinite→0, negative→0.
Сохраняет make_packet HEAD bytes. Более16 = InvalidSize; forwarding wrapper
обрезает span до16, сохраняя старую игнорируемую tail semantics. Mel limit к
этому16-bin low-level path не применяется; лимит16 задан форматом.

encode_mel — typed API: непустой mel, size<=max_mel_values; empty=InvalidArgument,
oversize=LimitExceeded; все элементы/metadata/gain finite, gain в [0,100],
peak_hz/magnitude>=0. Negative finite Mel разрешён (legacy floor при mapping).
Nonfinite=NonFiniteInput; finite invalid arguments=InvalidArgument.
Перед потенциальным float→index cast проверяются размер/вычисленные координаты;
max_mel_values не заменяет checked arithmetic. Сохраняется finite draft mapping:
50 interpolated values, centre=clamp((i+1)*(mel.size()+1)/51-1,0,size-1), затем
42 values formatted[4..45] усредняются по floor(i*42/16)..floor((i+1)*42/16)-1,
negative values floor0, gain до mapping, clamp только при bins encode. Products/
sums проверяются до float narrowing; overflow=NonFiniteResult, не silent NaN.
max_mel_values передаёт R2 из prepared input.columns/effective limits; source
Mel не ограничивается50: NetworkInput может иметь больше полос. Число50 относится
только к formatted scratch. Порядок float interpolation/bin sums сохраняется
для legacy finite byte parity; overflow проверяется без скрытой смены rounding.
Private representability guard max source `(1<<24)-1` обеспечивает безопасный
float-coordinate→size_t cast на32/64-bit targets; сверх guard LimitExceeded до
доступа к payload. Это предел арифметики, не50-band device restriction. Actual
prepared input/effective limits обычно существенно меньше.
Scratch фиксирован: 50+16 floats и44-byte staging packet; дополнительных prepared
heap buffers codec не требует. Редукция/output scratch живёт у R2, не в codec.
Legacy wrapper encode_wled сохраняет empty→valid silence и sanitation своего
исторического permissive entrypoint; не подменяет typed strict encode_mel.
Нестандартные finite Mel sizes сначала характеризуются из draft отдельно от HEAD.

silence не вызывает empty typed encode: header валиден, все signal fields нулевые,
counter ровно caller argument. **44 нулевых байта не являются packet silence.**

## Wire layout / decoder bounds

| Offset | Bytes / смысл |
| --- | --- |
| 0..5 | `00002\0`, точные ASCII bytes |
| 6..7 | pressure bytes; encoder zero, decoder сохраняет |
| 8..11 / 12..15 | little-endian IEEE754 binary32 sample_raw/sample_smoothed; encoder одинаковые sum(clamped bins)/16*255 |
| 16 / 17 | peak byte: encoder0/1, decoder any uint8 (>0 means peak); counter unsigned0..255 |
| 18..33 | 16 truncated `uint8_t(clamped bin*255)` |
| 34..35 | little-endian uint16 zero_crossing_count; encoder zero, decoder сохраняет |
| 36..39 / 40..43 | little-endian binary32 magnitude>=0 / peak_hz>=0 (Hz) |

Без packed structs/host-endian reinterpretation. Требование платформы:
sizeof(float)==4 и numeric_limits<float>::is_iec559, checked compile-time.
Decoder требует точный header и finite значения всех четырёх float fields;
NaN/Inf=NonFiniteInput. Не ограничивает finite amplitudes диапазоном[0,255],
не отвергает отрицательные finite floats или peak byte>1. Encoder bounds не
становятся wire validation. InvalidField зарезервирован, текущий decode его
не возвращает. Pressure/zero_crossing не обязаны быть0: decoder не отвергает
другие валидные AudioSync producers. Не выводить frequency cap из44.1kHz;
23000Hz сохраняется. Не требовать одинаковых raw/smoothed в чужом packet.
counter не связан с metadata.sequence; decoder не проверяет progression.

### Primary-source decoder policy check, requested by coordinator

Проверены immutable revisions:
[WLED struct](https://github.com/wled/WLED/blob/8db3d7a82f307258c2a874a9c97e643adbcb3b64/usermods/audioreactive/audio_reactive.cpp#L836),
[producer](https://github.com/wled/WLED/blob/8db3d7a82f307258c2a874a9c97e643adbcb3b64/usermods/audioreactive/audio_reactive.cpp#L1239),
[receiver](https://github.com/wled/WLED/blob/8db3d7a82f307258c2a874a9c97e643adbcb3b64/usermods/audioreactive/audio_reactive.cpp#L1277),
и [MoonModules source, на котором основан local layout](https://github.com/MoonModules/WLED/blob/b17819e2cefe2d76389ad842223631a3f4e6bd43/usermods/audioreactive/audio_reactive.h#L1166).
WLED producer ограничивает AGC tmpAgc (1096) и nonAGC sampleAdj (1160) до255,
затем передаёт raw/smoothed floats. Receiver (1283–1284) применяет floor0 для
эффектов, без rejection >255; peak (1297) трактует любой ненулевой byte как true.
Структура (841) явно допускает peak>=1. MoonModules даёт те же float/peak semantics
(1169–1171,1711–1713,1777–1778,1791), плюс counter offset17.
Вывод:255 — producer policy, не доказанный invariant AudioSync v2. Pure decode
сохраняет все finite floats и исходный peak byte; display/effect clamps вне codec.
Стабильные local encoder bytes/санитация/counter не меняются. Дополнительные
fixtures: finite raw/smoothed256/1024/negative; peak2/255; magnitude/Hz negative;
decode preserves values, counter patch still validates finite/header/size.

## Counter / delivery handoff

HEAD legacy `_frame_counter++` происходит перед writeDatagram: первый counter0,
рост на каждой попытке (включая failed send), modulo256; timer17ms. Stale policy
HEAD `elapsed()>300` (не >=300); это источник characterization для T2.
Dirty prepared path отдаёт producer counter без замены — не authority и не parity.

Предложение R2/T2: граф encode_mel/silence использует нейтральный counter0;
T2 копирует packet в собственный44-byte send buffer, вызывает write_counter со
своим destination counter и увеличивает его на попытку. Borrowed runtime packet
не меняется, повторная отправка не запускает DSP. Codec не инкрементирует counter,
не содержит clock/timer/destination/retry/stale state. Stale/known unavailable
silence создаёт T2 через silence с актуальным send counter. Unknown frame статус
не маскируется в runtime silence; resource handling остаётся явным у T2.
Explicit Stop/device fencing/endpoint и failures полностью T2; GUI не управляет DSP.

## Characterization / TDD fixtures (после разрешения implementation)

1. До extraction сохранить независимые **literal44-byte goldens** HEAD
   make_packet: empty, mixed bins0/.5/1/negative/>1/NaN/Inf, truncated tail,
   >16 ignored tail, metadata negatives/NaN/Inf, 23000Hz, counters0/255.
   HEAD make_packet oracle воспроизводится из git show HEAD; draft helper не
   считается HEAD. Golden expected bytes не вычисляются тестируемым codec.
2. Отдельно характеризовать draft finite encode_wled interpolation для
   Mel1/3/16/50 и нестандартных current-compatible50 mapping; gain0/1/100.
   Sanitation/overflow различия фиксируются отдельно; строгая typed policy —
   новое поведение с failing tests, не молчаливая смена legacy fixtures.
3. Reduction red/green: negative maxabs/signed mean, two spans unequal lengths
   как numeric population, zero/first zero, RMS mixed signs, FLT_MAX products
   через double, nonfinite input, invalid enum, empty, exact/max exceeded budget,
   unchanged sentinel output. Allocation counter после setup, C++20 noRTTI/noexceptions.
4. WLED red/green: insufficient buffer/suffix/limits, atomic failures, typed
   NaN/Inf/gain/overflow, literal valid silence (header+counter), invalid allzero,
   decode truncated/trailing/badheader/nonfinite; accepts finite floats outside
   local encoder range and peak byte2/255 without changing their values,
   nonzero pressure/zeroCross allowed, readonly input/unchanged DTO on failure,
   write_counter only byte17/no change on invalid packet, noalloc.
5. R2 integration (его tests): both PCM channels vs values-only bug; History
   differing old rows/latest override; absent row unavailable; known scalar0;
   per-output packet ownership/valid silence, malformed packet rejected через
   codec, generic signal validator only opaque geometry. D2 их files не меняет.
6. T2 integration (его tests): old bytes, counter0/1/255/0, repeated prepared
   sends with own patched counter, original graph bytes unchanged, cadence17ms,
   stale boundary300/301, immediate typed silence/unavailable, failed send counter
   semantics, bounded ownership/Stop fence. Timing не проверяется codec unit test.

## I integration request / ограничения

После source freeze: добавить scalar_reduce.cpp в existing dsp_kernels; новый
std-only wled_protocol target + public include/export/install/same-source ESP;
runtime link wled_protocol, desktop WLED link wled_protocol; убрать runtime→BDSP
только после отсутствия других protocol usages. Forwarding public include
device_protocols/wled_packet.h сохраняется для старых callers, не содержит второй
wire implementation; compatibility symbols/names остаются. Link/include shim
может требовать transitional I dependency до миграции всех R2/T2 consumers.
Регистрация tests/scalar_reduce_tests.cpp и tests/wled_protocol_tests.cpp у I;
targets link dsp_kernels/wled_protocol соответственно, assertions enabled.
Final SDK consumer и whole desktop/DLL сборка/короткие affected integration у I,
после освобождения рабочего слота; этот документ их не подменяет.

На contract стадии own directory `build-refactor-device-slice2` ещё не был создан;
после разрешения implementation он используется для direct compiler evidence,
описанного ниже. Tools из existing caches, никаких installs/branches/commits/
push/PR/ZIP, config.json/output untouched. Static inspection выполнен
Get-Content/rg/git show/git status. Hardware WLED/ESP/native graphics не подтверждены.

R2 подтвердил op-first enum/local statuses/explicit limits и typed-vs-legacy
encoder split; named mapping: EmptyInput→unavailable, NonFiniteResult→overflow,
LimitExceeded→exhausted, invalid op/nonfinite→invalid_argument. На runtime error
выход инвалидируется без stale scalar. R2 проверяет device bytes через codec у
ingress/consumer, Signal generic validator не интерпретирует AudioSync.
T2 подтвердил neutral0/private44byte send copy/write_counter/attempt increment,
17ms и elapsed>300; invalid packet не обновляет freshness и не заменяет прежний
valid payload; unavailable очищает pending и выбирает silence на следующем tick.
Это сообщения рабочих чатов о согласовании seams, не permission реализации.

Координатор принял final decoder policy и разрешил D2 implementation; отдельно
одобрил `wled_protocol::legacy::encode_wled` для permissive compatibility.
Обёртка сохраняет empty/nonfinite/arbitrary gain semantics из old draft, не
дублирует mapping в forwarding header. Overflow finite arithmetic или невозможный
index size безопасно дают valid silence с caller counter, metadata0: это явно
разрешённое отличие от unsafe/nonfinite intermediate legacy path. Legacy entrypoint
не заменяет strict typed API. R2/T2 notified о frozen headers и этом различии.

## D2 implementation / author verification

Изменены только D2 paths из ownership table: два headers/sources helpers,
forwarding old header, два tests и этот документ. `dsp.h`/`dsp.cpp`, CMake, R2/T2
sources, user config/output не менялись. Общий dirty diff не присваивается D2.

Chronology evidence, `build-refactor-device-slice2`:

1. До extraction HEAD types/function получены `git show HEAD:...`; отдельный
   standalone characterization.cpp использует прежний packed make_packet.
   Compile/run exit0 → characterization-goldens.log с literal44-byte hex.
   Old draft header сохранён отдельно draft_wled_packet.h для finite mapping,
   legacy-characterization.cpp/run exit0 → legacy-goldens.log. HEAD и draft
   источники явно разделены, fixtures не вычисляются проверяемым новым codec.
2. Новые tests до helper headers: scalar-red-compile.log и wled-red-compile.log,
   каждый compile exit1 из-за missing public header (TDD compile RED).
3. После helper implementation WLED compile/run exit0. Первый scalar compile
   обнаружил test-only CTAD ошибку двух std::array разных размеров в одном
   declaration; разделены declarations, последующие compile/run exit0.
4. После legacy forwarding + дополнительных sanitation/overflow/overlap cases
   финальный WLED build/run exit0; scalar final build/run exit0.
   scalar-tests.log: bounded numeric tests passed; wled-tests.log: HEAD goldens,
   bounded codecs and counter tests passed. Обе noallocation assertions проходят.
   Scoped whitespace check exit0. Независимый review ещё не выполнялся.

Actual standalone commands (compiler найден из existing cache, не installed):

```powershell
$d2Cpp = ((Get-Content build-refactor-baseline/CMakeCache.txt |
  Where-Object { $_ -match '^CMAKE_CXX_COMPILER:' }) -split '=',2)[1]
$env:PATH = (Split-Path $d2Cpp) + ';' + $env:PATH
& $d2Cpp -std=c++20 -fno-exceptions -fno-rtti -Wall -Wextra -Werror `
  -I libs/dsp_kernels/include libs/dsp_kernels/src/scalar_reduce.cpp `
  tests/scalar_reduce_tests.cpp -o build-refactor-device-slice2/scalar_reduce_tests.exe
& ./build-refactor-device-slice2/scalar_reduce_tests.exe
& $d2Cpp -std=c++20 -fno-exceptions -fno-rtti -Wall -Wextra -Werror `
  -I libs/wled_protocol/include -I libs/bdsp_protocol/include `
  libs/wled_protocol/src/wled_packet.cpp tests/wled_protocol_tests.cpp `
  -o build-refactor-device-slice2/wled_protocol_tests.exe
& ./build-refactor-device-slice2/wled_protocol_tests.exe
```

No configure/Ninja/shared build использовались D2. Portable unit + actual shim
integration проверены direct compiler; application build/DLL/SDK consumer и
runtime/desktop integration выполняет I/R2/T2 после source freeze. D2 результаты
не заменяют independent acceptance или hardware/native evidence.

I exact request: source scalar_reduce.cpp → existing dsp_kernels target;
wled_packet.cpp → new wled_protocol target, include `libs/wled_protocol/include`;
public export/install WLED header + existing forwarding header, same sources в
ESP SDK manifests; runtime/desktop потребители link wled_protocol; для existing
публичного device_protocols include обеспечить transitive codec link/include до
consumer conversion. Targets scalar_reduce_tests / wled_protocol_tests, respective
links dsp_kernels/wled_protocol; WLED tests additionally need old shim include dir.
Все эти manifests редактирует только I. Source hashes author freeze сохраняются
в own build/source-hashes.txt; private artifact oracle не добавляется в SDK.

Remaining risks: cross-owner runtime/consumer wiring и I exports ещё не собраны;
representability guard oversized valid buffer не гонялся как64MiB stress test,
защита проверена статически; physical span validity остаётся caller precondition.
TDD red здесь compile-level, не behavioural stub run. Cadence/counter attempts/
stale/Stop integration принадлежат T2 и не доказываются pure codec tests. D2 не
меняет UI/control/newwire. Public compatibility include сохраняется; его удаление
ради структуры не разрешено. No branches/commits/push/PR/install/userdata writes.
**D2 source-ready / frozen / awaiting integration and independent review.**
