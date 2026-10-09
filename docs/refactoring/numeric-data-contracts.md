# Числовые данные: обязательная поправка контрактов

2026-10-07, Europe/Moscow. Пользовательская поправка к scope, **DOC READY / IMPLEMENTATION NOT STARTED**.
Этот документ вместе с поправкой implementation-plan исправляет основание дальнейших срезов;
не объявляет существующий код реализованным или принятым. Исторические bounded PASS сохраняют
свой exact scope, но не подтверждают универсальные numeric contracts. Numeric mapping T3 остановлен
до нового handoff; независимая legacy session policy R может продолжаться.

Перед implementation и архитектурным review каждый отдельный чат полностью читает AGENTS.md и
[закреплённый Clean Architecture / DDD](https://github.com/ArthurKoba/ai-agent-workflow/blob/fdb39e72f330b5e6622895cc31ca8e7b73699629/skills/software-engineering/clean-architecture-ddd.md).
В этом чате редакция прочитана полностью. Справочник определяет границы, а не обязательные паттерны.
Контрактные имена ниже — проектные предложения; новые API, IDs и распределение файлов требует
handoff координатора. Никаких автоматических разрешений на code/schema/generated edits.

## 1. Один контракт, независимые измерения

| Измерение | Обязательное содержание |
| --- | --- |
| Семантика | PCM, window/windowed, Spectrum, Mel, History и quantity/unit сохраняют смысл; одинаковая форма не делает их взаимозаменяемыми |
| Numeric element | Int8, UInt8, Int16, UInt16, Int32, UInt32, Int64, UInt64, Float32, Float64; byte — alias UInt8, публичный bare int запрещён |
| Форма | Scalar rank 0: одно число любого перечисленного dtype; Vector rank 1; Matrix rank 2 с явными extent/axes |
| Representation/layout | Storage format, byte order, alignment, extent, stride, channels/channel axis, interleaved/planar, valid bits, encoding |
| Координаты/часы | Sample rate/timebase/timestamps; bins/bands/частотные координаты; clock domain и discontinuity |
| Structured payload | Schema identity + version + encoding/layout + bounded byte size; отдельная категория от primitive numeric array |

Достаточен компактный descriptor и bounded borrowed storage с проверенными typed accessors;
не нужен generic Tensor framework, virtual boxing каждого числа или отдельные registries в GUI/codecs.
Один portable registry fixed-width dtype и одна domain authority совместимости. Native memory
representation и serialized encoding различаются, wire не зависит от host endian или sizeof(int).
Публичные widths проверяются; для Float32/64 требуется определённая IEEE representation на границе.

Stereo больше не подразумевается через enum range или пару float spans: channel count, axis и layout
явные. Rank обозначает логические оси данных; channels должны иметь явное положение в descriptor
(отдельные planes либо ось Matrix), а не скрытое третье измерение. Например PCM mono Vector[samples],
PCM multi-channel Matrix[samples,channels] либо набор явно описанных channel planes Vector[samples].
Выбранный вариант фиксируется R в API handoff; endpoints сравнивают его точно. History Matrix[time,bins]
с несколькими каналами использует явно ограниченные planes либо явно описанные columns, без rank3.
Обработка rank3 — future extension; GPU 3D surface показывает Matrix и не расширяет DSP rank.

FFT/Spectrum — Vector[bins] на канал, Mel — Vector[bands]. SpectrumMagnitude не равен complex FFT
representation и не равен MelEnergy. История сохраняет семантику элемента/оси исходного вектора;
логические rows идут old→new, newest age=0; физическое кольцо и stride явно описаны.
Индексы не подписываются Hz/seconds без реальных координат/меток.
Scalar FilledFrameCount имеет UInt32 либо UInt64, не Float32. UInt64 >2^53 обязан сохраняться точно.

Structured packet не маскируется под UInt8 Vector: WLED AudioSync schema/version/44-byte LE encoding
должны совпасть отдельно. Custom schema описывает bounded структуру и версию, но не разрешает
автоматическое reinterpretation primitive arrays. Generic validation не разбирает WLED поля;
device protocol adapter проверяет device schema.

## 2. PCM и explicit Converter

PCM может иметь 16/24/32 valid bits независимо от storage. Packed signed PCM24 занимает 3 bytes;
signed valid24-in-Int32 занимает 4 bytes и требует явного положения значимых битов/sign extension.
В контракт входят signedness, valid bits, channel order/count, interleaved/planar, endian, alignment,
strides, sample rate, timebase и normalization. Packed24 — storage encoding, не фиктивный Int24 dtype.
Float PCM явно указывает normalized range; unsigned PCM silence — bias midpoint, а не нулевые bytes.
Тишина семантическая: PCM24 sign/bias и structured packet имеют собственное допустимое encoding.

Несовместимые порты не connect ни в domain, ни в editor, ни через imported/control graph.
Никаких implicit cast, scale, channel merge, resample, pack/unpack или скрытого Converter insertion.
Пользователь выбирает explicit Processor Converter с отдельными input/output contracts.
Numeric cast сохраняет семантику и форму; изменения layout/PCM format выполняет explicit format
conversion. PCM→FFT, Spectrum→Mel, reduction/accumulation — отдельные DSP processors.

Converter сохраняет в processing config все применимые policies:

- rounding: toward-zero или nearest-ties-to-even (default для float→integer);
- overflow/clipping: default reject, optional явная saturation; wrap не поддерживается;
- scaling: default identity; explicit gain/offset и PCM normalization, без implicit normalize;
- nonfinite: default reject NaN/±Inf; explicit replace/clamp только с конечными объявленными значениями;
- precision loss: default reject inexact, optional явно разрешённое округление/потеря точности;
- endian/pack/unpack: точные input/output encoding, valid bits, strides и channel layout.

Политики согласованы: разрешённое float→integer округление само по себе не снимает precision-loss
reject; пользователь должен разрешить округлённый результат. Проверки до опасного C++ cast.
Signed abs minimum, UInt64 narrowing, float overflow/subnormal и отрицательное→unsigned имеют
определённый результат или explicit refusal, без UB. Float conversion не использует double как
универсальный контейнер Int64/UInt64. PCM signed normalization: x/2^(valid_bits-1), обратный
переход со своей rounding/clipping policy; unsigned сначала вычитает midpoint. Все policy значения
сериализуются; default не переписывает явные пользовательские настройки.

Prepare выбирает конкретную bounded kernel specialization один раз; tick не выполняет virtual
dispatch по каждому элементу, не расширяет containers и не выделяет память. Converter output/scratch
заранее включены в checked admission. При runtime conversion failure событие typed unavailable с
диагностикой, без stale output; ошибочный destination не публикуется частично. Никаких OS I/O в DSP.

## 3. Узлы, совместимость и admission

Canonical роли: Source / Processor / Sink; RU Источник / Обработчик / Получатель;
каталог Источники / Обработчики / Выходы. Consumer — прежняя терминология, не новое canonical имя.
Существующие NodeRole уже имеют Sink; stable NodeKind/FrameType/config/wire IDs не переименовываются
ради UI. Если alias/migration потребуется, он explicit и versioned; RU/EN через ui::text/ui::bind.

Accumulator объявляет input/output shape и dtype: Window собирает Scalar PCM→Vector или PCM blocks
с явными channels; Spectrogram Vector[bands]→Matrix[time,bands] сохраняет dtype/семантику/coordinates,
отдельный count output Scalar UInt32/UInt64. Нельзя считать count float ради удобства renderer.
ScalarReduce объявляет operation, population, accumulation precision и result dtype. Maxabs целого
не теряет точность и отдельно проверяет минимум signed; mean/RMS могут выдавать Float32/64 только
по объявленному поддержанному контракту/precision policy. Latest History row и выбранные каналы
задаются явно. Универсальный контракт не обещает каждую kernel для каждого dtype.
Splitter/Switch/Gate сохраняют exact resolved contract всех outputs, включая dtype/layout/units.
Known inactive/detached с clock продолжает semantic silence/history; unknown — unavailable.
Compatible history не сбрасывается при изменении unrelated edges/output count; dtype/layout change
переносится лишь при exact совместимости, иначе сбрасывается только affected state.

Plot nodes — Sinks без processing output или implicit conversion. Renderer adapter может получить
float buffer вне DSP с declared precision loss, стоимостью и bounded budget; exact integer preview
использует целое/text и показывает loss/range, а не превращает uint64 в неточное float значение.
Переключение темы/скрытие view не меняет processing или dtype.

Capabilities объявляют actual поддержанные комбинации semantics/dtype/rank/layout/kernel/backend,
Converter policies/formats и memory/stack/pool limits, а не только набор FrameType. Prepare до
working mutation отвергает unsupported combination или insufficient memory; failed prepare/apply
сохраняет working graph/run intent/revision/resources и valid token по действующему session contract.
Memory estimate использует element width/valid storage/stride/alignment/channels/ring masks/retention,
checked product/addition и peak staging рядом с old bundle. Borrowed view нельзя сохранять async;
bounded owner удерживает bytes и весь descriptor/schema/coordinates lifetime. Нет full-history copy
на canonical processing callback; explicit snapshot остаётся отдельной измеренной операцией.

ESP32 optimized Float32 backend сохраняется; legacy default FFT2048 и существующие explicit4096
configs не переписываются. Float64 dtype в общей модели не обещает аппаратный Float64 FPU или FFT64
на ESP32. Отсутствующие kernels/converters отвергаются до Apply; реализация каждой заявленной
комбинации и реальный stack/memory budget подтверждаются отдельно, host build не hardware PASS.

## 4. Реальный gap и адресаты handoff

Это статическое чтение текущих исходников 2026-10-07, без build/tests. Отсутствие numeric Converter
подтверждено inventory domain NodeKind/parameters и поиск по собственным domain/libs/adapters.
Свежий T3 author handoff: numeric mapping STOP. Частичные
`adapters/control/include/control/processing_message_mapper.h`,
`adapters/control/src/processing_message_mapper.cpp`,
`adapters/desktop/include/control/control_session.h`,
`adapters/desktop/runtime/src/control_session.cpp`, `tests/desktop_control_session_tests.cpp`
не приняты. Owning requests/rebased views/framing могут быть сохранены после review, но nine type
aliases/26kinds/uint32 params относятся только к legacy. Max capability types9/codec arena K1 bounds
должны измениться вместе с versioned schema/caps и пройти отдельные boundary tests; не расширять
accepted K14/schema1 PASS. Независимая legacy policy R и её session-size evidence отдельны от payload.

| Реальный seam / source | Gap и корректировка | Предлагаемый owner / соседний документ |
| --- | --- | --- |
| [frame.h](../../libs/signal_contracts/include/signal_contracts/frame.h), [frame.cpp](../../libs/signal_contracts/src/frame.cpp) | FrameView/HistoryView float spans; shape по FrameType, stereo bool, нет dtype/layout; compatible сравнивает прежний PortContract | R: [node-runtime-contracts](node-runtime-contracts.md), новый typed descriptor/view/validation |
| [buffer_pool.h](../../libs/signal_contracts/include/signal_contracts/buffer_pool.h), [buffer_pool.cpp](../../libs/signal_contracts/src/buffer_pool.cpp) | Byte pool существует, retain/materialization предполагает float payload; alignment/lifetime и typed ring нужно повторно доказать | R, T для owning delivery |
| [graph_definition.h](../../domain/include/domain/graph_definition.h), [graph_validation.h](../../domain/include/domain/graph_validation.h) | PortDescriptor содержит только semantic/quantity/unit; Routing/Reduction выбирают FrameType; Converter отсутствует; caps только kinds/types/version | R: единая authority descriptors/compatibility/capabilities, explicit Converter parameters |
| [runtime.cpp](../../libs/dataflow_runtime/src/runtime.cpp), [runtime.h](../../libs/dataflow_runtime/include/dataflow_runtime/runtime.h) | State vector<float>, frame_count float, memory sizeof(float), scalar count contract и routing history float | R: prepared specialization/typed silence/count/state transfer/admission/noalloc |
| [scalar_reduce.h](../../libs/dsp_kernels/include/dsp_kernels/scalar_reduce.h), [dsp.h](../../libs/dsp_kernels/include/dsp_kernels/dsp.h) | Scalar helper принимает/выдаёт float; double accumulation не решает exact uint64; optimized backend ограничен | D: [dsp-device-codecs](dsp-device-codecs.md), kernels/Converter numerical policies |
| [owned_frame.h](../../presentation/ports/include/pipeline/owned_frame.h), [desktop delivery](desktop-delivery-contracts.md) | TypedFrame vector<float>/nested history; previews peak float; pools/envelopes должны сохранить весь новый contract | T: canonical owned bytes/descriptors/queues/renderer adapter/registry |
| [stream_codec.h](../../libs/bdsp_protocol/include/bdsp_protocol/stream_codec.h), [stream_codec.cpp](../../libs/bdsp_protocol/src/stream_codec.cpp) | DATA decode caller span<float>; legacy0..8 wire не выражает произвольный dtype | T: explicit legacy adapter + negotiated new DATA representation |
| [control_message.h](../../libs/bdsp_protocol/include/bdsp_protocol/control_message.h), [control.proto](../../libs/bdsp_protocol/schema/control.proto) | schema1 WireFrameType0..8 и 12 parameters; dtype/Converter/caps отсутствуют; CBOR/Protobuf float params не numeric payload contract | K: [control-codec-contracts](control-codec-contracts.md), versioned schema parity/goldens; T [desktop-control-contracts](desktop-control-contracts.md) mapper |
| [graph_document.cpp](../../adapters/desktop/src/graph_document.cpp), [project storage](project-storage-contracts.md), [storage control](storage-control-contracts.md) | graph2/processing1 projection и migration не включают dtype/layout/Converter | P + S/R: versioned JSON migration, typed DTO/capabilities, preserve unknowns/atomicity |
| [profile.h](../../libs/embedded_support/include/embedded_support/profile.h), [session.h](../../libs/embedded_support/include/embedded_support/session.h) | Semantic caps и float path не подтверждают device combination support | R/embedded owner: actual admission, taskstack/NVS; I SDK/ESP manifests |

Точные новые headers/signatures/file ownership определяет координатор до implementation; таблица
не переassign чужие файлы. Соседние документы здесь read-only; владельцы обязаны добавить amendment
и согласовать seams, не стереть прежние freezes/review evidence. I alone обновляет baseline ledger.

## 5. Версии и обязательная последовательность

Existing FrameType IDs0..8, DATA/framing bytes и legacy float goldens сохраняются. Explicit legacy
adapter восстанавливает прежние Float32/channel/layout/semantics; legacy Scalar count float остаётся
только в compatibility representation с явными границами точности. Новые uint64/count/PCM24 данные
не идут в legacy format через silent narrowing. Unsupported/lossy downgrade возвращает error.
Processing version, JSON document/graph versions, control schema, DATA payload version и schema
identity structured packets — независимые version axes. Proposals processing2/control-schema2 и
новый DATA version/feature IDs согласует root; здесь числа/registry IDs не закрепляются.
Negotiation включает dtype/layout/capabilities до Prepare/subscription; старый peer получает legacy
только при точно representable contract. CBOR и Protobuf должны выражать одинаковый новый DTO;
exact Int64/UInt64 wire не проходит через double. JSON integers за пределами exact parser range
хранятся явным versioned typed decimal encoding либо lossless integer parser, выбор P/K фиксирует
handoff; routine default proposal — typed decimal strings для 64-bit numeric values.

| Corrected slice | Результат / gate до дальнейшего зависимого применения |
| --- | --- |
| N0 contract correction | R/D/T/K/P подтверждают descriptor, storage/layout, policies, version/caps seams; root утверждает exact API/IDs/ownership |
| N1 typed foundation + explicit Converter | R/D реализуют fixed dtype/rank0..2/structured contracts, views, strict links, Converter/kernel support matrix, exact count/reduction, byte-width budgets/prepared routing; TDD + characterization legacy + fresh independent numeric/runtime acceptance |
| N2 numeric boundaries | T/K/P/S расширяют owned delivery, DATA/control codecs BOTH formats, DTO/JSON migration/storage/revision, explicit legacy adapters; проходит malformed/exact64/negotiation/rollback matrix; fresh independent protocol/storage acceptance |
| N3 device + UI integration | Editor использует единую compatibility, explicit Converter/RU-EN roles, renderer cost; actual ESP caps/backend/NVS/taskstack; I whole desktop build/short affected integration + SDK/ESP existing toolchain; fresh independent device/UI acceptance |
| N4 final reconciliation | Final срез5 объединяет старые outstanding native/memory/queue obligations и новые numeric gates; ни один accepted numeric feature не обходит N1/N2/N3 |

Это встроенная коррекция прежних срезов1–4, а не optional backlog после финальной приёмки.
Базовые legacy session/token/TTL/revision policy, pinned codec dependencies, atomic-save/unknown-field
mechanics и bounded historical routing/identity/WLED proofs могут сохранять acceptance в прежнем
scope. Signal/runtime/type compatibility/memory/delivery/protocol/GUI/device numeric scope открывается
заново. Codec schema1 bounded PASS не означает schema2/numeric completeness. Заблокированный native
publisher эксперимент не повторять/обходить; прежние UNVERIFIED и Slice5 full-copy/alloc остаются.

## 6. Матрица проверки каждой исправленной границы

Все пункты ниже обязательные planned tests, **NOT RUN** в этом DOC-only assignment. Generic dtype
representation/retention и несовместимость проверяются для всех десяти типов и rank0/1/2; вычисления
— для каждой advertised supported combination, остальные дают deterministic refusal.

| Dimension | Целевые cases / доказательство |
| --- | --- |
| Signedness/width | min/max/zero десяти dtype; negative→unsigned, widening/narrowing, signed abs min; exact Int64/UInt64 ±limits, 2^24±1, 2^53±1 и UInt64 max через view/retain/DTO/JSON/BOTH codecs |
| Cast policy | ties ±0.5/1.5/2.5, toward-zero, saturation/reject, float precision loss/overflow/subnormal/−0, NaN/±Inf reject/explicit replacement; unchanged destination on errors |
| PCM/layout | PCM16, packed24 LE/BE, valid24-in32 sign/bit alignment, PCM32 min/max/silence; unsigned bias; normalize roundtrip; mono/stereo/multichannel planar/interleaved, padded/misaligned/invalid stride, truncated bytes |
| Shapes/semantics | Scalar every dtype, Vector FFT/Mel bins, Matrix non-square/wrapped History [time,bins]; shape/dtype/semantic/unit/rate/channel/layout mismatch cannot connect; WLED/custom schema mismatch vs UInt8 Vector refusal |
| Explicit nodes | Converter enables only declared pair/policy; no auto insertion/cast; ScalarReduce exact result/population, Window/Spectrogram input/output/count dtype; fanout/gate/switch exact all-output contracts, known silence/unknown unavailable |
| Runtime/memory | checked extents×stride×width/channels/alignment/rows, scratch/old+candidate/pool/retained owner exhaustion; failure preserves working/token/resources; tick+Converter+callback zero alloc/no full-history copy; next-tick/Stop/prepare/async lifetime, affected-only state reset |
| Control + DATA | BOTH CBOR/Protobuf new dtype/layout/Converter/caps DTO goldens and rejection parity; DATA new representation endian/stride/schema goldens, malformed/truncated/fragmented limits; unsupported versions/combination refuse before mutation; legacy0..8 bytes unchanged/no lossy downgrade |
| Storage/version | legacy unversioned/graph2/processing1 explicit migration; new numeric version exact64 + converter policies roundtrip; future/malformed version/no overwrite; unknown fields/IDs/explicit defaults retained; atomic failure + up/down loss refusal, no history/tokens persisted |
| Device + UI | actual per-kernel caps including Float64 unsupported ESP, PCM format/converter/pool/stack limits; default2048 and explicit4096 unchanged; editor/import/control same incompatibility diagnostics; all plot Sinks, renderer-only loss/cost; RU/EN + centralized theme in requested affected UI cases |

Каждый owner сначала добавляет failing targeted test для нового поведения, characterization для
legacy переноса. После freeze I строит приложение и короткие затронутые unit/integration (один Ninja).
Независимый review запускает свои целевые oracles; автор GREEN не закрывает gate. Native GUI/hardware
проверяются только по конкретному сценарию/отдельному запросу; offscreen не доказывает native3D.
Полный набор/FPS/soak/Linux/музыка/динамики не добавляются к каждому срезу.
