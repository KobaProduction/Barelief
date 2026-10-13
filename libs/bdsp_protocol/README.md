# Протокол CONTROL и DATA

[Embedded и обмен](../../docs/architecture/embedded-and-exchange.md) · [Точный протокол](../../docs/reference/protocol.md)

C++20 target `bdsp_protocol` кодирует управление и кадры данных. Он зависит от
`signal_contracts`, `graph_domain`, `wled_protocol` и закреплённого BDSP,
без Qt, ОС transport и DSP. Installed target — `PortableDSP::bdsp_protocol`.

| Публичный заголовок | Назначение |
| --- | --- |
| [numeric_control_message.h](include/bdsp_protocol/numeric_control_message.h) / [numeric_control_codec.h](include/bdsp_protocol/numeric_control_codec.h) | CONTROL schema4, CBOR/Protobuf, graph DTO, capabilities и коррелированные ответы |
| [numeric_data_message.h](include/bdsp_protocol/numeric_data_message.h) / [numeric_data_codec.h](include/bdsp_protocol/numeric_data_codec.h) | DATA payload5, endpoint binding, source/delivery metadata, numeric и structured payload |
| [byte_stream.h](include/bdsp_protocol/byte_stream.h) | `ByteTransport` и bounded `PacketStream` поверх BDSP framing |
| [stream_fragments.h](include/bdsp_protocol/stream_fragments.h) | Фрагментация и bounded сборка полного кадра |

Codec использует caller-owned spans/arenas и явные status/size результаты.
Decoded views заимствуют destination memory. Wire validation проверяет структуру
сообщения; domain resolver и device admission дополнительно проверяют допустимость
схемы перед Prepare. Положительная negotiation/binding обязательна; automatic
legacy downgrade отсутствует.

DATA record kind явно отличает обычный Frame, включая dense Matrix, от
HistoryRow/Snapshot/Query/Empty/Reset. Описательная semantic metadata не выбирает
wire operation. Ring capacity и filled occupancy передаются отдельно.
Session/revision fences и политика lifecycle живут в application/session adapters.

`PacketStream` использует pinned upstream BDSP framing/checksum, принимает
произвольные частичные read и восстанавливается на delimiter после malformed
пакета. Pending output bounded; flush продвигается только на фактический write
count. Callback синхронный, повторный вход в тот же объект запрещён. TCP/UART
реализуют `ByteTransport` во внешних adapters и владеют системными ресурсами.

## Изменение CONTROL schema

Обычная сборка компилирует checked-in `generated/numeric_control.pb.c`.
Offline generator не запускается из configure/build/install. Порядок подготовки
и проверки pinned инструментов описан в [tools/control_codegen](../../tools/control_codegen/README.md).
Используйте Python executable, соответствующий tool manifest:

```powershell
python -S tools/control_codegen/run.py --check-tools
python -S tools/control_codegen/run.py --output-dir libs/bdsp_protocol/generated
```

Вторую команду выполняет владелец codec при изменении schema/options. Перед
сохранением результата генерацию повторяют в отдельных scratch directories и
сравнивают `.pb.c`/`.pb.h` byte-for-byte. Смена BDSP revision требует совместной
проверки packet byte compatibility; pinned dependency сейчас
`2c68056dfaeffba2f34fc64eee9bc9e331e83c7c`.
