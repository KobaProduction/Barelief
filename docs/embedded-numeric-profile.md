# Embedded numeric SDK и профиль ESP

Актуальная composition: `examples/esp32_streamer/main/main.cpp`. SDK использует то же portable ядро, что desktop: `signal_contracts`, `dsp_kernels`, `dataflow_runtime`, domain/application и `bdsp_protocol`. Сборка — C++20, без Qt, exceptions и RTTI. ESP-IDF, UART/TCP, NVS, heap и task lifecycle находятся в адаптерах и entrypoint. Компоненты прошивки компилируют исходники SDK; отдельного firmware DSP или исполнителя схемы нет.

`PreparedGraph`/`GraphExecutor` выполняет граф. `GraphLifecycle` и единственный `ProcessingControlService` обслуживают Prepare, Apply, параметры, Start, Stop и ExplicitPersist. `SyntheticPipeline::prepare_source`/`tick_source` готовит и выдаёт только Native PCM; stock entrypoint подаёт его в этот же executor. Standalone `SyntheticPipeline::prepare`/`tick` остаётся отдельным SDK-примером composition и не используется как второй исполнитель в прошивке.

## Контракты и границы

Canonical editions: document 2, graph 3, processing 2, CONTROL 2, DATA 4. Поддержка комбинации определяется фактическим runtime/device query, а не перечнем dtype или декартовым произведением capabilities. Неизвестная/неподдерживаемая комбинация отклоняется до замены рабочего графа.

Stock source — один `AudioSource`, node 0, stereo Float32 planar PCM, 8000 samples/s, блок 256 samples/channel. Capture hook обязан соблюдать этот контракт; synthetic source выдаёт восемь изменяющихся тонов без воспроизведения через динамики. Default RAW-граф: AudioSource → Converter Native→FixedLE → NetworkOutput. Spectral-граф: Window → Hann → StereoFFT → SpectrumMerge → Mel 8 → Converter → NetworkOutput. FFT window по умолчанию 2048; 4096 доступен при соответствующем compiled DSP maximum. History выключена. Внутренний Native формат не сериализуется в DATA: преобразование на wire задаёт явный Converter в графе.

Один CONTROL parser/PacketStream обслуживает CONTROL и DATA. `NumericSession` в externally framed mode получает готовые Fragment31 через `accept_packet`, а передаёт через injected sender; она не читает UART/socket. `publish` и `drain_packets` вызываются после возврата CONTROL poll, без reentry из его callbacks. FrameView и decoded Message заимствованы только на время callback; очереди владеют ограниченными буферами.

## Бюджет stock composition

`Profile` библиотеки задаёт переносимые admission ceilings. Параметры stock firmware отличаются от библиотечных defaults и собираются в entrypoint:

| Ресурс | Stock ceiling / reservation |
| --- | --- |
| Общая память | `min(STREAMER_MEMORY_KIB * 1024, streamer_host_memory_budget())`; configured default 320 KiB, heap измеряется до создания Device |
| CONTROL complete wire / decode arena / total text | 4 KiB / 8 KiB / 4096 bytes |
| Shared packet payload / read chunk | 1024 / 512 bytes; payload согласуется с peer, DATA chunk ≤ payload − 8 |
| Shared CONTROL owner | `ControlSession::storage_requirements(config).persistent_bytes`: два wire owners, private primary arena, packet/read storage и объекты |
| Replay и control scratch | два session snapshots + один scratch; каждый размер вычисляется `application::control_snapshot_bytes(caps)` |
| Persistence wire/scratch | три буфера по 4 KiB, плюс два 48-byte record headers |
| Persistence arena/staging | два буфера по 8 KiB и bounded workspace |
| Shared cold owner | `max(sizeof(PersistenceBuffers), external_staging_bytes)`; persistence и CONTROL staging получают исключительную lease |
| Суммарный `control_bytes` ledger | persistent CONTROL + shared cold owner + три DTO snapshots + размеры service/lifecycle/executor/helper objects + 4096 bytes headroom |
| Adapter reservation | `sizeof(Device)` без отдельно учтённых replay slots и двух DATA frame owners + source PCM 2048 bytes + UART driver 8 KiB + 4096 bytes margin |
| DATA frame / fixed queue slots | 4096 bytes / 2 |
| Task stack / node-plan ceiling | 32 KiB / 32 KiB |

Оставшаяся память ограничивает graph/source payload, scratch, retained working state и cold replacement peak. Embedded wrapper сохраняет фактический resource profile runtime query, включая fixed overhead и общий immutable zero pool. Node-plan, owned DTO metadata и общий peak учитываются раздельно. Stock admission ограничивает граф восемью узлами, 16 связями, двумя портами и четырьмя параметрами на узел, 64-byte field text и 8192-byte owned metadata. Stack reservation — программный предел admission, не измерение ABI или high-water mark платы.

Default memory hook читает текущую свободную 8-bit-capable heap. Host может задать реально предоставленный composition бюджет после остальных владельцев памяти. 320 KiB — configurable ceiling, а не обещание доступной памяти. Task stack 32 KiB выделяется из bounded heap после успешной подготовки; reservation учитывается один раз. При недостаточной памяти stock bootstrap возвращает exhausted до Prepare/изменения рабочего состояния. Размеры объектов зависят от ABI; contiguous allocation и фактический task stack проверяются на конкретной плате отдельно.

Предыдущий фиксированный CONTROL ledger 1120 KiB превышал internal-only budget 320 KiB и заменён вычислением владельцев выше. Shared cold storage не объединяет primary decode arena с входными/выходными сообщениями: staging lease освобождается сразу после decode, до application callbacks и ExplicitPersist. Persistence использует пять disjoint spans; при 4 KiB wire и 8 KiB arena их record/scratch/arena/workspace storage составляет 28832 bytes на ABI32 до дополнительного alignment. Оба codecs предварительно измеряют полный reply envelope применяемого графа; предел 4 KiB не обещает поддержку всех комбинаций максимальных cardinalities.

Компактный профиль прошёл bounded host integration для stereo FFT2048 с тем же CONTROL TCP/UART и бюджетом 320 KiB. Host ABI ledger: CONTROL 106505 bytes, adapter 25960, fixed frame owners 8192, остаётся runtime peak ceiling 187023 bytes; cold owner внутри CONTROL — 28896 bytes, каждый DTO snapshot — 17153. Эти значения относятся к проверенному host ABI, а не к измерению свободного heap платы. Первоначальная подготовка и atomic old+new replacement имеют разные peaks: в обоих ordinary случаях замена была отклонена как exhausted с сохранением рабочего графа. RAW/FFT2048/FFT4096 с явно заданным бюджетом 1024 KiB прошли также успешную замену. Эти расширенные результаты не увеличивают scope ordinary-профиля.

## Admission канала и смена графа

`graph_link_admission` измеряет через существующий DATA4 codec максимальную геометрию каждого NetworkOutput и суммирует все выходы, включая typed silence отключённых узлов. Для локального paced source PCM ограничен source block cadence; Window-derived/Scalar outputs консервативно учитываются с частотой `max(60 Hz, sample_rate / block_samples)`. Window может рекурсивно выдать несколько кадров за один source call, поэтому RAW-profile cadence не заменяет cadence применённой схемы.

Расчёт включает фактический negotiated packet capacity, 8-byte fragment header и консервативную framing overhead. Stock preparation adapter проверяет этот ledger до executor Prepare; подписка повторно проверяет уже применённый граф с согласованным packet size. Локальная composition не допускает NetworkInput или иной источник с неизвестным pacing. Это admission заданного link budget, а не измеренный TCP throughput/UART timing.

Успешные Apply/Stop инвалидируют старый DATA binding и очередь доставки. Selective DATA discard сохраняет CONTROL session и незатронутый TCP/UART resource binding; reconnect выполняется при transport/session failure. Новый binding создаётся по актуальному applied revision после положительного Subscribe/Ack.

## Explicit persistence и reboot

Stock entrypoint открывает NVS namespace `numeric` и использует `NvsProcessingStorage`; host может предоставить persistence/restore hooks. NVS adapter зависит от `portable_sdk` и `nvs_flash`, portable persistence — только от storage port и существующих numeric codecs.

Два отдельно committed blobs `processing_a`/`processing_b` хранят processing graph. Record содержит editions, sequence, длину и CRC; transport/session/request/token/revision не сохраняются. Restore читает оба слота, проверяет codec и actual device admission, выбирает последнюю допустимую схему и при повреждённом/прерванном candidate использует предыдущую last-known-good. Restore не пишет, не стирает и не ремонтирует partition. Future/неподдерживаемые records не перезаписываются сохранением.

На reboot восстановленный граф проходит тот же Prepare/Apply/Start service path; default graph используется только при отсутствии сохранённого record. Flash writes происходят исключительно при ExplicitPersist применённого графа. Prepare, Apply, Start, processing tick и чтение состояния не сохраняют автоматически; exact replay ExplicitPersist не повторяет write.

## Проверенное и границы evidence

Предыдущая авторская целевая проверка 2026-10-07 собрала embedded persistence/admission/session/fake-NVS tests и получила 3/3 PASS. Она покрывала оба codecs, exact64 metadata, read-only last-good restore, повреждения/future records, interrupted commit, ExplicitPersist/replay через тот же service, shared DATA fragmentation/backpressure/no-heap и source-only PCM через actual executor. Этот результат предшествует последним изменениям compact owners, codec measurement и runtime resource ledger; текущие затронутые проверки и независимая compact-приёмка завершены в scope, указанном ниже.

`tests/embedded_sdk_component/numeric_bootstrap_tests.cpp` включает настоящий stock `main.cpp` с fake IDF/NVS. Обновлённый integrated batch 2026-10-07 получил 5/5 PASS за 1.23 s: RAW/spectral2048/spectral4096 с бюджетом 1024 KiB и ordinary stereo FFT2048 TCP/UART с 320 KiB. Все случаи проверяют State через оба codecs, 128 source/DSP ticks, Stop/Start, ExplicitPersist и свежий bootstrap restore без лишних writes; недостаточный actual host budget 64 KiB отклоняется до service/NVS. Ordinary случаи дополнительно подтверждают отказ atomic replacement с сохранением работающего графа. Evidence: `build-refactor-integration/numeric-compact-bootstrap-ctest.log` и `build-refactor-integration/numeric-compact-footprints.log`. Полный reply envelope FFT-графа измерен в 2430 wire bytes / 5904 arena bytes; normalized persisted graph — CBOR 2170 или Protobuf 2126 wire bytes / 5800 arena bytes. Host composition подменяет platform resources; task/network execution на chip не измеряются. Текущий production main с MAX stack guard дополнительно собран: component 6/6 PASS за 1.09 s (`numeric-compact-bootstrap-current-ctest.log`). Независимые compact architecture/SDK и runtime/control/fake-device отчёты завершены со scoped PASS: `build-final-architecture-review/review-report.md` и `build-final-runtime-protocol-review/review-report.md`; аппаратный scope не расширен.

Предыдущий actual Xtensa ESP-IDF cross-build собрал SDK, transport, NVS и main archives; evidence: `build-refactor-integration/numeric-esp-cross-build.log`. Текущая compact Xtensa сборка завершена: `build-refactor-integration/numeric-compact-native-build.log`, SDK/NVS/TCP main archives и UART main object, build0, effective C++20; правила и формат артефактов независимо проверены. Это проверка compiler/link boundaries, не запуск firmware на chip. Installed canonical SDK consumer отдельно собран и запущен host toolchain. Author PASS и cross-build не заменяют независимую приёмку, реальные heap/task-stack измерения, throughput и аппаратное испытание отключения питания.
