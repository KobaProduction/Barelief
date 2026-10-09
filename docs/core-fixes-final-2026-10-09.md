# Исправления ядра после углублённого анализа

9 октября 2026 года. Scope: concrete findings трёх core reviews, непрерывность,
PCM/DSP/History, bounded доставка и desktop/embedded composition. Два исполнителя
завершили свои блоки; координатор интегрировал presentation и выполняет FINAL.
Итог проверок: release build и полный desktop CTest **98/98 PASS** (202.98 s),
SDK build/CTest **32/32 PASS**, installed fake-IDF component **6/6 PASS**,
installed consumer build/run exit 0. Ограниченные синтетические CLI запуски
с default 1/60 s и override 25 ms завершились exit 0. Implementation, integration
и независимая source/host приёмка этого блока завершены. Hardware/native graphics
acceptance остаётся отдельной и не заявлена.

## Исправленное поведение

- Единственный PreparedGraph/GraphExecutor работает до teardown процесса.
  Apply автоматически активирует корректную схему. Публичные Start/Stop удалены;
  wire symbols 6/7 зарезервированы и отвергаются. Disconnect и скрытие GUI
  не выключают DSP; MCU acquisition/DSP отделены от CONTROL/сетевой задачи.
- `execution_interval` задаётся в ядре как положительная сокращённая Ratio
  секунд, по умолчанию 1/60, диапазон 1 ms–60 s. Sample clock, анализ и GUI
  cadence разделены; deadlines сохраняют дробную sample phase. JSON/CONTROL
  передают точные целые числитель/знаменатель. CLI поддерживает override.
- Bypass операции с одинаковым полным входным/выходным контрактом даёт identity;
  изменение формы/смысла даёт типизированную тишину. Acquisition продолжает
  отсчитывать samples. Выходной bypass подавляет отправку, сохраняя обработку.
- Merge сохраняет смысл quantity/unit/coordinates; FloatUnit PCM согласован
  с native Window/FFT. Исправлены очистка unavailable PCM, Window continuity,
  History batch masks/rebase/reset и атомарная validation SDK helpers.
- Mean/RMS и FFT устойчивы к ложному overflow на больших конечных значениях;
  downstream ошибки не маскируются успехом. Mel проверяется независимым
  математическим oracle для regular/explicit Hz и magnitude/energy semantics.
- Loss-sensitive сигналы используют bounded owning FIFO, GUI preview — latest.
  Переполнение маркирует реальный gap; copied callbacks проверяют generation
  и cancellation. Demand конечного выхода учитывается до помещения в очередь.
- Полная Qt outgoing queue возвращает `would_block`, сохраняя непереданные
  байты. TCP adapter буфер 64 KiB входит в общий peer resource budget.
- Cold Prepare, CONTROL codecs и I/O вынесены из DSP mutex. Apply проверяет
  base identity и capability/reply admission. Несколько CONTROL-команд одной
  pump итерации не удерживают processing lock через следующую команду.
- MCU ресурсный ledger учитывает фактические owners без двойного подсчёта
  встроенных input objects. Профиль 320 KiB сохранён; producer stack 24 KiB,
  transport stack 16 KiB. Убрано реально неиспользуемое publisher assembly;
  State observation не занимает CONTROL session/replay slot.
- GUI автоматически применяет схему; повторный Apply того же JSON обновляет
  revision/epoch preview. Исправлена ширина narrow inspector; актуальные GUI
  проверки используют отдельные preview cards и graph-view sink menus.

## Приёмка и ограничения

Portable и desktop/control/firmware изменения получили независимое source review
другого владельца после исправлений. Source acceptance не заменяет runtime PASS.
Последний State polling fix принят независимо: интервал отсчитывается от ответа,
чтобы долгий State round trip оставлял время binding/DATA.
Qt backpressure и deterministic FIFO overflow regressions также приняты отдельно.
Host regression занимает 18.85 s: включает 60 реальных DATA bindings, reconnect
и local/remote History recovery. Его прежний generic quick timeout 15 s заменён
на bounded integration timeout 60 s; функциональные assertions сохранены.

SDK собран без Qt, exceptions и RTTI; установленный consumer исполняется успешно.
Installed component manifest/main компилируются с fake IDF. Шесть тестов включают
RAW, FFT2048, FFT4096 и штатные 320 KiB TCP/UART bootstrap/Persist/reboot.
SHA256 установленного firmware main.cpp совпадает с текущим исходником.

Evidence хранится отдельно от пользовательских config.json/output:
`C:\Users\Koba\AppData\Local\Temp\qt-core-fixes-final-20261009-09a90644885d46cead5eba857b19a72e`.
Актуальные SDK logs: `sdk-final-closure-build.log`, `sdk-final-closure-tests.log`,
`sdk-final-closure.xml`, `component-final-build.log`, `component-final-tests.log`,
`component-final.xml`, `sdk-consumer-final-build.log`, `sdk-consumer-final-run.log`.
Desktop logs: `release-closure-build.log`, `release-closure-tests.log`,
`release-closure.xml`. Ограниченные app runs: `cli-final-default.log`,
`cli-final-25ms.log`, `bounded-app-final-results.log`. GUI приложения запускались
в отдельных offscreen CTest fixtures; пользовательский GUI не управляется.

Не заявлены физический MCU запуск, allocator fragmentation/OOM устойчивость,
native stack high-water, WCET и гарантированные 60 итераций/сек на устройстве.
Offscreen GUI не подтверждает native OpenGL визуальную приёмку. Эти ограничения
не отменяют проверенную переносимую архитектурную границу, но требуют измерений
на целевой плате и конкретном processing graph.
