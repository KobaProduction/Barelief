# N0: explicit Converter / numerical DSP handoff

2026-10-07, Europe/Moscow. **DOC READY / PROPOSED APIs / IMPLEMENTATION NOT STARTED**.
Изменён только этот новый документ. D2 ScalarReduce/WLED freeze и его bounded
independent PASS неизменны; они не доказывают поддержку новых numeric contracts.
Новые headers, code, tests, IDs, schemas/generated и manifests не создавались.

Полностью прочитаны AGENTS.md, [numeric-data-contracts](numeric-data-contracts.md),
обновлённый [implementation-plan](implementation-plan.md) и все 1069 строк
[закреплённого Clean Architecture / DDD](https://github.com/ArthurKoba/ai-agent-workflow/blob/fdb39e72f330b5e6622895cc31ca8e7b73699629/skills/software-engineering/clean-architecture-ddd.md).
Implementation/review получают эту ссылку и требование полного чтения. Справочник
не backlog. Семантика, dtype и представление независимы; stable IDs не переименовываются.
Прочитан N0 proposal R [numeric-foundation-handoff](numeric-foundation-handoff.md),
конкретные seams обсуждены напрямую; это согласование авторов, не independent acceptance.

## 1. Foundation seam: что получает D

Принимается предложение R: homogeneous явно ограниченные channel planes,
rank0..2 внутри plane, channels не скрытая третья ось. Scalar имеет одну plane,
Vector — samples/bins/bands; History Matrix[time,columns] на канал old→new.
Interleaved PCM — один backing bytes, plane c offset=c×storage_width,
sample_stride=channel_count×storage_width; planar — другие checked offsets.
Channel identities/order/count входят в exact contract. Merge/resample не Converter.

Единственный R registry: Int8/UInt8/Int16/UInt16/Int32/UInt32/Int64/UInt64/Float32/
Float64; byte=UInt8 alias. Все 10 types хранятся/удерживаются/передаются точно независимо
от поддержанных вычислений. Rank0 — одно число любого dtype, не специальный float.
Native logical width и storage width различны: packed signed24 — logical Int32,
3-byte encoding, valid24-in32 —4 bytes. Structured packet — отдельная категория,
schema/version/encoding, не UInt8 Vector; generic numeric Converter её не принимает.

D использует R `NumericContract`, `NumericView`, `MutableNumericView`, checked
plane/stride access и dtype registry из signal_contracts; нет копии registry,
NodeKind/graph/JSON/Qt/BDSP/OS/ESP-IDF зависимости. Public byte→T access — memcpy/
endian decode в существующий T; alignment не создаёт lifetime объекта T. Prepared
backend scratch может быть typed; raw byte pool нельзя reinterpret_cast в T*.
R владеет semantic/link/coordinates/clock validation и выбором population;
D — правдивым numerical support query, алгоритмом и checked scratch requirements.

R/D согласовали разделение structural validation и value-domain validation.
Предлагаемый `ValueDomain { FiniteOnly, AnyIeee }` — constraint numerical node
input/support row, не новый dtype или разрешение любой семантике принимать NaN.
Structural validator проверяет storage/shape/strides/lifetime и всегда finite
coordinates; numeric sample values проверяет соответствующая input policy.
FFT/window multiplication/Mel/ScalarReduce — FiniteOnly. Converter float input —
AnyIeee для собственной default Reject либо explicit finite replacement/clamp;
успешный output Converter гарантирует FiniteOnly. Integer values finite по природе.
Ingress не доверяет producer finite claim без value checks на нужной границе.
Legacy `validate(FrameView)` finite-only semantics сохраняются в explicit adapter.
Value checks не требуют полного History scan на каждом callback: только relevant
new row/population и неизменяемое ранее проверенное состояние по R plan.

## 2. Exact proposed Converter APIs (не готовые headers)

Новые имена не wire ordinals; общие policy/value DTO принадлежат signal_contracts
по согласованию R/D, domain хранит их в explicit ConverterParameters, K/P переводят
в versioned wire/storage DTO. `NumericLiteral` — tagged exact Int64/UInt64/Float32/
Float64 bits, не все значения double. Никаких per-element variants на tick.

```cpp
// Proposed signal_contracts policy DTO, R header owner / D semantics owner.
enum class Rounding { TowardZero, NearestTiesEven };
enum class Overflow { Reject, Saturate }; // wrap отсутствует
enum class PrecisionLoss { RejectInexact, AllowDeclared };
enum class NonFinite { Reject, ReplaceFinite, ClampFinite };
enum class ScaleMode {
    Identity, IntegerAffine, DyadicAffine,
    PcmNormalize, PcmDenormalize, Binary64Affine
};
struct Dyadic { NumericLiteral numerator; std::int16_t exponent2; };
struct Scaling { ScaleMode mode; Dyadic gain, offset; /* exact typed literals */ };
struct ConversionPolicy {
    Rounding rounding = Rounding::NearestTiesEven;
    Overflow overflow = Overflow::Reject;
    PrecisionLoss precision_loss = PrecisionLoss::RejectInexact;
    NonFinite nonfinite = NonFinite::Reject;
    Scaling scaling; // Identity unless explicitly set
    NumericLiteral nan_replacement, negative_inf_replacement, positive_inf_replacement;
};

namespace dsp_kernels {
enum class NumericStatus {
    Ok, UnsupportedCombination, InvalidContract, InvalidPolicy, InvalidLayout,
    EmptyPopulation, LimitExceeded, BufferTooSmall, NonFiniteInput,
    Overflow, PrecisionLoss, BackendFailure
};
enum class LossFlag : std::uint32_t {
    None=0, Rounded=1, Saturated=2, ReplacedNonFinite=4, Underflow=8
};
struct NumericResult {
    NumericStatus status;
    std::size_t used_scratch, required_scratch;
    std::size_t error_plane, error_element; // sentinel SIZE_MAX if no location
    std::uint32_t loss_flags;
};
struct KernelLimits {
    std::size_t max_elements, max_planes, max_input_bytes, max_output_bytes,
                max_scratch_bytes;
}; // effective actual caps, no desktop defaults
struct ConversionRequest {
    signal_contracts::NumericContract input, output;
    signal_contracts::ConversionPolicy policy;
    KernelLimits limits;
};
struct ConversionSupport { NumericStatus status; KernelRequirements requirements; };
ConversionSupport conversion_support(const ConversionRequest&,
    const KernelCapabilities&) noexcept;
NumericStatus prepare_conversion(const ConversionRequest&,
    const KernelCapabilities&, PreparedConversion& out) noexcept;
NumericResult convert(const PreparedConversion&, const signal_contracts::NumericView&,
    signal_contracts::MutableNumericView& destination,
    std::span<std::byte> scratch) noexcept;
std::span<const KernelSupport> kernel_support_table() noexcept;
}
```

`KernelRequirements` exact checked bytes/alignment/plan-size + bounded stack;
`PreparedConversion` fixed-size caller-owned plan с immutable resolved contracts/
policy/limits и выбранным function pointer; без owning dynamic strings/spans на
временный request. `KernelCapabilities` — immutable compiled numerical subset,
не второй domain registry. KernelSupport явно включает input ValueDomain,
output guarantee и supported rounding/scaling/loss subset. Runtime композиция
проецирует её в actual device caps;
domain не линкует concrete backend. Prepare failure сохраняет out plan неизменным.
Convert сверяет actual bounds и immutable descriptor, не делает resolve графа.

Atomic convert: scratch disjoint с input/destination, staging всех logical output
элементов в dense byte order плюс fixed arithmetic workspace. Capacity/address/
overlap/stride проверки до записи. После успешного вычисления всех элементов —
nofail scatter logical bytes в уже проверенные destination offsets; padding
неизменён. Error: destination bytes/descriptor unchanged, used_scratch=0,
required_scratch — prepared bound, diagnostic first failing logical plane/index,
loss_flags=0 (нет частично принятого результата). Scratch contents unspecified.
Input/destination overlap допустим только через disjoint staging; без scratch —
BufferTooSmall, никаких heap fallback. Внутренний kernel один frame-level dispatch;
loop typed/specialized, без virtual/variant/dtype switch на каждом элементе.

## 3. Policies и supported conversion combinations

Default: identity scale, reject overflow/nonfinite/inexact, nearest-ties-even для
float→integer. TowardZero — отдельная explicit policy. Выбор rounding не разрешает
precision loss:1.5→Int32 при RejectInexact = PrecisionLoss, AllowDeclared→2.
Saturate — отдельно разрешённая потеря диапазона; требует AllowDeclared, иначе
InvalidPolicy на Prepare. NonFinite replacements тоже требуют AllowDeclared и
конечные объявленные typed значения; ClampFinite задаёт оба inf replacements и
отдельно NaN replacement (у NaN нет направления). Дефолт всегда Reject.

Предлагаемый N1 набор (НЕ IMPLEMENTED, capabilities добавляются только после tests):

| Pair / operation | Поддержка и refusal |
| --- | --- |
| Любой integer→integer из8 fixed types | Identity, exact signed/unsigned comparison, checked widening/narrowing; reject или explicit saturation; никогда modulo wrap |
| Integer→Float32/64 | IEEE exactness bit check; reject inexact или deterministic explicitly allowed rounding; UInt64 не проходит через Float64 intermediate для Float32 |
| Float32/64→integer | Finite/rounding/range checked до cast; reject inexact независимо от rounding enum; saturation лишь с явной policy |
| Float32↔Float64 / same dtype | Exact widening и checked narrowing, сохраняет −0; finite subnormal supported; inexact/underflow reject либо explicitly allowed rounding |
| Same dtype endian/layout/pack | Exact semantic/shape/channels preserved, checked byte decoding/repacking; packed24/valid24 canonicality; no numeric loss от одного endian switch |
| PCM16/24/32 integer↔Float32/64 | Только explicit normalization/denormalization, указанное below scaling; signed/unsigned bias и valid bits сохраняются в declared representation |
| Integer/DyadicAffine integer source | Exact gain/offset literal, fixed-limb arithmetic, power2 rationals; supported bounded exponent range [-63,+63], либо UnsupportedCombination при Prepare |
| Binary64Affine | Первоначально только float source/destination и AllowDeclared; declared separate Float64 multiply/add (no hidden FMA), finite checked; integer64 source не принимает floating gain fallback |
| Arbitrary rational scale / arbitrary double affine Int64 | UnsupportedCombination до explicit specialization/bit-exact tests; нет silent conversion в double |
| Scalar rank0 / ordinary Vector/Matrix rank1/2 | Все advertised dtype pairs в bounded planes; rank/axes/semantics/channels/clock/coordinates не изменяются |
| History Matrix Converter | Incremental specialization ниже; обычный full-frame Matrix loop не объявляется History processing kernel |
| Structured / semantic conversion / resample / channel merge | UnsupportedCombination generic Converter; отдельный schema/DSP processor |

Precision-loss относится к представлению **намеренного преобразованного значения**:
identity exact source value, IntegerAffine/Dyadic/PCM — точная integer/rational
арифметика; потеря обнаруживается до narrowing. Binary64Affine явно объявляет
rounded Float64 evaluation в allow policy, не маскируется под exact transform.
FFT/Mel math approximation задаётся numerical kernel/accumulator контрактом, а не
cast epsilon и не обещанием математически точного FFT. Эпсилон не проверка exactness.

## 4. Алгоритмы BEFORE casts, exact64 и UB

Основание осторожности: [C++ floating-integral conversions](https://eel.is/c++draft/conv.fpint)
не гарантируют безопасный out-of-range cast; [floating conversions](https://eel.is/c++draft/conv.double)
не задают наш deterministic rounding; [integer conversions](https://eel.is/c++draft/conv.integral)
не реализуют требуемую reject policy. Ни implementation-defined cast, ни host
rounding mode не является проектным numeric контрактом.

1. Validate dtype/encoding/actual extents/planes/strides/bytes и multiplication
   bounds прежде load. Decode IEEE bits или integer unsigned bits отдельно;
   NaN/Inf классифицировать до любой numeric cast. Negative zero остаётся IEEE0
   со знаком; для integer результат0 считается equal numeric value, знак0 исчезает
   по объявленному float→integer преобразованию, не random underflow.
2. Integer magnitude без `abs(INT64_MIN)`/`-INT64_MIN`: из отрицательного x
   вычислить uint64 magnitude через `-(x+1)` плюс1 после safe conversion; иначе
   unsigned bit arithmetic. Signed/unsigned compare — знак+unsigned magnitude;
   never cast negative to unsigned ради range test. Min negative допускает magnitude
   2^(b-1), positive signed max2^(b-1)-1, UInt max2^b-1, с safe b==64 branches.
3. Integer→integer: compare magnitude with exact destination bounds до cast;
   при signed minimum branch записать numeric_limits<T>::min() непосредственно.
   Saturation выбирает exact typed endpoint, затем proven representable cast.
4. Integer→float: integer bit-length и trailing zeros определяют exactness
   (precision24/53); например2^53+1 inexact,2^63 exactFloat64,UInt64max inexact.
   Для AllowDeclared guard/round/sticky и retained LSB реализуют ties-even;
   кодировать IEEE result, не direct implementation-defined int→float round.
   RejectInexact не пробует опасный inverse float→uint64 cast для проверки.
5. Float→integer: извлечь sign/exponent/significand (24/53bits), выделить integer
   magnitude и discarded fractional bits. TowardZero или guard/round/sticky
   NearestTiesEven выполняются в unsigned limbs, проверить carry/exponent/range,
   затем signedness bounds; только после этого cast/storage. Не сравнивать
   `double(UINT64_MAX)` с source: rounded bound=2^64 и ошибочно пропускает2^64.
   Ровно−2^63 может стать Int64min, +2^63 нельзя Int64,2^64 нельзя UInt64.
   Negative fractional→UInt может round to0, но при RejectInexact всё равно error;
   negative integer -1 overflow, saturation explicit0 только с разрешённой loss.
6. Float64→Float32: IEEE exponent/significand checked и software ties-even pack,
   включая normal/subnormal boundary, mantissa carry и sign0. Range clamp/overflow
   decision до float cast; finite rounded-to-zero subnormal — Underflow|Rounded
   при AllowDeclared, PrecisionLoss при Reject. Exact widening subnormal разрешён.
   Не предполагать x86 extended long double, ESP native Float64 FPU или FTZ
   correctness: profiles с FTZ не advertise exact subnormal math specialization.
7. Exact affine: portable fixed unsigned limbs + sign, no mandatory __int128.
   Integer64×integer64+offset требует до129bits; Dyadic gain/offset exponents
   ограничены[-63,+63], alignment и carry требуют заранее рассчитанного fixed
   workspace (предложение6×UInt64=384bits + sign/exponents; строгий bound proof
   до admission). Проверяются limb shifts/carries, division remainder/rounding;
   overflow width→explicit error, не wrapped multiplication. Неподдержанный
   exponent/scale отвергается при Prepare; ноль/identity fast path exact.
8. Nonfinite replace/clamp выполняется перед scaling; конечность replacements
   validated при Prepare. Scaling arithmetic overflow отклоняется, а numeric
   destination saturation может сработать лишь в proved exact range comparison.
   Overflow stage не превращается в arbitrary Inf, затем cast. При любом error
   publish unavailable с diagnostics R, не stale или partial destination.

## 5. PCM16 / packed24 / valid24-in32 / PCM32

Source/device/capture adapter объявляет **actual** signedness/valid bits/storage/
normalization/endian/strides/channels/rate/timebase. Его decode снятия transport
framing не меняет numeric meaning. Device configured output Float32 conversion
может быть explicit source-format option с declared policy/caps; legacy capture
normalization остаётся explicit compatibility adapter. Между graph ports implicit
unpack/normalize/scale/Converter insertion запрещены. Нативный источник Int16
идёт в float FFT через выбранный пользователем Converter, не hidden helper FFT.

| Формат | Before-cast decode/encode |
| --- | --- |
| Signed16 | Собрать UInt16 LE/BE; negative sign magnitude изtwo's-complement bits; checked Int16 storage |
| Packed signed24 | Собрать ровно3bytes UInt32; bit23 sign; negative logical Int32=−magnitude (до2^23, safe); не shift negative signed и не cast unsigned0xffxxxxxx в signed как sign-extension |
| Valid24-in32 LSB | Проверить upper8 равны canonical sign-extension bit23; decoded24 logical value, bad padding reject |
| Valid24-in32 MSB | Проверить low8 zero, logical24=unsigned bits>>8 + explicit sign decode; endian сначала, bit placement затем |
| Signed32 | Полный Int32 range, explicit endian, min specialbranch; не packed24 |
| Unsigned PCM | Extract validbits без сдвига наwordwidth; subtraction midpoint unsigned magnitude+sign exact; semantic silence=bias midpoint |

LSB/MSB valid24 logical value одинаков, bit placement определяет serialization,
не gain256. Signed normalize `x/2^(valid_bits-1)`; unsigned `(x-bias)/2^(b-1)`.
Float32 exact для PCM16/24, но всеPCM32 values не representable; Float64 exact
для указанных16/24/32 integer dyadics. Denormalize inverse rational scale+round,
range check до cast; +1→signedPCM overflow при Reject, explicit Saturate→max.
Default PCM32→Float32 RejectInexact допускает только фактически representable
значения; AllowDeclared подписан пользователем, не скрытый default. Channels,
rate/timebase и порядки не меняются; signed silence0/unsignedbias encode корректно.

## 6. ScalarReduce / exact count proposed APIs и combinations

Frozen `scalar_reduce.h/.cpp` сохраняются как float32 legacy API без смены ABI.
Будущий N1 typed API в новом `numeric_reduce.h/.cpp` использует DType/view Р.
R заранее выбирает selected channel planes и population: Vector all samples,
Scalar1, History только newest logical row с zero-row/override semantics.
Explicit PopulationParameters входят в processing config; channels All или
объявленный bounded ordered subset; default совместим со старым both stereo.
Empty History(frames0,count0) — valid state R; reduction EmptyPopulation →
unavailable, а не fake scalar0. FilledFrameCount — отдельный R output UInt64
default; UInt32 только declared capacity fitting specialization, never float.

```cpp
struct ReductionRequest {
    DType input_dtype, result_dtype;
    ScalarReduction operation;
    Accumulation accumulation; // ExactUnsigned, ExactSignedLimbs, Float64 etc.
    PrecisionLoss precision_loss;
    Rounding rounding;
    KernelLimits limits;
};
ReductionSupport reduction_support(const ReductionRequest&, const KernelCapabilities&) noexcept;
NumericStatus prepare_reduction(const ReductionRequest&, PreparedReduction& out) noexcept;
NumericResult reduce(const PreparedReduction&, std::span<const CheckedNumericPlane> population,
                     MutableNumericView& scalar_output, std::span<std::byte> scratch) noexcept;
```

CheckedNumericPlane — R prepared borrowed per-channel row/vector accessor,
not arbitrary raw bytes masquerading as contiguous T*. Same logical dtype,
checked total count before scan; no per-element semantic/ring lookup. Result rank0
one plane, typed payload bytes staged until full success. Kernels never choose
latest row themselves, never copy entire History. Packet not numerical population.

| Reduction | Предлагаемый numerical support / declared errors |
| --- | --- |
| Integer maxabs8..64 | UInt samewidth exact magnitude; signedmin→2^(b-1) fits unsigned result; unsigned maxabs identity; signed result only explicitchecked selected dtype capable of bound |
| Float32/64 maxabs | Samefloat exact abs bits, finite reject; UInt64 not cast tofloat result implicitly |
| Mean integer≤32 | Exact signed/magnitude sum with checked population bound; rational sum/N→declaredFloat32/64 with final exactness/rounding rules |
| RMS integer≤32 | Exact unsigned sum-of-squares (min abs unsigned), bounded limbs; declaredFloat32/64 final result; RejectInexact only when exact rational-square/result test succeeds; otherwise PrecisionLoss |
| Mean/RMS Int64/UInt64 | Initially only Float64+AllowDeclared, exact limb sum/squares before final declared Float64 evaluation; RejectInexact combination UnsupportedCombination until exact rational/sqrt rounding proof implemented |
| Float32 Mean/RMS | DeclaredFloat64 accumulator, outputFloat32 orFloat64; same existing sequential legacy population order characterized; stable scaled RMS avoids FLT_MAX square overflow when needed |
| Float64 Mean/RMS | New ANSI reference specialization only after tests, declaredFloat64 arithmetic + scaled RMS/mean; no claim of exact real mean/RMS or hardware acceleration |
| Integer result Mean/RMS | UnsupportedCombination initially; user chooses declaredfloat result or explicit Converter; не auto-insert integer-round stage |

`max_elements` для exactinteger aggregate предлагаем ограничить UInt32max count:
sum64×count32≤96bits, square64×count32≤160bits; portable3×UInt64 accumulator
с explicit sign покрывает сумму/квадраты (workspace bound + carry tests).
Double accumulation не exact UInt64 storage/identity/maxabs. Integer64 mean/RMS
Float64 stage допускается лишь с объявленной loss; exact inputs не проходят через
double перед aggregate. Lesser-width exactReject RMS oracle сравнивает candidate
binary result squared×N с exact sumsq, без epsilon (и проверяет range/subnormal).
Requested accumulation/output/policy support отражается в caps, не ignored fields.

## 7. Actual existing backend table и proposed wrapper scope

Статически проверены existing [dsp.h](../../libs/dsp_kernels/include/dsp_kernels/dsp.h),
[dsp.cpp](../../libs/dsp_kernels/src/dsp.cpp), [backend_ansi.cpp](../../libs/dsp_kernels/src/backend_ansi.cpp),
[backend_esp_dsp.cpp](../../libs/dsp_kernels/src/backend_esp_dsp.cpp),
[backend_tables.cpp](../../libs/dsp_kernels/src/backend_tables.cpp) и read-only CMake.
Ниже current source support, не новый hardware/performance/test PASS:

| Existing kernel | ANSI | ESP_DSP selection | Dtype/layout actual |
| --- | --- | --- | --- |
| FFT radix2 | ESP-DSP vendor `fc32_ansi` | SDK-selected `fc32` (target/config dependent) | Float32 typed contiguous real input→Float32 magnitude N/2; complex scratch2NFloat32 |
| FFT radix4 | `fc32_ansi` | Тот же ANSI radix4 path | Float32, characterized radix4 geometry; нет FFT64/Int FFT |
| Window coefficients / Hann/Hamming/Blackman/Rect | Common std math producing float | Тот же common code | Float32 output coefficients; coefficients may use std math intermediates, не Float64 coefficient API |
| Bark / MelFilterBank | Common float implementation | Тот же common code | Float32 magnitudes/bands; Mel bands≤50 currentcompiled limit |
| StereoProcessor | Float32 two-span wrapper | Common wrapper + selected FFT | Ровноleft/right, не current native multichannel API |
| Existing ScalarReduce | Float32 spans, double sum | Commonportable C++ routine | Float32 result, не exactinteger/count API |
| Converter / typed numerical reduce | Отсутствует | Отсутствует | Этот N0 proposal, не advertised implementation |
| Float64/integer FFT/window-function/Mel | Отсутствует | Отсутствует | deterministicunsupported; foundationdtype не обещает kernel |

Window accumulator (R) собирает bytes dtype-preserving для advertised PCM Scalar/
blocks→Vector, без float arithmetic; Spectrogram (R) dtype-preserving row ring.
WindowFunction multiplication, FFT, Bark/Spectrum gain, SpectrumMerge и Mel
численные kernels остаются Float32 для первого corrected support set. Explicit
Converter необходим до входа Float64/integer, если пользователь выбирает этот
маршрут; kernels не делают скрытый cast. New wrapper может обработать N prepared
planes одной общей Float32 FFT scratch по очереди, bounded outputs на Nplanes;
не включает одновременно N scratch без budget. Semantic PCM/window/windowed→
SpectrumMagnitude, Spectrum→Mel остаются отдельными processors, complexSpectrum
не выдаётся за magnitude. New Float64 kernels требуют отдельного source/test
handoff и support evidence; их нет в этом N0 promise.

Default deviceFFT2048/explicit4096 не переписываются. Current compile limit из
`PORTABLE_DSP_MAX_FFT_SIZE` поддерживает512/1024/2048/4096, runtime actual intersection
с configured device/kernel limits; FFT::prepare допускает lower-level N≥4 power2,
а existing application estimate допускает N≥512. Не превращать lower-level
возможность в новые device advertised defaults. Radix4 требует power4 supported
geometry. Existing shared tables process-wide, initialized prepare, ownership/
external table sufficiency preserved. ESP Float64 storage/conversion может быть
software и дорогим; actualdevice может advertiseUnsupported Float64 computation,
даже если byte-retain Float64 допускается. Hostsourcecompile не Float64FPU proof.

## 8. Memory / History processing / source-vs-processing boundaries

R required_storage считает lastaddress каждого plane из offset+Σ(extent-1)×stride
+storagewidth checked; отдельно logicalelement count (всеplanes), allocation
alignment padding, maxaddress/span bounds. Нулевые extents имеют explicit no-data
meaning, не extent-1 underflow. Scalar ровно1value, никакого size0 sentinel.
Source packet endian/packing не меняет width native registry; packed24 budget3,
valid24-in32budget4. Plane/channel count, stride sign/overlap/sparse layout,
ring masks/newestoverride и coordinates metadata входят в budget.

Converter staging=sum(logicalelements×destination_storagewidth), limb workspace
fixedbound и alignmentchecked; destination offsetspadding отдельно. FFT work2N×4,
coefficientsN×4, spectrumplanes×(N/2)×4, Melplanes×bands×4, History capacity×bands×
planes×actualstoragewidth+ring metadata. Суммы/products/pooltotal/oldbundle+candidate
staging checked до reserve. Elementwidth8 и interleaved24 не проходят sizeof(float)
estimate. Backend repack inputstrides→prepared Float32scratch учтён как bounded
workload, не fullhistoryflatten. Physical stack bound и nested caller peak требует
ESP integration receipts отдельно от heap/Payload estimate.

Ordinary Matrix conversion может быть whole declared boundedframe. **History**
N1 incremental specialization: R owns convertedring, D converts только новый row/
bounded insertedrows заtick; plan tracks producerlineage+row cursor, zero masks;
retained older rows уже в output dtype. Initialbootstrap/fullring transfer только
prepare/controlboundary с checkedpeakbudget, не canonical processingcallback.
Gap/discontinuity/unknownrowlineage → affectedreset/unavailable, не fullringfallback.
Пока incremental specialization не реализована, supportquery возвращает
UnsupportedCombination для History Converter, explicit diagnostics. Это ступень
N1 admission, не объявление выполненного History numeric scope.

## 9. Boundary oracles / TDD slices / будущие paths

Ни один proposed numeric test ниже не запускался в DOC-only N0:

| N1 piece | Независимый от implementation oracle / target cases |
| --- | --- |
| Foundation R | Все10widths/rank0..2/retain/endian/unaligned/padded planes; exact Int64min/max/UInt64max,2^24±1/2^53±1; descriptor не равен форме; UInt64 count >2^53 сохранён byteexact |
| Cast D | Literal integer endpoints и IEEE bit patterns; 2^63/2^64 exclusivefloatlimits, nextafter neighbours, −1→UInt, ties±0.5/1.5/2.5, mantissa carry; explicitpolicy reject/saturate/replacement/unchanged destination |
| Float D | −0 bitpreservation, min/maxnormal/subnormal, Float64tiny→Float32zero and exact widening, NaN/Inf reject; no dependence FE rounding mode; IEEE expectedbits generated offline then checked-in, no testedkernel used as oracle |
| PCM D/adapter | LE/BE literal `00 00 80`→−8388608 and `ff ff 7f`→8388607; canonicalLSB/MSB padding bad;16/24/32 min/max, UIntbias silence, +1 inverse reject/saturation, PCM32precisionloss; interleaved3channels/padded/misaligned strides/truncated sample |
| Scale/reduce D | Offline exactinteger/rational oracle (arbitraryprecision only test generation), signedminmaxabs, UInt64max exactUIntresult; integerlimb carries/squares/popcount; mean/rms fractional/inexact/results, newestrow excludes old rows, declaredchannel subset |
| Runtime R+D | Failinglate element leaves entire destsentinel/plan/token/working intact; preparedlimits, scratch insufficient/alias/pool exhausted; noalloc afterprepare, no per-elementvirtual, History incremental cost independentcapacity; unaffectedring continuity |
| Existingkernels D/R | Characterization legacyFloat32goldens/FFT magnitudes/windows/Mel, backend specifictolerance; unsupported Float64/Int paths reject beforeApply; actual2048/4096 preservation, dtype-awarememory |
| Boundaries T/K/P/I | BOTHcodecs/DATA/storage exact64/typedliteral policies/PCMlayout roundtrip and lossy downgrade refusal; legacybytes preserved; SDK/ESPactualcaps/stack, appbuild/shorttarget integration; fresh numericreview separate |

TDD: failing target for each new cast/policy/primitive, then smallest specialization;
characterize existing Float32before move; typedConverter→R preparedrouting/count→
incrementalHistory→boundary schemas/storage→device/UI/N4. Недопустимо завершить N1
общимfloat smoke без pair/layout/policy matrix и независимых numeric/runtime oracles.

Предлагаемое будущее владение, требует coordinator source handoff:

| Owner | Source/test responsibility |
| --- | --- |
| D | Новые existingpackage `libs/dsp_kernels/include/dsp_kernels/{numeric_convert,numeric_reduce,kernel_support}.h`, `src/{numeric_convert,numeric_reduce,kernel_support}.cpp`; private bounded arithmetic/PCM implementation здесь; `tests/numeric_conversion_tests.cpp`, `numeric_reduction_tests.cpp`, `pcm_conversion_tests.cpp` |
| D later only approved | Existing dsp.h/dsp.cpp/backend files только proventypedwrapper/support clarification; frozen scalar_reduce/WLED unchanged unless narrow newhandoff |
| R | signal_contracts registry/descriptors/views/typedaccess/policyvalueDTOs/storagevalidation/retention, domain Converter/Reduction config/strictlinks/caps, runtime plan/clock/history/count/silence/diagnostics; его foundation/runtime tests |
| Capture/device adapters T/assignedembedded | Actualcapture formats/sourceoptions/transport decode, queues/destination/native device resources; D только purePCMcodec, не miniaudio/IDF calls |
| T/K/P/S | Owned delivery/newDATA/registry; BOTHcontrol schema/generated/DTO; versionedJSONlossless64; applicationrevision/caps handoffs; никаких ихfiles D |
| I | Все CMake/export/install/sourceSDK/ESPmanifests и baselineledger; собирает application послеfreeze и shortaffectedtests, oneNinja |

## 10. Решения handoff / limits evidence

R/D seams согласуются в N0; root утверждает signatures/policies/pairs/IDs/ownership
до code. Обсуждены explicitplanes/rank0..2, UInt64count, exactUIntmaxabs и отделение
precisionlosscast от FFTmath. Открыты: final DTO names/limits/result errorlocation,
exponent/limb proof для exactscale, scalaraccumulation/output matrix, History
incrementalstate/cursorloss/bootstrap boundary, target device Float64 converter
availability/subnormal math и stackbudget. Policy fields никогда молча не игнорируются.

Actualcommands: Get-Content/read-onlyrg по source/headers/CMake, pinnedrawguide
read, primary C++draft sections read; apply_patch только этогофайла, scoped
whitespacecheck. No configure/build/test/shared Ninja, no installs/agents/chats/
branches/commits, config/output/schema/generated/frozenhelpers untouched.
Historicalauthor/independentreports не расширены. Это concreteDOCproposal,
не workingnumericimplementation/acceptance. **DOC READY; source implementation requires next concrete handoff.**

### N0 final owner coordination receipt

R прочитал полный D handoff и согласовал proposed filenames/signatures, planes,
ValueDomain, fixed-width policies, UInt maxabs, staged atomic destination и
History refusal до incremental N1b. Root принял направление portable casts всех
advertised pairs/ranks, checked staging и truthful Float32 backend table; это
не acceptance будущих вычислений. R отдельно получил N1a handoff на реальные
foundation headers/views/validation/tests, но D этим назначением code не начинает.

Следующий конкретный seam: usable R data_contract/data_frame/numeric_policy
headers определяют actual names/layout; D сверяет ConversionRequest/policy literals,
ValueDomain/plane access/status/result declarations с ними и получает отдельный
implementation handoff. Не ждать wire IDs K для pure numerical kernels. Saturation
и nonfinite replacement требуют AllowDeclared; arbitrary Binary64Affine integers
не проходят как exact64 fallback. Dyadic bounds/limb proof и integer RMS precision
принимаются по targeted tests ступенями, unsupported combinations объявляются явно.
Published shared destination owner нельзя мутировать; input/destination overlap
возможен только для exclusively owned writable destination через disjoint staging.
History refusal временно до N1b, не завершение numeric цели. Frozen D2 unchanged.

All future support capabilities require implemented tests + fresh independent
review. N0 DOC READY / D IDLE awaiting usable headers and concrete source handoff.

## 11. N1a actual implementation candidate, 2026-10-07

Этот appendix заменяет статус N0 для перечисленных ниже новых D файлов. Это
авторский GREEN и source freeze для интеграции/независимого numeric review,
не acceptance приложения/ESP и не завершение N1b History.

Actual APIs: `numeric_convert.h`, `numeric_reduce.h`, `kernel_support.h`; sources
с теми же именами и private `src/numeric_arithmetic.h`. Старые `scalar_reduce`,
DSP backends, WLED и CMake/manifests не изменены D. R DTO используются напрямую,
без второго public dtype registry или wire ordinal. `KernelContract` хранит
channel IDs/clock в собственных fixed arrays, value spans cleared; `contract()`
rebases при любом copy/move. Admission ceilings:64 planes,256 clock bytes.
Prepared plans после публикации используются как immutable; caller не подменяет
function/policy/requirements. Exclusive writable destination обязателен.

| Actual path | Support and explicit limits |
| --- | --- |
| Converter Identity | All100 dtype pairs; Scalar/Vector/ordinary Matrix; integer64 exact sign/magnitude, float IEEE bit decode/software packing |
| IntegerAffine/DyadicAffine | Integer input, Int64/UInt64 coefficients, exponents [-63,+63]; IntegerAffine exponents0. Floating coefficients/source require explicit Binary64Affine, не fallback |
| PCM normalize/denormalize | Explicit PCM mode, signed or unsigned bias, declared validbits, packed24/LSB/MSB canonical storage; output bounds/precision checked |
| Binary64Affine | Float32/64 input/output, Float64 arithmetic capability and AllowDeclared; finite typed coefficients with bounded dyadic exponents; separate IEEE64 multiply/add, no FMA |
| Nonfinite | Default Reject. ReplaceFinite/ClampFinite require AllowDeclared and three finite declared replacements; Clamp endpoints ordered, NaN separate. Selected replacements checked against transformed destination at Prepare |
| MaximumAbsolute | All10 source dtypes,100 output pairs with runtime precision/bounds checks; Int64min can yield exact UInt64, cannot fit Int64 |
| Integer Mean/RMS | All8 integer source dtypes, Float32/64 results; exact sum/squares, correctly rounded final ratio/sqrt, RejectInexact or AllowDeclared. This extends the initial N0 restriction on64bit results |
| Floating Mean/RMS | Float32/64, only Float32/64 result with AllowDeclared and Float64 arithmetic capability; scaled sequential IEEE64 sum avoids max-value square/sum overflow |
| Integer result Mean/RMS | UnsupportedCombination; no implicit integer rounding stage |
| History input | UnsupportedCombination for whole History in both generic kernels. Caller projects/selects ordinary row/vector; compact incremental gather/converted ring is R N1b, still pending |
| FFT/window/Bark/Mel/stereo | Existing Float32 implementations only; table never advertises Float64 FFT or hardware acceleration. Detailed Converter/Reduction admission is support query, not source inventory rows |

No implicit packing/scaling/resampling/channel merge: Converter preserves semantic,
rank, extents, axes, channel order, coordinates and clock; representation/dtype
change is explicit. Unit/normalization change requires selected PCM mode.
`FrameLimits` and `KernelLimits` are explicit caller data; zero is restrictive.
Integer affine coefficient or exponent combinations outside admission are refused
before publication. Inactive literal IEEE payloads, including NaN/-0, and inactive
exponents are preserved exactly; their structural representation is checked with
R `validate_literal`, but they do not activate scaling/replacement.

Exact arithmetic proof and bounds:

- Private magnitude is12 UInt32 limbs (384bits), independent of compiler int128.
  Addition uses UInt64 carry; a32×32 product +existing limb +carry fits UInt64.
  Shifts guard >=384 and32-bit remainder0 before shifting; all128/64 endpoint
  comparisons precede integer storage/casts. Signed min is formed in unsigned bits.
- Integer64×integer64 needs128bits. Aligning two dyadic terms across bounded
  exponent difference126 needs at most254bits plus one carry, below384.
- Count is checked <=UInt32max. Exact signed sum needs <=96bits; unsigned
  squares and count need <=160bits. No integer sample goes through Double.
- Final integer Mean/RMS searches positive IEEE bit order to find the lower
  representable value. Exact comparison uses candidate×N or candidate²×N against
  integer numerator. Each candidate significand has <=53bits; square×N <=138bits;
  shifts that exceed limb space prove ordering against the bounded numerator.
  Exact midpoint comparison chooses nearest ties-even; TowardZero chooses lower
  magnitude. RejectInexact requires equality, including irrational RMS. Search
  upper2^64 bounds every integer population mean/RMS; no approximate sqrt is used.
- Software float packing uses guard/sticky/retainedLSB, preserves signed zero,
  detects subnormal loss/range before storage. Loss policy is independent of
  rounding policy. Binary64Affine and floating reductions explicitly declare
  IEEE64 arithmetic; scoped environment restores caller rounding/exception flags
  on success/error. Affine volatile stores force separate multiply and addition.

Transactional/resource contract:

- Converter scratch = maximum logical output count×storage width (including3-byte
  packed24), alignment1. Reduction scratch = one encoded scalar. Products/counts
  checked at Prepare; source/destination storage checked against explicit budgets.
- Whole logical result is staged, then no-fail scatter; padding is unchanged.
  Scratch must not overlap input/output, descriptors, layouts, channel IDs, clock,
  coordinates or prepared plan. Input/output byte alias is allowed only through
  disjoint staging and exclusive output ownership. No typed pointer into byte pool.
- On error output is unchanged, used_scratch/loss_flags0, and conversion late-error
  reports first logical plane/index. Reduction returns EmptyPopulation for actual
  zero population, not scalar0. FiniteOnly reductions classify NaN/Inf by bits.
- D loops use prepared source/destination template specialization (100 Converter
  pairs;10 Reduction sources), no per-element virtual/numeric dtype dispatch.
  R `load_bits` remains the checked canonical storage accessor; its format/layout
  checks are not a new unchecked DSP adapter. Generic History dispatch is refused.
- `plan_bytes=sizeof(actual Prepared...)`. `stack_bytes=8192` is a conservative
  reference reservation, NOT a measured ESP task requirement. GCC13.1 Windows
  -O2 `-fstack-usage` artifacts record individual frames: entry convert/reduce1680,
  Converter run <=976, Reduction run1280, ratio pack576, comparison176; call-chain
  estimates exclude libc/compiler runtime. Integration must measure its actual
  target/profile, including R validators/libc. No hardware stack/FFT64 claims.

Reproducible author evidence under `build-refactor-numeric-dsp`:

- `arithmetic-red.log` (compile0/run3) -> `arithmetic-green.log` (0/0);
  `public-converter-red.log` (0/3), `reduction-red.log` (0/3),
  `pcm-subnormal-red.log` (0/3) precede implemented corresponding behavior.
- Final `numeric_conversion_tests-final.log`, `pcm_conversion_tests-final.log`,
  `numeric_reduction_tests-final.log`: each compile0/run0 using actual frozen R
  source, not archives/stubs. Compiler existing MinGW13.1, -O2 C++20,
  -fno-exceptions -fno-rtti -UNDEBUG -Wall -Wextra -Werror -static.
- Canonical final evidence: `run-numeric.ps1`, `final-before.json`,
  `final-after.json`, `final-receipt.log`. All21 project compilation inputs have
  identical SHA256 before/after and still match current files; includes every D
  header/source/test and transitive readonly DSP/contract headers, R new cpp plus
  frame/buffer_pool cpp. R policy EDBF68FE...; data_frame EF31B030... matches freeze.
  Earlier `all-before.txt`/`all-after.txt` are point snapshots, not final freeze.
  `.su` regenerated for final D sources after receipt; reference measurements only.
- Independent literal oracles cover all100 exact-one pairs, integer min/max,
  UInt64 >2^53, Float64 2^64→UInt64 overflow, Int64min, dyadic cancellation,
  ties/subnormal/signed zero/NaN/Inf, inactive payload replay, canonical PCM24
  and signed16 big endian, unsigned biased subnormal, last-element atomicity,
  scratch alias/insufficient scratch, prepared metadata copy, ambient fenv,
  exact1/3 reject/rounded result and UInt64max/Int64min RMS. Operator-new counters
  record zero C++ heap allocations during repeated prepared kernel calls.

I owns CMake/SDK/ESP/app build and independent numeric acceptance. N1a files frozen
for those steps; generic History stays explicitly unsupported until R N1b compact
row/incremental converted-ring contract is implemented and separately reviewed.

Final D SHA256 (full remaining list in `final-after.json`):

| File | SHA256 |
| --- | --- |
| numeric_convert.cpp | 2ABA18BB0384A59CDB8448B0CB3415631C7A315A99599DFBCE46FA8B4A624DF5 |
| numeric_reduce.cpp | 2A8D55CE56F89176D4AE92A9A7F7412F5087427D4C5BA432B0CD2C6598D73767 |
| numeric_arithmetic.h | 03C0D351F2AAACEE0CD599AFF1FB536AE473816FACF2CEDC9187674D74367C17 |
| numeric_conversion_tests.cpp | C8A413FBF0059A4517EB5F19771A7963D638FE73BF14315F13CFE364294D07F0 |
| numeric_reduction_tests.cpp | 4EAD16D1592F3DA71007F90937954420FE0A63E20F52A5CD85DE3EE3A1B36DE7 |
| pcm_conversion_tests.cpp | 62601DE4ECF00A2CEB14AEA360068645E3FB700776D1D33B9EC2B8C481FA617D |

N1a SOURCE READY / D IDLE for root integration and fresh independent numeric review.

## 12. ESP portability repair and positive Float32 reduction, 2026-10-07

This section supersedes section11's384bit/12-limb and floating-reduction matrix.
The I integration report recorded a real compile failure with cached
Xtensa15.2/IDF6.1/picolibc: its standard library does not expose `std::fenv_t`,
fenv functions or FE_TONEAREST/FE_ALL_EXCEPT/FE_INEXACT. Plain Xtensa without the
actual picolibc response file is a different libc profile and is not this oracle.
No SDK/OS/hardware header was added to the portable library.

One compiled arithmetic authority is exposed as
`declared_ieee64_environment_supported()`. It reports the standard facility
actually used to build the library, not a CPU/FPU/ESP name. With absent facility,
the native environment class and native operation bodies do not exist in the
translation unit; no no-op fenv shim or fake rounding/flags is supplied.
`support`/`prepare` rejects unavailable native modes before publishing a plan.
Caller Float64 capability remains independent of this native-environment facility.

| Canonical operation | Current ESP/picolibc with no fenv | Host with native fenv |
| --- | --- | --- |
| Identity, integer/dyadic affine, explicit PCM conversion | Previous100 pair admission/limits unchanged; software bits | Same software bits |
| MaximumAbsolute, all10 sources | Software bits, no native environment | Same software bits |
| Integer Mean/RMS to Float32/64 | Exact aggregate and final ratio/sqrt, all previous precision/rounding rules | Same software bits |
| Float32 Mean/RMS to Float32/64 | Positive exact software path; RejectInexact or AllowDeclared; Float32 result needs no native Float64 capability | Same exact software path, independent of ambient fenv |
| Float64 Mean/RMS | UnsupportedCombination at Prepare | Previous declared scaled IEEE64 sequence, AllowDeclared and Float64 capability |
| Binary64Affine | UnsupportedCombination at Prepare | Previous declared separate IEEE64 multiply/add, AllowDeclared and Float64 capability |

Thus Float32 canonical Mean/RMS are implemented on the main ESP target rather
than delegated to legacy `scalar_reduce`. The source inventory still means
source dtype availability; operation-specific admission uses the compiled
capability and exact pair/policy query. No Float64 FFT/native acceleration claim.

Float32 arithmetic is deliberately corrected on **both** profiles to the same
mathematically exact aggregate before final rounding. The previous scaled
sequential IEEE64 calculation could lose a Float32 minimum subnormal between
opposite FLT_MAX values. This is a reported behavior correction, not a hidden
target-dependent accumulation policy. Native Float64/Binary64Affine semantics
remain unchanged on supported host profiles, including saved rounding/flags.

Positive portable algorithm/resource proof:

- Private magnitude is now20 UInt32 limbs,640bits. Integer algorithms retain
  their original bounds (affine<=255, sum<=96, squares<=160bits).
- Every finite Float32 is exact significand×2^exponent with at most24bit
  significand and exponent[-149,104]. A signed sum of <=UInt32max values aligned
  to exponent-149 needs <=309bits. Float32 squares align to exponent-298:
 48bit significand +506bit shift +32 population bits =586bits maximum.
- Mean passes the exact signed sum's exponent to final ratio packing; RMS passes
  the exact squared numerator and exponent-298. Search upper bound is FLT_MAX
  (Float32 bits7f7fffff, Float64 bits47efffffe0000000), bounding either real result.
  Candidate×N or candidate²×N are compared by leading exponent then aligned
  integer magnitude; equal-leading admitted operands fit640bits. Adjacent IEEE
  midpoint comparison implements ties-even; TowardZero selects lower magnitude.
- Only the final encoded result can incur precision loss. RejectInexact requires
  exact equality. AllowDeclared reports Rounded and, for an inexact result in
  the subnormal/zero range, Underflow. No native floating operations, sqrt,
  rounding-mode changes or exception-flag observation occurs in Float32 reduction.
- Population<=UInt32max, KernelLimits/FrameLimits/64planes/256clock/staging and
  alias/error atomicity remain enforced. Limb loops and scans are bounded; this
  software path does not claim an accelerated FPU throughput measurement.

Evidence/new authoritative freeze:

- Original immutable integration `numeric-dsp/esp-build.log` is actual source
  compile RED. Artifact-local `esp-fenv-probe-red.log` also reproduces absentfenv
  using the real cached cxxflags/picolibc specs, not a fake SDK header/macro.
- `float32-reduction-red.log` compile0/run3 captures the real lost-subnormal case
  before the exact aggregate implementation; `float32-reduction-green.log`0/0.
  Independent final Float64 oracle is3685555555555555 for
  `(FLT_MAX+minimumFloat32subnormal-FLT_MAX)/3`.
- `run-numeric-portability.ps1` and `portability-repair/{final-before.json,
  final-after.json,final-receipt.log}` replace the prior D21 freeze authority.
  All21 actual project inputs before/after/current equal; three short targeted
  executables each compile0/run0, strict/static MinGW13.1 profile. Uses R repaired
  source data_contract91E66E85... and data_frame571D0F12..., unchanged sharedheaders.
  Prior section11 artifacts are historical, preserved rather than overwritten.
- Final cases additionally prove portable Float32 result with Float64 capability
  false, exact RMS1/FLT_MAX, minimum-subnormal1/3 rejected atomically or rounded0
  with Rounded/Underflow, sqrt(2/3)×minsub rounded1, unchanged ambient host fenv,
  and zero C++ operator-new allocations during repeated software Float32 RMS.
- `portability-repair/esp-fenv-probe-green.log` compile0; probe static_asserts
  compiled native facility false. `target-probe-before/after.json` include private
  source, probe, cached response/specs/sdkconfig hashes and match. This is a
  header capability check, **not** a cross-built SDK archive or hardware execution.
- `portability-repair/stack-host/*.su` records640bit reference compiler frames:
  convert/reduce entry1680, Converter specialization<=1040, Float32 Reduction1504,
  ratio pack704 and absolute compare288 bytes.8192 remains conservative REFERENCE
  reservation, not actual ESP task peak or complete libc/compiler call-chain proof.

Changed D production/test files: kernel_support.h/cpp, numeric_arithmetic.h,
numeric_convert.cpp, numeric_reduce.cpp, numeric_conversion_tests.cpp and
numeric_reduction_tests.cpp; own handoff and own build artifacts. No CMake,
manifests, R, app, SDK headers, legacy scalar/DSP/WLED or user data changes.
I owns one bounded SDK/ESP/app integration retry after source freeze; independent
DSP acceptance follows integration. Generic History N1b and physical ESP task peak
remain pending. D portability source candidate frozen / AUTHOR GREEN / IDLE.

## 13. Independent F1/F2 narrow repair, 2026-10-07

The independent review `build-gate-numeric-dsp-review/review-report.md` returned
FAIL2P2. Its fixtures stayed read-only. This section is a new author source
freeze and GREEN receipt for re-integration/review, not independent acceptance.
Section12's positive Float32 software path,640bit bounds, capabilities, History
refusal and resource limitations are unchanged.

F1: unsigned PCM bias alignment failure for a huge finite input now reaches the
existing destination encoder's Overflow policy. A <=53bit IEEE significand and
<=64bit bias cannot fail640bit alignment near a representable endpoint. A large
case with non-tiny leading exponent proves destination overflow even after the
bias; omitting its unrepresentable materialization cannot change the endpoint
direction. Encoder returns exact unsigned maximum/zero with Saturated when
declared, or Overflow with unchanged output under Reject. Tiny input alignment
still takes the prior explicit biased rounding path, including negative
TowardZero and RejectInexact. No floating multiply or overflow cast was added.

F2: Binary64Affine captures native FE_UNDERFLOW alongside FE_INEXACT before its
scoped environment restores the caller state. It also carries software
prepacking underflow from input/gain/offset, which native operations on already
rounded zero cannot recover. Native Float64 reduction observes FE_UNDERFLOW too;
that analogous case has its own executed author RED, rather than a static claim.
Native facility admission additionally requires the standard FE_UNDERFLOW flag;
actual host availability remains true and actual ESP/picolibc false. There is no
no-op fenv, weakened loss policy or changed separate multiply/add order.

New authoritative evidence is under `build-refactor-numeric-dsp/review-repair`:

- `oracles-red.log`: actual unchanged independent two-mode fixture compile0;
  pcm-saturation run1 (`status9 bitsa5 loss0`), affine-underflow run1
  (`status0 bits0 loss1`). `oracles-green.log`: compile0, both modes run0;
  exact outputs `bitsff loss2` and `bits0 loss9`, both caller fenv preserved.
- Fixture SHA B9D8A76114E97DDE8BE67E6EB31D0F7F526BB70504BE44F6F786520EFD6816CD
  is unchanged; `oracle-before.json`/`oracle-after.json` equal. No review source
  or expected values were edited. Executables/logs are own author artifacts.
- `native-reduction-red.log`: compile0/run3 for Float64 minimum subnormal/3
  missing Underflow. Final numeric_reduction_tests covers result0/flags9 and
  saved ambient rounding/FE_INVALID. This closes the static observation with
  executed author evidence, pending independent recheck.
- `run-numeric-review-repair.ps1`, `final-before.json`, `final-after.json`,
  `final-receipt.log`:21 actual source/header/test inputs before/after/current
  match; strict static direct sources, short3 suites each compile0/run0. R
  repaired source91E66E85.../571D0F12... unchanged; no old archives.
- Additional cases: DBL_MAX both signs for all4 unsigned PCM widths, exact
  endpoint/padding/Saturated, Reject atomicity/loss0/used0; tiny negative biased
  RejectInexact and existing TowardZero preserved; native affine underflow under
  all4 host rounding modes, tiny gain/offset prepacking flags despite a normal
  final result. Existing exact Float32/noheap/alias/preparation tests retained.
- `target-capability.log`: actual cached Xtensa/picolibc header probe compile0,
  native facility remains unavailable. This is not a new full SDK archive or
  physical target run. Shared Ninja/CMake/manifests and SDK headers were untouched.

Production changes only numeric_convert.cpp, numeric_reduce.cpp and private
numeric_arithmetic.h; own3 test files and this appendix. Public DTOs/support table,
legacy scalar/DSP/WLED, R/domain/runtime/app/config/output unchanged by D.
Frozen production hashes:

| File | SHA256 |
| --- | --- |
| numeric_convert.cpp | D612024E5B655E4A7F5D6299EE2F985A56B8BC7AE261B29879BD63606E44131F |
| numeric_reduce.cpp | E7DC71E754B50E737D610CA2A46743EB53B0CFD8DB6DF8A0C5F7168DFF326F6B |
| numeric_arithmetic.h | F1AF549B93C11976266B07887C6A1D938A3426CAF55DB69719E8E8836C698968 |

Scratch/plan/count limits and reference8192 reservation remain unchanged;
physical ESP task peak and complete nested libc/compiler stack are unverified.
I re-integration and the same independent review are next gates. Generic History
N1b and the whole numeric migration remain unfinished. D SOURCE READY / IDLE.

## 14. F3 exact ambient fenv restoration repair, 2026-10-07

Same independent rerecheck closed F1/F2 and found F3. On the admitted MinGW
runtime, `fesetenv` alone returns success but merges newly raised SSE exception
flags into saved state. Exact public-API snapshot checks, rather than retained
FE_INVALID presence, exposed flags5→55 for native underflow and5→7 for exact
subnormal arithmetic. Rounding had been preserved; full exception state had not.

Narrow product repair is only the private NearestEnvironment destructor:
`feclearexcept(FE_ALL_EXCEPT)` precedes `fesetenv(&saved_)`. The first removes
this scope's raised status; the second restores caller flags and control state
from the saved opaque standard environment. No platform header/register layout,
fake fenv, native capability degradation, rounding/loss change or numerical
algorithm change. Disabled/software environments still do neither operation.
Native results/loss flags are collected before restoration as in section13.

New authoritative evidence under `build-refactor-numeric-dsp/fenv-repair`:

- Unchanged independent `rerecheck/repair-edges.cpp` fresh author reproduction:
  `repair-edges-red.log` compile0/run1,220checks/16 exactflags failures. After
  product fix `repair-edges-green.log` compile0/run0,220checks/0 failures. Covers
  all4 rounding modes, both signed native mean subnormals, affine underflow,
  exact subnormal/denormal status, coefficient/offset prepacking and numerical
  loss expectations. Fixture source/expectations were not modified.
- Existing independent originals: review-oracles-green compile0, both F1/F2
  modesrun0; reduction-oracles-green compile0/run0,628/628; boundary-oracles-green
  compile0/run0,18/18. Exact goldens/header and immutable fixture hashes before/
  after equal (`fixtures-before.json`/`fixtures-after.json`). No reviewer output
  is overwritten; all executables/logs are artifact-local author evidence.
- Own native affine test now snapshots all actual FE_ALL_EXCEPT flags and checks
  equality for both lossy half-minsub and exact minsub under all4 modes. Own
  native mean checks signed minsub/3, loss9, exact flags and rounding for all4
  modes. Existing positive Float32 software/noheap/alias/atomic cases retained.
- `run-numeric-fenv-repair.ps1`, `final-before.json`, `final-after.json`,
  `final-receipt.log`:21 actual compiled inputs equal before/after/current;
  three short strict/static direct-source suites compile0/run0. R source and
  public DSP DTOs unchanged, no archive/stub/profile substitution.
- Cached actual Xtensa/picolibc header `target-capability.log` compile0. Its
  absent native facility and positive software implementation are unchanged.
  This remains header evidence, not hardware execution or a new SDK archive.

Changed production: `numeric_arithmetic.h` only, SHA
1DD04C2ACA470E96CC247D3507EEA0FC62412AE4B0529B7F7AD6AEB6BDA3430C.
Own tests: numeric_conversion_tests SHA
DF9D2E0F7EC7066EE0D42861156281B6F846BBBA2AE1A4BD8083B0FCFD616566;
numeric_reduction_tests SHA
2EBF9FF3EFC731213CC964E57AAA6B4AD09922829EE535E26942DF3A5A86C825.
Other section13 source hashes, scratch/count/640bit/reference8192 limits and
API/capabilities unchanged. No foreign source/CMake/SDK/fixture/user data edits.

F3 author GREEN / SOURCE FROZEN for same independent reviewer and I integration;
no independent PASS claimed. Physical ESP task peak, generic History N1b and
the whole numeric migration remain unfinished. D IDLE.
