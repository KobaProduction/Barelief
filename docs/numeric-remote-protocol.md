# Числовой remote-протокол

Этот документ описывает текущий canonical путь: CONTROL schema 2, HELLO envelope 2,
DATA payload 4 и delivery grammar 1. Форматы старых графов и старые remote bridge
не участвуют в этом пути. Нормативные поля и проверки находятся в
[CONTROL schema](../libs/bdsp_protocol/schema/numeric_control.proto),
[CONTROL DTO](../libs/bdsp_protocol/include/bdsp_protocol/numeric_control_message.h),
[DATA DTO](../libs/bdsp_protocol/include/bdsp_protocol/numeric_data_message.h) и
[DATA codec](../libs/bdsp_protocol/src/numeric_data_codec.cpp).

## Границы исполнения и транспорта

Один `PreparedGraph`/`GraphExecutor` исполняет схему устройства.
`ProcessingControlService` управляет Prepare, Apply, параметрами, Persist,
токенами и replay. Успешный Apply сразу активирует обработку до завершения
процесса. Wire-номера Start=6 и Stop=7 зарезервированы и отвергаются;
Persist=8 и Release=9 сохраняются. Qt сервер и ESP entrypoint обращаются к этому же
service; отдельного сетевого DSP-исполнителя нет.

Граф содержит `execution_interval`: сокращённую положительную дробь секунд
с Int64 numerator и UInt64 denominator, по умолчанию 1/60. Допустимый диапазон
1 ms–60 s. Это интервал анализа по времени сигнала; частота захвата и отрисовки
настраиваются отдельно. Отсутствующее поле принимает значение по умолчанию.

`ControlSession` использует один bounded BDSP `PacketStream` для CONTROL и DATA.
Auxiliary callback передаёт заимствованный `PacketView` только на время вызова.
Обработчик сохраняет нужные данные во владеющий bounded буфер и отправляет ответы
после выхода из callback. Второй parser на том же TCP/UART потоке не подключается.

TCP и UART передают одинаковые BDSP пакеты с существующей COBS-ZPE framing,
checksum и delimiter. Qt использует `QtTcpByteTransport` либо
`QtSerialByteTransport`; firmware — адаптер `ByteTransport`. Чтение и запись
неблокирующие, частичная запись сохраняет точный неотправленный suffix.

| BDSP ID | Назначение |
| --- | --- |
| 21 / 22 | HELLO / HELLO_ACK |
| 23 / 24 | CONTROL Command / Reply в выбранном codec |
| 25 | Фрагмент полного CONTROL сообщения |
| 26 / 27 | DATA Subscribe / положительный Ack |
| 28 | DATA Events, включая входной owned History snapshot |
| 29 / 30 | Холодный History Query / Snapshot response |
| 31 | Фрагмент полного DATA сообщения |

## HELLO envelope 2

HELLO не кодируется CBOR или Protobuf. Числа little endian, без ABI padding.
Строка — `u16 byte_length` и UTF-8 bytes; identity непустая, максимум 256 bytes.

| HELLO, порядок полей | Тип |
| --- | --- |
| envelope version = 2 | u16 |
| nonce, ненулевой | u64 |
| CONTROL schema = 2 | u32 |
| codec mask | u32 |
| максимальное полное CONTROL сообщение | u32 |
| максимальный BDSP packet payload | u32 |
| ожидаемая device identity | length-prefixed string |

| HELLO_ACK, порядок полей | Тип |
| --- | --- |
| envelope version = 2; echoed nonce | u16; u64 |
| status, 0 означает принятие | u8 |
| CONTROL schema; выбранный codec | u32; u8 |
| лимит полного сообщения; лимит packet payload | u32; u32 |
| writable | u8, 0 или 1 |
| выданные client, session, epoch | три u64 |
| device identity; diagnostic | две length-prefixed строки |

Codec mask: CBOR = 1, Protobuf = 2, BOTH = 3. При BOTH выбирается CBOR;
Protobuf доступен при положительном пересечении его бита. Неверная schema,
непересекающиеся codecs, другая identity или недостаточный envelope отвергаются.
Packet payload согласуется как минимум сторон, в диапазоне 512..65535 bytes.
Полное сообщение и packet payload — разные лимиты: CONTROL размером 96 KiB
может проходить через пакеты размером 4096 bytes.

Сервер не уменьшает свой полный CONTROL envelope ниже заранее проверенного
профиля: предложение клиента должно покрывать его configured wire limit.
При отрицательном ACK schema/codec/лимиты/writable/выданная identity равны нулю;
device identity и diagnostic остаются строковыми полями. Текущие отказные status:
2 — schema, 3 — codecs, 4 — limits, 6 — session busy, 8 — device identity.

Nonce относится к физической negotiation, а не к глобальному порядку клиентов.
В живой сессии повторный HELLO запрещён её состоянием. `reset()` закрывает
application session, очищает framing и nonce high-water mark. Новое соединение
может иметь меньший или повторившийся случайный nonce: сервер выдаёт новый
`session`, по которому старые CONTROL и DATA отклоняются. Epoch обозначает
incarnation application service, а не очередной TCP reconnect.

## CONTROL 2: codecs, команды и ревизии

CBOR и Protobuf используют одну типизированную schema/traversal и одинаковые
ограничения. Идентификаторы dtype, rank и альтернатив проверяются символически;
пересылка C++ enum ordinal или struct memory не является wire format.
`NumericLiteral` сохраняет точный signed/unsigned integer либо float bit pattern.
Int64/UInt64 не проходят через JSON double; Float32 и Float64 не объединяются.

| Command symbol | Команда |
| --- | --- |
| 1 / 2 | Capabilities / State |
| 3 / 4 | Prepare / Apply |
| 5 | SetParameters |
| 6 / 7 | Reserved; rejected |
| 8 / 9 | Persist / Release |

Prepare содержит canonical graph: document 2, graph 3, processing 2, control 2,
data 4. Mapper глубоко владеет параметрами, строками и контрактами, затем
domain resolver и actual capability query проверяют граф. JSON, OS I/O и codec
не входят в prepared processing path.

У service одна глобальная pending preparation. Токен принадлежит конкретной
session и имеет TTL из application profile. Apply проверяет owner, token,
graph identity и expected revision. Отказ не подменяет working graph.
Одинаковый request с тем же содержимым использует replay; изменённый request
с повторным ID получает conflict. Replay удерживает immutable working plan
через `working_numeric_snapshot()`, а не создаёт граф после успешного Apply.
Persist является отдельной явной командой для applied graph revision.

`Message.expected_revision` и `Reply.state.revision` — **CONTROL revision**.
`EngineCommand::ApplyGraph.expected_revision` desktop API — **graph revision**:
remote adapter сначала сверяет её с текущим графом, затем использует CONTROL
revision для Prepare/Apply. Public Start/Stop отсутствуют. DATA всегда ссылается
на applied graph revision; смена соединения не останавливает DSP.

CONTROL extension tag 1024 содержит 16 bytes: LE `u64 graph_revision`, затем
LE `u64 persisted_graph_revision`. Это поле не заменяется `State.revision`.
Capabilities описывает реальные configured combinations, kernel/backend,
value domain и plan/payload/scratch/stack/peak resources. Отдельный набор dtype
или видов узлов не разрешает их произвольное декартово произведение.
Stack resource является reservation, а не измерением native ABI stack.

## DATA 4 и delivery grammar 1

DATA начинается с LE magic `0x5441444e` (`NDAT`), `u16 payload=4`,
`u16 grammar=1`, символа kind, flags, value-domain symbol и нулевого reserved
byte. Flags = 0 означает полный descriptor, flags = 1 — accepted reference.
Далее идут семь LE u64 полей envelope: nonce, session, delivery_sequence,
delivery_epoch, delivery_generation, revision, request.

Endpoint задаётся `(graph_id, node_id, direction, port_id)`. Payload сохраняет
dtype, rank, extents, logical channel order, representation, normalization,
semantic, quantity/unit, clock, coordinates, planes/strides и FrameStamp.
Full descriptor и reference layout определяет codec, а не `sizeof(Message)`.

Delivery epoch проверяется против выданной service epoch. Delivery generation
согласуется со stamp generation. Положительный delivery sequence отсеивает
повторную и более старую доставку в данной session/revision. Source sequence,
producer epoch, generation и timestamp сохраняют собственный смысл;
delivery sequence не служит счётчиком строк History или временем оси.

| Symbol group | Значения |
| --- | --- |
| dtype 1..10 | Int8, UInt8, Int16, UInt16, Int32, UInt32, Int64, UInt64, Float32, Float64 |
| rank 0 / 1 / 2 | Scalar / Vector / Matrix |
| DATA encoding 1 / 2 / 3 / 4 | FixedLE / FixedBE / PackedSigned24LE / PackedSigned24BE |
| kind 1..7 | Frame, HistoryRow, Snapshot, Query, Empty, Reset, Unavailable |

CONTROL разрешает Native descriptor symbol 0 для обработки. DATA Native symbol
не имеет. Desktop adapter явно проецирует Native descriptor в FixedLE,
сериализует те же значения/биты и восстанавливает исходный processing contract
у принимающего владельца. Dtype, shape, axes, channels и normalization не
меняются. На big endian машине используются точные load/store bits.
Это транспортное кодирование; изменение числового типа/семантики требует
явного Converter. Embedded network profile может требовать FixedLE/FixedBE
и отвергать Native endpoint при admission.

### Положительная привязка порта

Subscribe26 использует DATA `Message{kind=Unavailable}` с полным wire contract,
endpoint и stream ID, без payload. Envelope содержит nonce/session/applied
graph revision/service epoch, `delivery_sequence=1`, `delivery_generation=0`,
положительный request, помещающийся в u32. Для History также задаётся capacity.
Ack27 возвращает эту же привязку; неподходящий контракт не даёт положительный Ack.
Accepted reference имеет `(binding=request, contract=1)`.

References разрешаются только через caller-owned `ReferenceContext` с теми же
nonce/session/revision и принятыми bindings. Codec допускает до 256 bindings;
Qt endpoint/server ограничивают собственный набор 128. Qt отправляет полные
descriptors, а embedded publisher может использовать принятые references.
Feature symbols: Numeric=1, Structured=2, Rows=4, Query=8. Поддержку конкретного
пути определяют положительная версия/привязка и actual profile; отдельного
неописанного feature поля в Subscribe нет.

### History: realtime, холодный снимок и ingress

Realtime History передаётся компактным `HistoryRow`, без копирования всей матрицы.
Контракт строки — Vector с semantic исходного history element; полный History
contract передаётся отдельно. `history` хранит filled, capacity и настоящий
accepted row ordinal. Empty/Reset/Unavailable являются явными событиями.

Desktop HistoryRow metadata использует DATA extension TLV tag 1024:
LE u16 tag, LE u32 length=27, затем version=1, flags, mask byte,
LE u64 logical_row, LE u64 physical_row и 8 bytes time coordinate.
Flags bit0/bit1/bit2 означают наличие time coordinate, mask и silence.
Эта DATA TLV namespace отличается от CONTROL extension tag 1024.
Координаты сохраняются; индекс Mel не становится герцами, индекс строки — секундами.

Query29 — холодный запрос полного History, с ненулевым request и без payload.
Snapshot30 возвращает реальный runtime ring, его filled/capacity/ordinal,
координаты и silence mask. Полная матрица не переходит в realtime callback.
Qt `EngineSnapshot.numeric_frames` владеет результатами такого запроса.

Внешний History ingress отправляется через Events28 с `kind=Snapshot` и
ненулевым opaque request; это не ответ Query. Desktop sender требует отдельный
`publish_numeric_history(endpoint, owner, real_ordinal)`. Сервер глубоко
удерживает frame в ограниченных пулах и вызывает `feed_numeric_owned` после
выхода из service lock. Runtime получает **оба** обязательных аргумента:
`Message.history.ordinal` и arena retention owner, удерживающий pools/slots/storage.
Borrowed History отвергается до изменения runtime. Ordinal не вычисляется из
delivery sequence, filled count или ring oldest. Runtime проверяет точный delta;
discontinuity/смена producer позволяют явный snapshot rebase.

## Фрагменты, память и выборочный сброс

CONTROL25 header — 29 bytes: LE u16 fragment version=1, u64 session,
u64 message sequence, u32 total, u32 offset, u8 original ID и u16 count.
Chunk ограничен `min(60000, packet_payload_capacity-29)`. Offset должен точно
продолжать текущую assembly; session/sequence/total/kind также проверяются.

DATA31 использует общий 8-byte header: LE u32 total и LE u32 offset,
затем chunk до `min(60000, packet_payload_capacity-8)`. Начальный offset 0
начинает/restarts bounded assembly. Полное сообщение остаётся в DATA envelope,
а не в переносимом DSP. Заимствованная assembly живёт только до следующего
accept/reset; выходной frame удерживается в pool до возврата callback.

`discard_outbound_data()` сбрасывает только Events28/Snapshot30/Fragment31:
encoded DATA owner и целиком ещё не записанный пакет. Если запись уже началась,
остаток этого immutable BDSP пакета завершается; возвращается would_block,
и caller продолжает poll того же транспорта. CONTROL reply/command/fragments,
deferred request, parser и service identity сохраняются. Caller сначала
очищает собственные DATA queues/assembly/encoded owners. Apply не требует
разрыва всего TCP/UART; отрицательный Apply не очищает working graph.

Async queues хранят владельцев, не borrowed FrameView. Qt per-peer queues
ограничены 8 элементами, receive arenas — 16 slots с фиксированным размером.
Contexts удерживают arena после уничтожения endpoint. Переполнение не расширяет
контейнер на processing path. Reconnect и принятый Apply очищают
соответствующую доставку; другие устройства/CONTROL sessions не сбрасываются.
При потере remote session Qt отправляет typed Unavailable для bound ports,
затем обнуляет snapshot epoch/revisions/graph/cold owners и публикует stopped
status. Session/handler generations запрещают queued callbacks старому клиенту;
доставка queued snapshot/reply использует текущий authoritative snapshot.

## Текущие composition envelopes

Числа ниже — ceilings, не обещание одновременной достижимости всех максимумов.
Runtime admission, transport codec и memory/throughput проверяются вместе,
до working mutation. Существующие generic codec presets не заменяют profile.

| CONTROL setting | Qt numeric remote | Stock full ESP composition |
| --- | --- | --- |
| wire / decode arena | 512 / 512 KiB | 4 / 8 KiB default |
| total text / one string | 128 KiB / 256 B | 4 KiB / 64 B |
| nodes / edges | 512 / 2048 | 8 / 16 |
| ports / parameters / records | 8192 / 16384 / 65536 | 32 / 32 / 1024 |
| capability rows | 512 | 4 |
| packet payload | 65535 B local ceiling | 1024 B local ceiling |
| session storage / peer budget | 8 / 2 MiB | `storage_requirements(config)` / 96 KiB |

Default negotiation/CONTROL/CONTROL-fragment timeout — 5000 ms. Application replay,
scratch, retained plans and DSP reservations учитываются отдельно.
Generic сервер preflight проверяет actual capabilities BOTH codecs и резервирует
`4096 + capabilities_wire + 8*max_owned_metadata` wire,
`capabilities_arena + 4*max_owned_metadata` arena. Компактный stock ESP использует
точную `reply_envelope_fits`: единственный preparation adapter проверяет каждый
кандидат до executor Prepare, включая boot, restore, local/remote команды и
SetParameters. Проверяется полный graph + immutable capabilities с максимальными
скалярами, всеми diagnostics и revision extension в BOTH codecs; wire, arena,
text и cardinality limits остаются обязательными. Компактный admission flag
разрешён только при этой composition гарантии; динамическая замена capabilities
в такой session запрещена. Generic `device_limits()` сам по себе эту гарантию
не даёт. Снижение CONTROL envelope не меняет DSP контракт stereo FFT2048.

Session хранит два wire буфера: неизменяемый TX и RX. После полной проверки и
копирования decoded views в primary arena RX можно использовать как encode
scratch; при заблокированном TX он удерживает один deferred request. Новый
fragmented request не перезаписывает deferred request. Вторая decode arena
остаётся непересекающейся staging областью. Stock composition берёт её по
exclusive lease из общего cold NVS workspace и освобождает до mapper/service
callbacks, поэтому Persist использует тот же workspace в другой фазе. Primary
arena и wire buffers остаются private. `storage_requirements` учитывает ABI
объектов, выравнивание и заданный read chunk (stock 512 B); external workspace,
allocator overhead, replay, DTO owners, DSP и task stack входят в общий memory
ledger отдельно. Эти ceilings не являются доказательством общего RAM peak.

В host component проверке текущего полного stereo FFT2048 максимальный BOTH
reply занимает 2430 B wire, 5904 B decode arena и 407 B text. Persisted graph:
2170 B CBOR / 2126 B Protobuf wire, 5800 B arena, 327 B text. Поэтому default
NVS использует 4 KiB wire / 8 KiB arena; его 28896 B cold workspace разделяется
со staging CONTROL по времени. Wire/arena ceilings конфигурируются через
Kconfig; фактический профиль по-прежнему проверяет каждый кандидат.

[Connected component tests](../tests/embedded_sdk_component/numeric_bootstrap_tests.cpp)
проверили реальный stock entrypoint для TCP и UART с full stereo FFT2048 и
бюджетом 320 KiB: общий accounted peak 326522 B из 327680 B, включая полный
32 KiB task stack. На проверенной host ABI резервы составили 106505 B CONTROL,
25960 B adapter и 8192 B frame pool; граф требует 185865 B из оставшихся
187023 B. В этом бюджете полная атомарная замена графа отвергается до working
mutation и сохраняет работающий граф. В extended 1024 KiB профиле проверена
замена RAW / FFT2048 / FFT4096. Это author component evidence расчёта и
поведения, а не измерение heap на физическом ESP32: firmware использует
фактически доступную память и может безопасно отказать при меньшем бюджете.

Desktop DATA codec ceiling: message 900000 B, payload/elements 800000,
metadata 65536 B, coordinates 32768 B, 16 planes, 2000 History rows.
Desktop engine более строго ограничивает собственные frame resources и
проверяет network contract через DATA measure до Prepare. ESP DATA limits
выводятся из `embedded_support::Profile`; stock RAW/spectral profile имеет
History OFF и проверяет суммарный wire rate всех network sinks с учётом
packet fragmentation и UART 8N1/TCP budget.

Qt server identity по умолчанию `numeric-desktop`; stock ESP — `numeric-esp32`.
TCP endpoint задаётся host/port; client `configure_network` поддерживает
`host`, `port`, `device_identity`. UART задаётся явно `SerialEndpoint`;
конфигурация поддерживает `port_name`, `baud_rate`, `device_identity`.
Изменение адреса reconnect затрагивает этот endpoint.

Stock firmware выбирает UART либо host-owned accepted TCP descriptor через
Kconfig/host hooks. Configured memory KiB — потолок; фактически доступный budget
сообщает `streamer_host_memory_budget()` (default free 8-bit heap).
Наличие PSRAM не предполагается. Недостаточные memory/link/profile ресурсы
дают отказ, а не скрытое уменьшение admitted contract.

## WLED output boundary

`WLEDEncode` — pure processor, выдающий structured contract
`wled-audio-sync`, version 1, `audio-sync-44-le`, ровно 44 bytes.
`WLEDOutput` — отдельный adapter sink. Его registered graph fields:

| Field | Default / допустимость |
| --- | --- |
| destination | `255.255.255.255`, непустая строка; IP проверяет Qt composition |
| interface | `0.0.0.0`, непустая строка; bind проверяет Qt composition |
| port | 11988, integer 1..65535 |
| cadence_ms | 17, integer 1..60000 |

`NumericWledOutputPolicy` хранит одну private 44-byte копию latest packet.
Precise timer берёт один due packet за cadence без catch-up burst.
Destination counter начинается с 0, проходит 255→0 и расходуется на каждую
send attempt, включая неудачную. Производительский packet остаётся неизменным.
Bypass выхода подавляет отправку; DSP продолжает исполнение. Counter принадлежит
выходному адаптеру и не управляет жизненным циклом ядра.

Stale наступает при monotonic elapsed **>300 ms**; ровно 300 ms ещё fresh.
Known Unavailable немедленно очищает latest и следующий due send передаёт
валидную silence. Неверный packet отвергается без замены latest или продления
freshness. Неудачная/неполная UDP запись останавливает output policy и
публикуется как adapter error. Destination, cadence и socket lifecycle находятся
в composition, не в pure WLED codec и не в BDSP DSP/framing path.

## Исходники и проверка

- [CONTROL session](../adapters/desktop/runtime/src/numeric_control_session.cpp),
  [mapper](../adapters/control/src/numeric_processing_message_mapper.cpp).
- [Qt endpoint](../adapters/desktop/runtime/src/numeric_remote_endpoint.cpp),
  [server](../adapters/desktop/runtime/src/numeric_control_server.cpp),
  [wire ownership adapter](../adapters/desktop/include/control/numeric_remote_wire.h).
- [Embedded profile](../libs/embedded_support/src/profile.cpp),
  [shared-mux DATA session](../libs/embedded_support/src/session.cpp),
  [stock entrypoint](../examples/esp32_streamer/main/main.cpp).
- [WLED policy](../adapters/desktop/include/wled/numeric_wled_output_policy.h).
- Targeted fake seam tests: `numeric_control_session_tests` (BOTH codecs,
  replay/two clients/rejected Apply, selective DATA fence, lower/repeated nonce
  reconnect и stale-session replay), `numeric_wled_output_policy_tests`
  (cadence, counter, unavailable, exact300/301, failure, clock rollback).
- Connected desktop discovery/disconnection oracle: `registry_discovery_tests`.
  Runtime owned History scenarios и отдельную resource/lifetime границу описывает
  [numeric runtime handoff](refactoring/numeric-runtime-handoff.md).

Перечень тестов описывает scope, а не присваивает PASS. Итоговые команды,
артефакты сборки и независимая приёмка фиксируются в
[progress checklist](refactoring/progress-checklist.md).

## Автоматические desktop-маршруты и инвентаризация тестов

`NumericRoutingAdapter` — Qt-адаптер control plane, отдельный от DSP и JSON-free
prepared path. Composition root владеет адаптером и удаляет его раньше движка.
Декларация входа хранится в `nodes[].adapter` (в visual3 —
`nodes[].internal-data.adapter`); `network.routes` с локальным `node_id` имеет
приоритет. `provider_uuid` + `stream_id` разрешаются только через зарегистрированный
CONTROL2/DATA4 publisher и точный endpoint из `metadata.streams`. Потеря directory
не допускает использования старого cache или прямого fallback. Прямой маршрут
требует `host`, `port`, `remote_endpoint={graph_id,node_id,direction,port_index}`.
Частично заполненный adapter можно сохранить/применить; до полного binding он
остаётся unavailable с диагностикой. Неверные типы/UUID/endpoint отказаны атомарно.

Пределы: 128 маршрутов, 32 соединения, 8 History endpoints на соединение. Кадры
входят через владеющий `feed_numeric_owned` с исходными producer stamps,
discontinuity и retention owner. History rows запускают ограниченный cold snapshot
запрос; полная History передаётся с точным ordinal и владельцем arena. Транспортный
разрыв не создаёт новый DSP generation и не останавливает обработку маршрута.
Удалённый Apply с непустым adapter отказан: CONTROL2 не передаёт desktop routing
metadata, поэтому молчаливое удаление этой настройки недопустимо.

В GUI composition `NumericDesktopHost` размещает движок, CONTROL/DATA, маршруты
и собственный registry-клиент в worker event loop. GUI-клиент registry передаёт
только явное намерение connect/disconnect с копией endpoint; mutable discovery
cache между потоками не передаётся. GUI получает bounded owning mailboxes, а
холодные query и синхронный ingress используют thread-safe API движка. Передача
данных и CONTROL не требуют обработки событий GUI.

Удалённый receiver готовит pool до подписки: число слотов равно числу выбранных
портов плюс 97 (очереди Host/endpoint и слот для замены retained кадра). Максимум
225 слотов при 128 портах. Пределы DATA4 payload/metadata сохраняются, включая
допустимый padding: 800000/65536 байт на слот. На платформе с восьмибайтовым
`BufferSlot` это 104731792 байт для 24 портов и 194749200 байт для 128, включая
два массива слотов. Бюджет одного endpoint — 192 MiB суммарно для живых поколений;
контексты и retained кадры удерживают заряд после reconnect. При достаточной
ёмкости текущий pool используется повторно для новой подписки и смены revision;
это не освобождает занятые слоты и не меняет immutable кадры старых контекстов.
Новый binding готовится атомарно; превышение бюджета оставляет старый binding.
При известном графе `subscribe_numeric_frames` возвращает отказ с диагностикой.
Рост ёмкости, требующий одновременно двух поколений сверх бюджета, может
потребовать disconnect и освобождения retained contexts перед новой подпиской.
После retirement reconnect готовит новое поколение в том же бюджете. DSP/DATA
callback не расширяет pool. Неограниченное сохранение старых кадров потребителем
не входит в гарантию ограниченного резерва.

Входящая очередь remote и исходящая очередь server хранят до 32 ordinary frames
и отдельно до 8 History events. Ordinary frames заменяются по endpoint на месте:
порядок обслуживания портов сохраняется при обновлении latest значения. Поток
одного порта не вытесняет другой выбранный порт постоянным повторным добавлением.
Переполнение History сохраняет bounded gap/cold-resync поведение. Гарантия
проверяемого blocked-GUI сценария ограничена 24 портами; отдельная проверка
повторной подписки на 60 портов использует обычную обработку GUI событий и
последовательную подачу. Число 128 задаёт admission maximum, а не утверждение
доставки всех 128 портов одной пачкой при заблокированном GUI.
На ACK подписки ожидается один outstanding binding; следующий отправляется после
подтверждения точного binding ID. Через 500 ms неподтверждённая подписка повторяет
тот же ID. Это сохраняет ограниченную очередь ACK сервера при одновременных
`readyRead` и timer callbacks. Session/revision/epoch fences и проверки ACK
действуют также для повторной отправки.

| Исходный тест | Текущее решение и проверяемый сценарий |
| --- | --- |
| `network_runtime_tests.cpp` | Canonical NumericEngine/adapter: direct DATA4 exact64, атомарный отказ config, continuous Apply, приоритет explicit routes, partial/empty adapter без stale ingress; owning History/cold snapshot с точным ordinal, compatible Apply и idle reconnect с сохранением старых строк. |
| `distributed_transport_tests.cpp` | Адаптирован: три реальных NumericEngine/CONTROL2 сервера + registry, два hop DATA4, fanout, selective unavailable, новый publisher incarnation при том же UUID, независимая ветка, отказ cache/fallback после потери registry. Дополнительно три production Host и worker registry проверяют 160 exact64 кадров при blocked GUI, remote CONTROL mutation и retirement directory до GUI callbacks. Старые GraphProgram/CONTROL1 CLI фикстуры не исполняются. |
| `engine_transport_tests.cpp` | Реальный TCP, два клиента, fragmented 80KB UInt64 без потери точности, retained arena после disconnect, reconnect, stale/future Apply, continuous lifecycle, read-only и отказ unsupported adapter; ограниченная admission нового поколения arena, отказ при retained старом поколении и восстановление после освобождения context; доставка demand-selected конечного выхода большого графа. |
| `stream_transport_tests.cpp` | Structured canonical WLEDOutput в production Host: actual localhost UDP без GUI events, cadence, stale silence, counter/wrap и continuous lifecycle. Диагностический `--same-thread` использует тот же oracle для воспроизведения зависимости от GUI; его ожидаемый RED хранится отдельно от обычного CTest. Прежние TypedFrame publisher/subscriber заменены DATA4 TCP/route тестами выше. |
| `desktop_serial_stream_tests.cpp` | Адаптирован: fake QIODevice UART, DATA4 chunks503, exact64 через реальный NumericEngine, partial writes, callback Stop/reset с большим coalesced suffix, новый producer, explicit serial config/unknown fields. Не аппаратная UART приёмка. |
| `stream_fragment_tests.cpp` | Сохранён portable opaque BDSP fragment oracle; добавлены chunks1016/503, bounded assembly и reset. Формат внешних fragments не равен версии внутреннего payload. |
| `aggregator_tests.cpp` | Сохранён: async registration/snapshot/refusal/timeout/reconnect/cancellation, malformed wire и изоляция slow peer/backpressure; дополнен explicit connection/disconnection intent для worker mirror. |
| `registry_discovery_tests.cpp`, `capabilities_tests.cpp` | Сохранены canonical CONTROL2/DATA4 metadata/discovery и disconnect/incompatible advertised versions. |
| `desktop_byte_transport_tests.cpp`, `desktop_packet_socket_tests.cpp` | Сохранены Qt transport/framing/partial write/backpressure и socket lifetime oracles. |
| `numeric_control_session_tests.cpp` | Сохранён canonical BOTH CONTROL, replay, fences, compact staging и callback lifetime. |
| `numeric_wled_output_policy_tests.cpp`, `wled_protocol_tests.cpp` | Сохранены pure policy/wire проверки; UDP интеграция выше дополняет их, но не подменяет. |

Таблица — инвентаризация реализации и назначение текущих registration targets.
PASS/independent acceptance фиксируются только по фактической integrated сборке
и запуску в итоговом evidence; эта запись сама по себе их не присваивает.
