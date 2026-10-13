# Платформенные адаптеры desktop

Здесь находятся Qt/OS-реализации capture, byte transport и реестра компонентов.
Публичные заголовки расположены в `adapters/desktop/include`; composition roots
приложений подключают их к переносимым application ports.

`QtTcpByteTransport` использует QTcpSocket, `QtSerialByteTransport` — QSerialPort.
Операции выполняются на owning Qt thread без ожидания bytes/write completion.
`QtPacketChannel` использует bounded BDSP PacketStream, сохраняет неотправленный
suffix при частичной записи и ограничивает входные/выходные очереди.
Serial endpoint задаётся явно; handshake и reconnect принадлежат session owner.

Реестр [Aggregator](aggregator/README.md) публикует доступность и capabilities.
Аудиоданные передаются напрямую между выбранными ядрами. Qt transport и registry
не меняют математический смысл схемы и не выполняют DSP.

См. [desktop runtime](../../../docs/architecture/desktop-runtime.md),
[обмен между ядрами](../../../docs/architecture/embedded-and-exchange.md),
[протокол](../../../docs/reference/protocol.md) и
[сборку и запуск](../../../docs/reference/build-and-run.md).
