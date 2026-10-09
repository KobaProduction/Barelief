# N0 → N1: portable numeric foundation handoff

2026-10-07, Europe/Moscow. **N1a F1/F2 REPAIR FROZEN / AUTHOR GREEN / INDEPENDENT RECHECK PENDING**.
Initial N0 proposal below precedes approved N1a implementation; exact delta/evidence at end.
В этом назначении изменён только этот документ. Четыре файла application legacy
freeze, исходники, схемы, generated, CMake и пользовательские данные не менялись;
сборки/тесты не запускались. Ни API, ни новые wire/config IDs ниже ещё не присвоены.

Полностью прочитаны AGENTS.md, [numeric-data-contracts](numeric-data-contracts.md),
обновлённый [implementation-plan](implementation-plan.md) и все 1069 строк
[закреплённого Clean Architecture/DDD](https://github.com/ArthurKoba/ai-agent-workflow/blob/fdb39e72f330b5e6622895cc31ca8e7b73699629/skills/software-engineering/clean-architecture-ddd.md).
Этот URL и требование полного чтения передаются каждому implementation/review чату.
Ниже статическое сопоставление текущих headers/callsites и конкретное предложение,
не доказательство работающей numeric реализации или расширение прежних PASS.

## 1. Граница и выбранная модель каналов

signal_contracts задаёт физически проверяемый descriptor/view/ownership; domain —
единственная authority допустимых портов/связей/комбинаций. Runtime разрешает
контракты и выбирает специализации при Prepare, затем исполняет тем же
PreparedGraph/GraphExecutor. DSP kernels не зависят от graph/domain/application,
protocol/schema/GUI не определяют совместимость. Нет generic Tensor framework,
виртуального числа, второго scheduler или GUI/codec registry типов.

**Выбор: явно ограниченные channel planes, без channel axis внутри rank.**
Один numeric frame имеет homogeneous dtype/shape/semantics и упорядоченный набор
planes. Rank описывает оси внутри каждой plane; plane count/channel identities
являются отдельной явной частью контракта, которую endpoints сравнивают точно.

| Данные | Rank/оси plane | Каналы и storage |
| --- | --- | --- |
| Scalar, в том числе FilledFrameCount | 0, ровно одно значение | Ровно одна plane; результат reduction не маскируется под multi-scalar |
| PCM/Window/Windowed | 1, sample | Mono/stereo/multichannel: N planes с фиксированным порядком каналов |
| SpectrumMagnitude/MelEnergy | 1, bin/band | N planes; смысл, quantity/unit и coordinates сохраняются |
| History | 2, time × bin/band (либо time × объявленная ось источника) | N planes; внутри каждой [oldest→newest][column], не rank3 |
| Primitive Matrix | 2, две объявленные оси | N planes только если семантика допускает каналы; ragged planes не поддерживаются |
| Structured | Rank неприменим | Отдельная category/schema; не UInt8 Vector |

Interleaved PCM представляется теми же channel planes поверх ОДНОГО byte storage:
plane c offset=c×storage_element_bytes, sample stride=N×storage_element_bytes.
Planar — раздельные offsets с unit sample stride. Не нужно deinterleave-copy для
view. Рекомендуется запретить writable overlap logical elements; shared backing
interleaved planes не означает overlap выбранных адресов. History ring cursor —
состояние, не channel axis. StereoSpectrum legacy alias превращается в семантику
SpectrumMagnitude и два channel planes; его стабильный старый ID не меняется.
Выбор упрощает неизменность rank при PCM→FFT→Mel→History и поддерживает более двух
каналов без flattening channel в bins. Цена: bounded plane descriptors/metadata,
которую нужно учесть в admission. Max channels/planes задаётся actual caps;
универсального desktop/ESP значения здесь не предлагается.

## 2. Предлагаемый vocabulary и подписи signal_contracts

Имена — reviewable API proposal, не готовый компилируемый header. Все helper types
в этом разделе принадлежат signal_contracts; enums не задают wire ordinal.

```cpp
enum class DType { Int8, UInt8, Int16, UInt16, Int32, UInt32,
                   Int64, UInt64, Float32, Float64 };
using Byte = std::uint8_t; // alias, не одиннадцатый dtype
struct DTypeDescriptor { DType type; std::uint8_t bytes, alignment; /* signed/IEEE */ };
std::span<const DTypeDescriptor> dtype_descriptors() noexcept;
const DTypeDescriptor* dtype_descriptor(DType) noexcept;

enum class Rank { Scalar, Vector, Matrix };
enum class PayloadCategory { Numeric, Structured };
enum class StorageEncoding { Native, FixedLittleEndian, FixedBigEndian,
                             PackedSigned24LE, PackedSigned24BE };
struct ShapeContract { Rank rank; AxisContract axes[2]; ExtentBounds extents[2]; };
struct NumericContract {
    DType dtype; ShapeContract shape; SemanticContract semantic;
    ChannelContract channels; RepresentationContract representation;
    CoordinateContract coordinates; ClockContract clock;
};
struct StructuredContract {
    SchemaIdentity schema; /* identity + version */
    EncodingIdentity encoding; SizeBounds payload_bytes; // no numeric rank/dtype
};
using DataContract = std::variant<NumericContract, StructuredContract>;

struct PlaneLayout { std::size_t offset, stride_bytes[2]; };
struct NumericView {
    const NumericContract* contract;
    std::span<const std::byte> storage;
    std::span<const PlaneLayout> planes;
    Extents actual; RingRows ring; CoordinateView coordinates;
};
struct StructuredView { const StructuredContract* contract; std::span<const std::byte> bytes; };
struct DataFrameView {
    FrameStamp stamp; // exact integer lineage, clock, status; see §7
    std::variant<NumericView, StructuredView> payload;
};
Status validate_contract(const DataContract&, const FrameLimits&) noexcept;
Status validate_view(const DataFrameView&, const FrameLimits&) noexcept;
template<class T> Status load_element(const NumericView&, ElementIndex, T& out) noexcept;
Status history_row(const NumericView&, std::size_t logical_row, NumericView& out) noexcept;
Status required_storage(const DataContract&, const StoragePlan&, std::size_t& out) noexcept;
Status retain_frame(const DataFrameView&, const RetentionPlan&,
                    BufferPool& payload, BufferPool& metadata, OwnedDataFrame& out) noexcept;
```

DType registry единственный: fixed widths проверяются static assertions; Float32/64
граница требует IEEE binary32/binary64. `dtype bytes` означает native logical
width, не размер packed24 storage. Публичный bare int и double-контейнер для UInt64
запрещены. `SemanticContract` содержит смысл (PCM/window/windowed/Spectrum/Mel/
History/scalar), quantity/unit и history element semantics; complex FFT не
SpectrumMagnitude. `AxisContract` содержит роль sample/bin/band/time/declared axis.
`RepresentationContract` содержит storage encoding/valid bits/bit placement/
normalization/alignment/layout kind и объявленные strides или их bounded profile.
Семантика независима от dtype/rank; combinations всё равно валидируются.

Contract extent bounds включают maximum и правила actual extent: Scalar=1;
PCM block length может меняться внутри negotiated bounds; FFT/Mel обычно fixed;
History имеет bounded capacity и actual filled rows. Не считать разную длину
двух PCM ticks сменой типа. Неразрешённый ingress имеет явный unresolved state,
не нулевой dtype/extent, который случайно подходит ко всем портам.

Contract/layout profile не содержит адресов, ring oldest или producer sequence.
Native contract после разрешения не содержит неопределённого endian; serializer
всегда пишет явный endian/encoding. Coordinate contract задаёт ось/единицу/часы и
regular или bounded explicit coordinates; frame view хранит actual coordinates.
Для bin frequency рекомендуется рациональный Fs/N, timestamps — Int64 ticks
с объявленным timebase. Изменение timestamp/cursor не делает контракт несовместимым;
изменение grid/rate/unit/clock domain может сделать состояние несовместимым.

Scalar без sample clock разрешается с event clock: текущий обязательный sample_rate
для всех FrameType нельзя переносить в новый generic validator. Empty History
(frames=0, capacity>0) — допустимое состояние с count=0; missing/unavailable отдельно.
Scalar не хранится в metadata.magnitude. Preview quantity не является payload.

`load_element<T>` проверяет T==declared logical dtype; memcpy/endian decode в
существующий T, без unsafe reinterpret_cast byte pool в T*. Packed24 загрузка —
Int32 с explicit sign extension. Проверка alignment сама не создаёт lifetime T.
Unchecked specialized access разрешается только после validation/Prepare под
immutable storage; не public unchecked typed span для произвольных wire bytes.
Optimized Float32 backend может использовать подготовленные typed scratch arrays;
repack в scratch без изменения выходного контракта учитывается как backend cost.

## 3. PCM: точные представления и тишина

| Представление | Logical dtype / bytes элемента | Обязательные условия |
| --- | --- | --- |
| Signed PCM16 | Int16 / 2 | valid_bits=16, sign two's-complement, endian, normalization явно |
| Packed signed PCM24 | Int32 / 3 | valid_bits=24, packed LE/BE; диапазон −8388608..8388607; не Int24 dtype |
| Valid24-in32 LSB | Int32 / 4 | bits[23:0], верхние 8 — canonical sign extension |
| Valid24-in32 MSB | Int32 / 4 | bits[31:8], нижние 8 — нули; битовая интерпретация явная |
| Signed PCM32 | Int32 / 4 | valid_bits=32, диапазон Int32, endian явно |
| Unsigned PCM | Соответствующий UInt dtype / declared bytes | valid_bits≤storage width; silence=2^(valid_bits−1), bias/bit placement явно |
| Float PCM | Float32/64 / 4/8 | IEEE, declared normalized range либо explicit unnormalized units |

Все форматы объявляют channel identities/order/count, rate/timebase, layout/
strides/alignment. Нельзя принимать valid24-in32 как PCM32 или packed24 как
трёхэлементный UInt8 Vector. Неканонические padding/sign bits — deterministic
refusal, не silent repair. Signed silence=0; unsigned silence=bias, например
UInt8 silence=128. Float normalized PCM рекомендуемый диапазон [-1,+1]; обратная
нормализация +1 в signed integer требует объявленной overflow policy, потому что
positive integer max меньше 2^(valid_bits−1). Signed normalize x/2^(b−1), unsigned
сначала exact вычитание bias. Ни normalization, ни endian pack не выполняются
неявно при связи/ingress. Source adapter объявляет настоящий captured формат.

Structured generic validator проверяет schema/version/encoding/length/lifetime,
не поля WLED. Device adapter — authority 44-byte LE AudioSync поля/valid silence.
Routing structured silence требует явно зарегистрированного pure silence provider
для этой schema, иначе available silence не объявляется (unavailable diagnostic).
Один memset(0) не реализует unsigned PCM или schema-dependent silence.

## 4. Domain links, Converter и DSP seam

```cpp
struct ConfiguredPort { PortIdentity identity; PortDirection direction; DataContract contract; };
ContractMatch compatible_ports(const DataContract& output, const DataContract& input) noexcept;
ValidationResult resolve_processing_contracts(const GraphDefinition&,
    std::span<const ResolvedIngress>, const DeviceCapabilities&, ResolvedGraphContracts&);
struct ConverterParameters { DataContract input, output; ConversionPolicy policy; };
// D API: signal_contracts types only; no domain::NodeKind dependency.
ConversionSupport conversion_support(const ConversionRequest&, const KernelCapabilities&) noexcept;
NumericStatus prepare_conversion(const ConversionRequest&, const KernelCapabilities&,
                                 PreparedConversion& out) noexcept;
NumericResult convert(const PreparedConversion&, const NumericView&,
                      MutableNumericView& destination, std::span<std::byte> scratch) noexcept;
```

Domain owns structural graph validation and strict resolved-link compatibility;
DSP supplies the truthful specialization support query, runtime composition
projects its table into actual DeviceCapabilities. No domain→concrete DSP/backend
link. `ResolvedGraphContracts` — подготовленный индекс контрактов того же плана,
не второй граф/исполнитель. Caller/control-plane owns resolution storage and
publishes it only on full success. Existing static describe_node can expose
configured/unresolved ports; every edge must resolve before Apply or explicitly
follow the existing unavailable-ingress contract, without assuming Float32.

Match compares category, semantic/quantity/unit, dtype/rank/axes/extent profile,
channels/order, normalization/valid bits/encoding/layout/grid/clock, structured
schema identity+version+encoding/length. Addresses/actual ring cursor не сравнивает.
Port acceptance profiles may list alternatives; Prepare picks one common exact
contract, not cartesian guessing. Unsupported pair returns diagnostic node+port+
field/edge before working mutation; editor/import/control call this same authority.
No cast, auto Converter insertion, merge, resample or semantic reinterpretation.

Converter — explicit Processor. Numeric cast preserves semantic/shape/channels/
coordinates/time; format conversion may change declared representation/layout/PCM
normalization only. PCM→FFT, Spectrum→Mel, reduction/resampling/channel merge are
separate processors. Structured→numeric conversion is not generic Converter.

Policy fields: rounding {toward-zero, nearest-ties-to-even}; overflow {reject,
saturate}, scaling {identity, explicit gain/offset, explicit PCM normalization};
nonfinite {reject, explicit finite replacements/clamp}; precision_loss {reject,
allow_declared}. Defaults: nearest-even for float→integer, reject overflow,
identity scaling, reject nonfinite/inexact; wrap absent. Rounding permission не
снимает precision-loss reject. Для exact integer path gain/offset должны иметь
exact integer/rational representation, либо policy явно разрешает float evaluation
и loss; существующего double gain недостаточно для exact UInt64 affine mapping.
−0, subnormal/underflow, signed abs min, negative→unsigned и float range проверяются
до C++ casts. Precision policy не заменяется epsilon эвристикой.

Policy/value DTO в signal_contracts: NumericLiteral tagged Int64/UInt64/Float32/
Float64 bits, не double-only. D предлагает IntegerAffine/DyadicAffine exact scaling,
Binary64Affine initially только float source/destination + AllowDeclared; arbitrary
rational scale unsupported до отдельного proof. Saturate/finite replacement требуют
AllowDeclared, иначе InvalidPolicy на Prepare; default RejectInexact не снимается.

D определяет exact specialization/support и accumulator widths. Runtime выбирает
kernel один раз; tick не dispatch по dtype каждого элемента. Destination на ошибке
не меняется: prepared scratch/staged output либо prevalidation pass с доказанным
no-fail commit. Публикация только после полной успешной conversion; error → typed
unavailable diagnostic, без partially converted/stale frame. Scratch/peak budget
обязательны. History conversion не сканирует всю матрицу на каждом callback:
предлагается initially advertise Converter для Scalar/Vector и обычной Matrix;
History либо reject до incremental-row specialization, либо N1 D/R реализуют и
проверяют собственный converted ring/new-row lineage. Этот выбор нужен до N1 gate.

Window: Scalar PCM→Vector либо block PCM→Vector по explicit accumulator contract;
Spectrogram: Vector→History Matrix, dtype/semantics/planes/coordinates сохранены.
Count output Scalar UInt64, Unit::Frames — рекомендуемый default; UInt32 только
explicit advertised/configured specialization с checked capacity≤UINT32_MAX.
Filled count=min(rows accepted, capacity), не lifetime counter. ScalarReduce задаёт
operation/population/latest row/channel selection/result dtype/accumulation policy:
maxabs Int64 min требует UInt64 result или refusal, не abs UB. Mean/RMS integer→
float допустимы только с declared result/precision policy; все kernels всех dtype
не обещаются. Splitter/Switch/Gate сохраняют exact resolved output contract.

## 5. Combination matrix и checked admission

Предлагаемая строка KernelCombination содержит kernel/backend, input/output
ContractProfile, policy set, input ValueDomain, geometry limits и memory needs.
DeviceCapabilities хранит actual строки, не декартово произведение отдельных
lists dtype/rank/kind. Empty set поддерживает ничего. Runtime выбирает одну
доказанную специализацию при Prepare. Domain использует portable support data,
не зависит от конкретного DSP backend. Матрица immutable для service epoch.

| Initial N1 предложение | Support/refusal |
| --- | --- |
| Storage/access/retain/routing | Все ten dtype и rank0..2 при checked geometry; structured отдельно |
| Converter Scalar/Vector/обычная Matrix | Только реализованные input/output/format/policy pairs |
| Converter History | Explicit UnsupportedCombination до incremental-row specialization; не generic full-matrix loop на tick |
| MaxAbs integer | Same-width UInt result, exact signed min |
| Mean/RMS Int64/UInt64 | D предлагает только Float64+AllowDeclared: exact fixed-limb sum/squares перед declared final float evaluation |
| FFT/window/Mel | Existing optimized Float32 subset; int/Float64 kernels не обещаются без implementation |
| Spectrogram/count | Exact новая row, dtype/planes сохранены, count UInt64 default/explicit UInt32 |
| WLEDEncode | Только объявленная Mel input combination/device schema |

Host Float64 type не доказывает ESP FPU/FFT64/taskstack. Default FFT2048 и explicit
4096 config не переписываются. Caps пересекают kernel support и actual adapter
formats/backend; negotiated geometry не увеличивает profile ceiling скрытно.

Checked envelope: plane_end=offset+Σ((capacity_extent_i−1)×stride_i)+storage_width.
Все products/additions/align_up checked ДО pointer arithmetic. Scalar один элемент;
empty actual History не вызывает extent−1 underflow. Shared interleaved backing
считается по max plane_end, не N×полный span; отдельные owners суммируются.
PCM packed24 storage_width3, valid24-in32=4, Float64=8. Проверяются alignment
base/strides, logical writable overlap, truncation, actual≤capacity, ring/mask/
coordinate bounds, size_t narrowing. Native layout нельзя наследовать от wire.

Ledger включает descriptor/schema/stream/channel labels/coordinates, ring+mask+
silence rows, buffers каждого output, Converter output+dense staging+limb scratch,
FFT typed workspaces, pool padding/slots/references, retained queue owners и
control snapshots. Peak=live_old+candidate+prepare_scratch+retained_old+adapter_reserved
со checked additions; sharing вычитается только при доказанном single allocation
accounting. Steady-state alone недостаточно. Proposed admission signature:
`AdmissionResult admit_resolved_graph(const ResolvedGraphContracts&, const DeviceCapabilities&, const RuntimeLimits&, const MemoryOccupancy&)`.
Actual limits: bytes/channels/extents/retained slots/coordinates/scratch/stack.
sizeof новой DTO/replay/codec arenas измеряется заново; прежние +8B session evidence
не доказывают numeric budget. Typed failure сохраняет old working/pending/resources.

## 6. Runtime, owning buffers и History delivery

DataFrameView/DataContract заменяют float payload/contract на canonical seams
process_ports, PortFrameView, port_contract, history/apply_history_route того же
PreparedGraph/GraphExecutor. Identity node+port/direction/DeliveryStamp сохраняются.
IngressSpec получает resolved contract и byte/geometry envelope; max_samples не
используется как универсальная длина любого payload. Legacy overload — explicit
adapter в тот же executor, не второй process engine.

History byte-ring row accessor возвращает Vector исходного dtype/semantics.
Route masks используют semantic silence всех planes; closed rows остаются silent
после newest/wrap/Prepare, duplicate не append. Compatible surviving state не
сбрасывается при unrelated edge/output-count change; dtype/layout/channels/grid
change reset только affected nodes. Ring cursor/producer timestamps не type change.

OwnedDataFrame владеет payload И descriptor/schema/identity/planes/coordinates/
row metadata; view() rebases offsets после move/copy. Failed retain/acquire оставляет
out неизменным. Published bytes immutable; pool lifetime длиннее всех handles;
alignment и exhaustion explicit. BufferHandle сам по себе не доказывает lifetime
T или полного descriptor. Нельзя сохранять указатель на mutable runtime ring.

Canonical local History callback borrowed до next tick. Proposed async projection
HistoryRowEvent несёт exact latest row+filled count+contract/lineage/timestamp;
consumer имеет bounded delivery mirror и отмечает gaps, не второй DSP executor.
Full History retained snapshot — explicit measured query outside processing;
generic retain_frame(full History) не вызывается из canonical callback. External
уже immutable owning full History может share-retain owner вместе с route metadata,
но mutable ring нельзя сделать immutable простым retain указателя. T должен
согласовать row/delta/full-query/gap repair. Это конкретное устранение текущих
flatten/full-copy seams, а не обещание, что новый byte header исправит их сам.

D согласовал proposal `convert(plan,input,MutableNumericView&,span<byte> scratch)
noexcept -> NumericResult{status,used_scratch,required_scratch,error_plane,index,loss_flags}`.
Prepare checks dense staging Σ(elements×destination_storage_width)+fixed-limb
workspace. Scratch disjoint input/destination. Input/destination overlap допускается только
с полным disjoint staging, проверенным layout и exclusive writable destination,
не записью в published shared owner. Все результаты stage, затем
validated no-fail scatter logical bytes (padding unchanged); ошибки не меняют
public destination. Dtype specialization выбирается один раз, без per-element
virtual dispatch. History-specific conversion initially unsupported до incremental
row/owned ring strategy. Ordinary Matrix conversion — explicit bounded computation.

Nonfinite seam: current validate отвергает NaN/Inf до Converter, блокируя explicit
replacement. Предложение: structural validate отдельно от value-domain validation;
FFT/Mel finite-only, Converter input допускает IEEE values для своей policy.
Coordinates finite всегда. Legacy validate остаётся прежним. D подтвердил ValueDomain{FiniteOnly,AnyIeee} в support query: integer inherently
finite, FFT/window-multiply/Mel/reduce FiniteOnly; Converter IEEE input AnyIeee,
output FiniteOnly on success. Source finite guarantee не заменяет boundary
validation фактического payload; History проверяет relevant ingress/current row,
не full-matrix scan каждого callback. Default nonfinite reject не ослабляется.

## 7. Exact counters/lineage

FilledFrameCount — Scalar UInt64 default, UInt32 только configured/advertised
capacity≤UINT32_MAX. Filled=min(accepted rows, capacity), не lifetime count.
Lifetime counter при наличии отдельный, overflow explicit refuse/unavailable,
не wrap. Payload не дублируется в float magnitude; preview integer/text exact.
Sequence/generation/epoch/graph_revision/source_sequence/producer_epoch UInt64,
timestamp Int64; clock domain/timebase explicit. UInt64max и >2^53 не проходят
через double. Ring indices не seconds; newest age0 определяется timestamps.
Service R CAS, G accepted Apply, session epoch/token ownership остаются прежними.
Numeric Apply R+1/G+1; reject сохраняет typed working/token/intent/R/G/resources.
Local delivery E/G не переписывает producer lineage; legacy V — separate projection.
Numeric reject не наследует legacy failed-intent exception. Counter overflow до commit.

## 8. Proposed exact ownership/files и migration seams

Это proposal root, не разрешение менять соседние файлы.

| Owner | Proposed files/boundary |
| --- | --- |
| R signal | Новые libs/signal_contracts/include/signal_contracts/data_contract.h,data_frame.h и src/data_contract.cpp,data_frame.cpp; existing frame.h/.cpp legacy seam; buffer_pool.h/.cpp aligned owning |
| R domain | domain/include/domain/graph_definition.h,graph_validation.h; src/graph_definition.cpp,graph_validation.cpp; новый include/domain/processing_contracts.h + src/processing_contracts.cpp для resolution/caps |
| R runtime | libs/dataflow_runtime/include/dataflow_runtime/runtime.h,src/runtime.cpp; tests/signal_contracts_tests.cpp,domain_contract_tests.cpp,dataflow_runtime_tests.cpp,node_routing_tests.cpp; proposed numeric_contract_tests.cpp,numeric_runtime_tests.cpp |
| D kernels | Новые dsp_kernels include/dsp_kernels/{numeric_convert,numeric_reduce,kernel_support}.h и src/{numeric_convert,numeric_reduce,kernel_support}.cpp; tests/numeric_conversion_tests.cpp,numeric_reduction_tests.cpp,pcm_conversion_tests.cpp; frozen scalar_reduce/WLED readonly до отдельного handoff |
| S/R separate handoff | Application DTO/caps/replay/budgets follow-up; frozen processing_control header/cpp/tests/legacy doc не открываются этим N0 |
| T N2/N3 | presentation/ports owned_frame.h, capture/queues/registry/desktop mapper, bdsp_protocol stream_codec.h/.cpp, History row/full envelopes |
| K N2 | control_message/codecs/control.proto/generated BOTH CBOR/Protobuf + caps/arenas |
| P N2 | graph_document/storage/projection versions/unknowns/lossless JSON64/policies |
| Embedded/UI N3, I | Actual device caps/taskstack/NVS, descriptor-driven editor/Sinks; I alone CMake/SDK/manifests/install/build |

Current incompatibilities (static, not test evidence): frame.h float spans/stereo
bool; frame.cpp type→shape/equality/sample_rate/finite assumptions; retain float
copy and incomplete descriptor; runtime.cpp float frame_count line25/vector<float>
line29/sizeof(float) estimates/count float lines714–718; domain ports/caps only
FrameType, double ParameterDescriptor not exact64 authority; Converter absent;
scalar_reduce float spans/result; TypedFrame float/nested history, PortPreview float;
DATA decode float. Старые ABI нельзя смешивать с новыми headers.

Explicit legacy adapters map IDs0..8 в exact old Float32/channels/layout/semantics,
сохраняя old wire/goldens. Count float downgrade только exactly representable;
PCM24/UInt64/custom schema incompatible legacy refuse без mutation/narrowing.
Float32 kernels truthful subset, renderer-only loss/cost вне DSP и exact integer
preview. Удаление transitional canonical float members возможно после всех callers/
owners/capture/DATA/control/storage/UI/device migrations и независимых goldens.
Boundary old-peer adapters остаются пока legacy support обязателен; снятие только
по решению root/user. Не reinterpret float as integer.

Processing/JSON/control schema/DATA feature/structured schema версии независимы.
Новые numeric версии, Converter kind/semantic/schema/feature IDs root назначает
после N0 agreement; здесь числа не присваиваются, stable FrameType0..8/node IDs
сохраняются. Enum ordinals не wire. K/T/P согласуют exact64 lossless CBOR/Protobuf,
JSON typed decimal strings proposal, caps matrix и arenas (old max_types9 не bound).

## 9. Planned acceptance и N0 decisions

Все сценарии NOT RUN. Legacy сначала characterization old float goldens/identity/
History masks/wrap/Stop/session. New behavior RED до implementation; после freeze
I whole build/short affected checks, fresh independent numeric/runtime acceptance.

| Oracle | Cases |
| --- | --- |
| Represent/own | ten dtype×rank0/1/2 min/max/zero, 2^24±1/2^53±1/UInt64max; endian/alignment/access/lifetime после move/next tick/Stop |
| PCM/shape | PCM16/packed24 LE-BE/valid24-in32 two placements/32 badpadding/truncation/bias; mono/stereo/>2 planar/interleaved/padded strides; non-square wrapped/empty History/count/age0 |
| Strict links/caps | dtype/shape/semantic/unit/rate/channel/layout/schema mismatch refuses; only explicit supported Converter succeeds; no silent cast/auto insertion; missing backend refuses before effects |
| Conversion | ties ±0.5/1.5/2.5, saturation/reject, signed min/negative unsigned/exact64, subnormal/−0/NaN/Inf, later-element failure leaves destination unchanged; rounding alone not precision permission |
| Runtime | exact routing/semantic silence/all outputs, persistent closed rows, affected-only reset/count integer; no tick/kernel/callback alloc/full-history copy |
| Memory/atomic | SIZE_MAX products/stride/align/packed width/channels/old+candidate/limbs/retained metadata, pool exhaustion; failed numeric Prepare/Apply retains typed token/working/intent/R/G/resources |
| Boundaries | new contract same across R/D/T/K/P; old bytes untouched; N2 codec/JSON and N3 actual device/UI tests cannot be replaced by N1 host PASS |

Прочитан полностью [numeric-dsp-handoff](numeric-dsp-handoff.md), D concrete
proposal agrees ValueDomain split/channel planes/Packed24logicalInt32/UInt maxabs,
Int64/UInt64 MeanRMS Float64+AllowDeclared, fixed-limb exact cast transforms,
dense staging/no-fail scatter и initial History Converter unsupported. Это
agreement предложения, не implemented support или accepted gate.

Root decisions before N1 handoff: (1) R/D names/ValueDomain/count/support roster;
(2) R/T channel/coordinate bounds + HistoryRowEvent/external-owner/query/gap seam;
(3) R/S/I peak occupancy/replay/caps lifetime and consolidated ABI rebuild;
(4) K/T/P versions/new IDs/lossless JSON64/legacy mappings/arena limits;
(5) exact proposed file ownership and actual embedded kernels/profile.
History Converter refusal не убирает explicit Converter из текущей задачи;
unsupported combinations публикуются честно до incremental implementation.

Риски: ownership/History metadata lifetime и peak staging, double params/preview/
schema exact64, nonfinite reachability без ослабления FFT semantics. Не обещаются
все kernels все dtype. Старые native/queue/full-copy/alloc UNVERIFIED и запрет
повтора blocked publisher experiment сохраняются. **DOC READY**; code/builds не
начаты, application legacy freeze в этом назначении неизменён.


## 10. N1a consolidated source freeze / evidence

Root approved only new signal_contracts data_contract.h/data_frame.h/numeric_policy.h,
src/data_contract.cpp/data_frame.cpp, tests/numeric_contract_tests.cpp and this
appendix. Existing frame/buffer_pool/domain/runtime/application/desktop/schema/
CMake remain readonly; no branches/commits/install/user data/GUI/network tests.
The application legacy four published hashes remain unchanged. N0 statements
above describe the earlier DOC-only phase; N1a is actual compiled foundation,
not an accepted migration or completed N1/N2/N3 gate.

Actual reusable API is in the new headers. D received compiling header and actual
source snapshots with before/after hash requirement; no historical archive ABI.
Ten DType descriptors; numeric rank0..2 with homogeneous channel_order planes,
separate StructuredContract schema/version/encoding/byte bounds. Native/LE/BE,
packed signed24 logicalInt32, LSB/MSB valid24 canonical byte access; rational
regular coordinates/clock and explicit Float64LE coordinate bytes. Caller FrameLimits
has no defaults/universal maxplanes. Actual offsets/strides/ring/masks/coordinate
views are bounded borrowed state. Scalar exactly one plane; FilledFrameCount
contracts accept UInt32/UInt64+Frames only, never float. `load_element<T>` and
`store_element<T>` preserve own-width integer/IEEE bits through memcpy, no byte→T*
numeric lifetime assumption or double integer container. Exact zero/bias silence.

Physical shape/layout/metadata validation is separate from FiniteOnly/AnyIeee
value validation. Canonical valid-bit padding is checked by access/value validation;
structural view validation does not scan full History numeric values. NaN/Inf bits
survive structural transport; selected D kernel/node rejects or replaces them.
`validate_values` scans its explicitly supplied population/view; runtime N1b must
pass relevant ingress/latest row, not repeatedly full retained History. Writable
planar footprints must be disjoint; interleaved planes share one backing with
consistent bounded row/sample stride and channel offsets. This initial physical
layout subset refuses arbitrary overlapping or transposed layouts honestly.

RetentionRequirements computes actual payload + metadata blob with checked
products/add/alignment. Full validate_view also bounds plane/channel/clock/schema/
stream/mask/coordinate metadata aggregate. Pool storage is caller-owned; both
handles must outlive users. Retention failure leaves out unchanged and releases
partial acquisitions; donor aliases or bad pool alignment are refused before copy.
OwnedDataFrame stores immutable metadata arrays established with non-allocating
placement array construction (metadata, not numeric reinterpretation); channel/
clock/schema spans point into retained metadata. copy/move shares bounded handles,
view reconstructs payload/layout/coordinate/mask spans, independent of donor lifetime.
Fixed owner object/slot/stack cost is additional to the reported blobs and belongs
in future R/I peak admission. No hidden heap fallback.

History row accessor uses caller-owned output contract/layout arrays, preserves
per-plane physical strides, old→new cursor and semantic silence; aliases of those
output descriptors are refused. It intentionally borrows original backing.
**REQUIRED N1b:** compact row gather into prepared row-sized immutable slot before
async HistoryRowEvent retention. retain_frame(history_row) still snapshots full
backing, so it is NOT canonical row-cost delivery. Full retained physical History
is explicit query/snapshot outside processing. T agreed this missing seam and
UInt64 filled/row ordinal/lineage/gap/reset/full-query contracts in its N0 document.
No full-copy removal in production runtime/delivery is claimed by N1a.

Shared numeric_policy.h matches D agreed DTOs; exact tagged NumericLiteral stores
Int64/UInt64/Float32/Float64. P request added literal_equal(tag+own-width bits) and
validate_literal(structural IEEE preservation); inactive NaN payload/-0 preserved,
active policy validity remains D. K agrees literal bit-wire mapping, no universal
maxplanes, contract/profile in control versus actual layout/ring/stamp in DATA.
Owning domain graph/parameter DTO remains R N1b, not borrowed DataContract persisted
by P. No new version/registry ID assigned by this implementation.

Author behavioral evidence (NOT independent acceptance):

* Initial foundation scaffolding compile0/run3 at real dtype registry size oracle;
  red-build.log/red.log. Initial full primitive/layout/PCM/ownership cases existed
  before implementation, then minimum implementation and focused edge extensions.
* Separate literal helper behavioral RED compile0/run3 on same NaN bit identity;
  literal-red-build.log/literal-red.log/literal-red-hashes.txt, then bit-exact GREEN.
* Consolidated strict C++20 -fno-exceptions -fno-rtti -UNDEBUG -Wall -Wextra -Werror
  compile0/run0. Current final-build.log has no diagnostics; final-run.log PASS;
  final-status.log records build0/run0/sources_equal=True. Six code/test SHA256s
  identical before/after. Executable launched only after compiler exit0.
* Cases: all ten dtype × rank0/1/2 × Native/LE/BE min/max/zero bit-exact access and
  retain/copy/move, integer >2^53/UInt64max/count; interleaved3 planes/padded/truncated/
  misaligned/alias/stride overflow; PCM packed24 both endian/valid24 bit placements/
  padding/range refusal/unsigned bias; wrapped/empty History/masks/different per-plane
  row strides; metadata/coordinates/channels/clock/stream retained after donor changes;
  structured schema mismatch/length/unavailable/owning bytes; NaN exact bits and
  finite-only separation; complete metadata budget and pool exhaustion/rollback.
  Scoped operator-new observer reports no allocations during bounded retention.
* Intermediate compile/test failures are not PASS. Initial assert macro/test padding
  sentinel and comment splitting were corrected; printing a pre-existing green.log
  after failed compile was stale output, not an executed stale binary. Earlier failing
  comment source hash was not captured; no fabricated historical hash. Later pool-test
  compile failure has exact pool-test-compile-failure.log/pool-test-failure-hashes.txt.
  See recovery-notes.txt. Final receipt uses source-consistent separate final logs.

Exact repeat command from project root:

```powershell
& ./build-refactor-numeric-foundation/run-foundation.ps1
```

Script compiles new test + data_contract.cpp/data_frame.cpp + readonly existing
buffer_pool.cpp/frame.cpp directly with existing MinGW C++20, -static and flags
above; no CMake/shared Ninja/old archives/new tools. It captures six hashes before/
after and refuses execution after compilation failure. Existing legacy sources/
tests preserved readonly; no old behavior migrated, no old full suite rerun.

MinGW x64 measured sizeof: DataContract248, NumericContract240, DataFrameView216,
OwnedDataFrame520, PlaneLayout24, ConversionPolicy120 bytes; sizes.cpp/sizes.log.
Those are host ABI measurements, not portable wire sizes/ESP stack acceptance.
Metadata blobs/payload limits are independent; domain/control replay peaks need
recalculation before N1b/N2. All exact final hashes in source-hashes-before.txt/
source-hashes-after.txt; final doc hash reported separately after this appendix.

Status: **N1a FROZEN / READY FOR I INTEGRATION AND NEW INDEPENDENT NUMERIC REVIEW**.
Whole app/SDK integration belongs I next; foundation not yet wired into domain,
runtime/capture/codecs/storage/editor/device. History compact delivery/incremental
Converter/capability matrix/peak ledger/numeric versions remain mandatory future
assigned scope, not implicit acceptance or optional backlog.


## 11. Independent F1/F2 findings and narrow repair freeze

Independent report build-gate-numeric-foundation-review/review-report.md records
FAIL, two P2 findings. Its original fixture review-oracles.cpp is unchanged.
Earlier author GREEN/I integration is historical, not acceptance of these cases.
Root released only R6/this handoff for repair, N1b remains gated. I explicitly
released its frozen snapshot after finishing builds/hash reads before any R6 edits;
root confirmed that release. No shared Ninja/CMake/DSP/old API/user files changed.
AGENTS reread; pinned DDD same fully read revision, no repeat needed in this chat.

F1: History admitted SpectrumMagnitude on Band (also PCM/window/windowed/Mel
mismatches), then returned a Vector that the same validator rejected. Repaired
with one internal source-vector semantic/axis predicate reused by Vector and
History admission: PCM/window/windowed→Sample, SpectrumMagnitude→Bin,
MelEnergy→Band, generic Numeric→declared valid non-None axis. History/Scalar
as an element remain rejected. history_row validates its source and proposed
row contract before publishing, with no new profile default or data scan.
Positive semantic/axis projections keep their original IDs/dtype/planes/coordinates.

F2: row output layouts could alias v.storage and overwrite borrowed payload.
Repaired preflight considers every written output object/range (NumericView out,
NumericContract out, first plane_count PlaneLayouts) versus source view/contract
objects, payload bytes, plane layouts, channel IDs, clock text, silence masks and
both coordinate spans, plus mutual output overlap. All checks/offset/source/row
contract validation precede writes; failure leaves source and outputs unchanged.
Normal disjoint projection stays borrowed and works with different per-plane row
strides/masks. This repair does not introduce compact row retention/N1b behavior.

TDD evidence in own build-refactor-numeric-foundation:

* Before edits: repair-regressions.cpp compile0/run1, 29 failed expectations across
  24 semantic/axis combinations (positive and negative) and seven legal live-object
  alias fixtures: each writable output in payload, clock metadata, mask, channel IDs,
  coordinates. repair-red-build.log/repair-red.log/repair-red-source-hashes.txt.
* Before edits: immutable original review-oracles.cpp strict compile0; both modes
  run1. unchanged-oracles-red-build.log/unchanged-{history-semantic,row-alias}-red.log.
* Minimum source repair then those cases integrated into numeric_contract_tests.cpp;
  full targeted foundation strict compile0/run0. final-status.log records sources
  equal before/after; final-run.log includes repair regression failures=0.
* Unchanged original fixture strict compile0, history-semantic0
  (contract3/view3/row3 refuses incompatible input), row-alias0
  (source validation0/row result2/source_mutated0). Own unchanged-*-green logs;
  fixture before/after hashes identical. Running these ourselves is author evidence,
  not independent recheck closure or gate PASS.

Actual modified R6 files: data_contract.cpp, data_frame.cpp, numeric_contract_tests.cpp.
Three public headers stay byte-identical to the original freeze; API/ABI/sizeof
and descriptor IDs unchanged. All six final hashes in repair-final-hashes.txt;
repeat script run-foundation.ps1 still compiles actual current sources, runs only
on compiler0, and records before/after equality. Immutable reviewer fixtures never
edited; all reproduction/build output confined to own artifact directory.

Status: **REPAIR FROZEN / READY FOR I AND SAME INDEPENDENT REVIEWER RECHECK**.
F1/F2 are author-repaired, not independently accepted yet. Root owns gate decision;
D direct checks must use these final foundation hashes, not the prior snapshot.
Future N1b compact gather/domain/runtime/incremental history and N2 boundaries
remain required and unstarted in this repair scope.
