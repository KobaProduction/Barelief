# Проверки C++ и границ модулей

## Трассировка отзывчивости GUI

Диагностическая цель `application_startup_trace` принимает `--source PATH`,
`--output NEW_DIRECTORY`, `--gui-fps 60`, `--preview-hz 60` и `--measure-ms 8000`.
Она работает с копией проекта и синтетическим восьмитональным источником,
без воспроизведения через динамики. Оригинальный файл проверяется по SHA-256.
`BARELIEF_TRACE_GESTURES=1` добавляет панорамирование схемы, повторные клики,
перемещение узла и панорамирование числового графика. События направляются
только виджетам тестового процесса, без перемещения системного курсора.

`trace.json` содержит длительности `gui.delivery_drain`, `gui.presenter_snapshot`
и `editor.navigation`; `summary.json` — состояние обработки и renderer.
В `interactions.json` записываются выбранные цели, фаза, `dispatch_ms` обработчика
и `interval_ms` между таймерными жестами с целевым интервалом 16 мс. Это не замер
задержки физического устройства ввода. Для сравнения используйте одинаковые
проект, частоты, backend и нагрузку машины. Offscreen/software подтверждает
GUI-сценарий, но не производительность нативного GPU renderer.

## Инструменты навигации и рефакторинга

Для навигации по символам и рефакторинга сначала используйте подходящие
встроенные инструменты среды/Codex, если они доступны и работают. Serena MCP
используется только при отсутствии, недоступности или неисправности этих средств.
Наличие обычного текстового редактора не доказывает доступность семантических
операций. Если встроенных средств достаточно, Serena не запускается.

Резервная Serena устанавливается глобально через `uv tool install -p 3.13 serena-agent`
и подключается для Barelief через `.codex/config.toml` к общему локальному
HTTP-серверу `serena_barelief` (`http://127.0.0.1:8766/mcp`). Унаследованный
STDIO-сервер `serena` здесь отключён: отдельный процесс на каждый чат не нужен.
Подключение `serena_barelief` также отключено по умолчанию. Включайте его только
для резервного сценария; после завершения остановите ненужный сервер, учитывая
активные задачи других клиентов общего экземпляра.
Запуск: `cmake --build --preset release --target serena-mcp` или
`./scripts/start_serena.ps1` из PowerShell. Скрипт запускает скрытый сервер;
именованный mutex удерживается до его завершения и запрещает второй экземпляр,
включая одновременные попытки запуска. Резервный сервер запускается явно только
при необходимости; обычная сборка приложения от него не зависит. После изменения
MCP-настроек проверьте доступность инструментов в текущем чате: запись конфигурации
сама по себе не подтверждает подключение. При сбое после переноса проекта сначала
проверьте рабочий путь чата. Клиенты получают отдельные сессии через
`initial_instructions` и активируют проект `Barelief`, используя общий индекс.
Настройки проекта
и кэш находятся локально в `.serena/`. Для C++ нужна актуальная compilation
database в корне проекта: после configure обновляйте её из
`cmake-build-release/compile_commands.json`. Первые межфайловые запросы требуют
индексации clangd; пустой ответ до её завершения не подтверждает отсутствие ссылок.
Serena помогает с навигацией/рефакторингом и не заменяет следующие проверки.

Для повторных C++ semantic edits используйте штатный режим clangd `--sync`
в пользовательском `ls_specific_settings.cpp.ls_extra_args`. Он обрабатывает
запросы последовательно; background index продолжает работать отдельно.
Быстрое открытие, изменение и закрытие файла может оставить асинхронные задачи
разбора ждать остановленный preamble thread и занять все рабочие слоты.
Увеличение timeout или успешный первый обзор символов не проверяют этот сценарий.
Проверка подключения включает несколько последовательных semantic edit/read
циклов на отдельном временном C++ файле, затем символы и ссылки проекта.

При зависании Serena сначала сохраните диагностику и выясните причину.
Не заменяйте семантический рефакторинг ручными правками без решения проблемы.
Не повторяйте mutation с неизвестным результатом: дождитесь исхода и учтите
уже сохранённые изменения. Перезапускайте только принадлежащие своей задаче
процессы. Для Python MCP задайте `PYTHONUTF8=1`, `PYTHONIOENCODING=utf-8`
и `PYTHONUNBUFFERED=1`, чтобы stderr не нарушал UTF-8 транспорт Codex.

Через pre-commit запускаются clang-tidy, Clazy и Chromium checkdeps.
Python и QML не проверяются линтерами. Python используется только для запуска
инструментов. Правила include-зависимостей находятся в стандартных файлах DEPS.

В корневом `.clang-tidy` настроены production-пороги и строгая политика
тестовых builders, fixtures, oracle и инфраструктуры:

- `readability-function-size`: максимум 20 операторов функции;
- `readability-function-cognitive-complexity`: максимум 15.

Физические строки не ограничиваются. Эти пороги — политика проекта,
а не универсальный стандарт числа «мысленных действий».

Установка в отдельное окружение:

```powershell
python -m venv .agent-workspace/quality-venv
./.agent-workspace/quality-venv/Scripts/python.exe -m pip install -r scripts/requirements-quality.txt
./.agent-workspace/quality-venv/Scripts/python.exe -m pre_commit install --hook-type pre-push
```

Перед проверкой подготовьте release-сборку обычным способом. CMake preset
включает экспорт `cmake-build-release/compile_commands.json`; clang-tidy
использует оттуда include paths и defines. Generated Qt headers должны быть
созданы сборкой. Сам pre-push не собирает приложение.

Для C++ headers без собственной записи launcher выбирает доказанный C++ owning
контекст; произвольная соседняя C-команда больше не используется. Hook явно
задаёт язык C++ и начальный стандарт C++23 через стандартные
`--extra-arg`/`--extra-arg-before`; собственный `-std` compilation database
остаётся приоритетнее начального стандарта. Пороги и состав проверок не меняются.
Описание аргументов: [LLVM clang-tidy](https://releases.llvm.org/22.1.0/tools/clang/tools/extra/docs/clang-tidy/index.html).

`scripts/run_clang_tidy.py` запускает отдельный процесс на каждый выбранный
C++ файл через общий `scripts/quality_execution.py`. Заголовки проверяются как
main file, сохраняя диагностику inline-функций без расширения header filter.
Порог операторов, cognitive complexity и повышение замечаний до ошибок задаёт
прежний `.clang-tidy`. По умолчанию число workers ограничено доступными CPU и
75% свободной RAM при оценке 768 MiB на worker. Аргумент `--jobs N` имеет
приоритет над `QUALITY_JOBS=N`; фиксированного ограничения в четыре worker нет.

Оба hook запускаются один раз через `always_run` и `pass_filenames: false`.
Общий selector получает область изменений до фильтрации инструментами, поэтому
изменение только заголовка также запускает соответствующую проверку.
Pre-commit буферизует консольный вывод; текущий прогресс доступен в ignored
`.agent-workspace/quality/progress-<tool>-<PID>.status.json` и JSONL-журнале.
Статус содержит `jobs`, `running`, `completed`, `total`, `failed` и время.
Ошибка процесса или недоступный инструмент сохраняют ненулевой итог gate.

Hook `quality-tools` запускает regression для selector, scheduler и обоих
wrappers при изменении quality scripts, их тестов, конфигурации или CI.
Тот же hook входит в полный CI/pre-push проход. В его изолированной среде
закреплён `identify==2.6.20`, чтобы проверять соответствие прежнему C++ file
filter; пропущенный regression считается ошибкой, а не успешной проверкой.

```powershell
./.agent-workspace/quality-venv/Scripts/python.exe -m pre_commit run --all-files --hook-stage pre-push
```

Pre-push определяет C++ scope через `scripts/quality_scope.py`, а содержимое
берёт из checkout. Незакоммиченные правки могут отличаться от отправляемого
commit. CI выполняет проверку в своей чистой редакции. Созданный workflow
не означает успешный серверный запуск или настроенные branch protections.

Исключения vendor/generated и пользовательского `output/` перечислены в
`.pre-commit-config.yaml`. Старые нарушения не получают baseline.
Линтер не заменяет проверку контрактов, архитектурное ревью и тесты.

## Область проверки C++

В автоматическом режиме selector читает `PRE_COMMIT_FROM_REF` и
`PRE_COMMIT_TO_REF`; wrappers также принимают `--from-ref` и `--to-ref`.
Проверяются изменения отправляемого Git-диапазона вместе со staged, unstaged
и untracked изменениями checkout. HEAD должен соответствовать целевому ref.
Прежние общие исключения из `.pre-commit-config.yaml` применяются централизованно.

Изменённые заголовки сохраняются в scope clang-tidy и добавляют owning translation
units из compilation database. Обратные зависимости строятся рекурсивно по
literal includes и include roots. Macro includes, `include_next`, modules,
response files, неразрешённые includes или заголовок без владельца требуют
полного scope. Неявные include paths toolchain не угадываются.

Переименование/удаление C++, изменения общих CMake/DEPS/quality scripts или
конфигурации проверок также требуют полного scope. При отсутствии надёжных
Git-данных, compilation database или диапазона выбирается полный проход.
Кэшированный PASS и baseline старых нарушений не используются.

`--scope full`, `--full` или `QUALITY_FULL=1` проверяют всё применимое текущее
дерево, включая untracked исходники. Обычный `pre-commit run --all-files` без
Git refs выполняет полный проход. Позиционный список файлов предназначен для
адресной диагностики и не заменяет gate. Перед первым коммитом требуется полный
gate текущего checkout; само по себе это не разрешает создание коммита.

## Qt: Clazy

Clazy запускается для `.cpp/.cc/.cxx` из presentation/qt, adapters/desktop и apps
через `scripts/run_clazy.py`. Это только launcher нативного KDE Clazy:
`level1` включает level0 и level1, `--only-qt` ограничивает Qt diagnostics,
`CLAZY_CHECKS_AS_ERRORS` делает выбранные Clazy warnings ошибками.
Отдельный анализ изменённых заголовков требует owning translation unit;
полный CI-прогон анализирует translation units, а не заголовки сами по себе.

Укажите путь установленного Clazy через `CLAZY_STANDALONE`; необязательный
локальный fallback — `.agent-workspace/clazy-executable.txt` с одной строкой пути.
Qt Creator распространяет clazy-standalone в `bin/clang/bin/`.
Нужен комплект с builtin headers, совместимый с используемым Qt/toolchain.
Для Windows MinGW hook явно передаёт target `x86_64-w64-windows-gnu`;
при смене toolchain соответствующий аргумент нужно согласованно изменить.
CI устанавливает Qt Creator официальным Qt installer action.
Отсутствующая/устаревшая compilation database и отсутствующий инструмент
останавливают проверку, а не считаются PASS.

`scripts/run_clazy.py` передаёт каждый owning translation unit общему
`scripts/quality_execution.py`. Workers запускаются параллельно; набор `level1`
(включая level0), `--only-qt`, target, compilation database и повышение всех
выбранных замечаний Clazy до ошибок одинаковы для каждого процесса.
Заголовки зависимостей остаются в анализе. Переменные окружения, которые могли
бы исключить каталоги/заголовки или ослабить ошибки, очищаются перед запуском.

Без `--jobs` общий scheduler учитывает доступные CPU и 75% доступной RAM.
Для Clazy в расчёте используется оценка 1 GiB на worker; фиксированного ограничения в четыре
worker нет. `--jobs N` задаёт явное положительное число, `QUALITY_JOBS=N` —
общую настройку окружения; аргумент CLI имеет приоритет. `QUALITY_JOBS=auto`
сохраняет автоматический выбор. Явное значение может превысить оценку памяти:
выбирайте его с учётом одновременно работающих приложений.

Каждый TU получает сообщения `start` и `passed`/`failed` с временем и результатом. Диагностика
отдельного процесса выводится целиком, без смешивания строк разных workers.
Машинный прогресс пишется в `.agent-workspace/quality/progress-<tool>-<PID>.jsonl`
и соседний `*.status.json` с текущими jobs/running/completed/total/failed и временем;
путь журнала можно задать через `QUALITY_PROGRESS_FILE`. Ошибка любого worker блокирует
gate; остальные назначенные TU всё равно завершают проверку.
Для адресной проверки можно передать исходники позиционными аргументами:

```powershell
python scripts/run_clazy.py --jobs 2 --extra-arg=--target=x86_64-w64-windows-gnu adapters/desktop/analysis/offline/profile_json.cpp
```

Без позиционных файлов wrapper получает Git-aware scope через общий selector,
затем выбирает Qt translation units. `--scope full`, `--full` или `QUALITY_FULL=1` принудительно
проверяет всё применимое текущее дерево, включая новые исходники; явный full
имеет приоритет над списком адресных файлов. Отсутствие надёжного Git/dependency
контекста приводит к полному scope, а не к пропуску проверки.

Pinned QtNodes и QtADS исправляются при configure через
`cmake/patch_qtnodes_clazy.cmake` и `cmake/patch_ads_clazy.cmake`.
Патчи повторно применимы и останавливают configure при несовпадении исходного
фрагмента с закреплённой редакцией. QtNodes использует полные имена типов в
meta-object signatures; `save() const` остаётся вызываемым через `Q_INVOKABLE`.
QtADS регистрирует `SideBarLocation` через `Q_ENUM_NS` и экспортирует namespace
meta-object из общей библиотеки.
Изменяемые свойства QtADS auto-hide tab получают общий `tabStateChanged` NOTIFY:
сигнал отправляется при изменении положения, ориентации, активности или режима
иконки. Эти исправления не исключают зависимости из диагностики Clazy.

## Архитектурные include-границы: Chromium checkdeps

```powershell
python scripts/vendor/chromium_checkdeps/checkdeps.py --root .
python tests/checkdeps_tests.py
```

Root DEPS описывает доступные include namespaces. Вложенные DEPS запрещают
зависимости переносимых модулей от Qt, UI, платформенных адаптеров и чужих
слоёв. Проверяются как короткие публичные пути, так и repository-relative пути.
У domain явно разрешён только уже используемый `dsp_kernels/window_math.h`
из DSP: это не разрешение подключать весь runtime/backend.

Pre-push всегда проверяет весь исходный tree по этим правилам, включая новые
неигнорируемые директории; vendor/generated и build/output исключены.
Pinned исходники Chromium и BSD license хранятся в `scripts/vendor/chromium_checkdeps`.
Малые локальные адаптации перечислены в LOCAL_CHANGES.md: Windows Git,
untracked директории, angle brackets, C/C++ extensions и общий Qt prefix.

Проверка видит literal includes, но не раскрывает макросы, не анализирует
семантику вызовов, не строит полный compiler dependency graph. В upstream
пропускаются `#if 0` и flat non-Qt includes без `/`. Сокращённые include paths
не разрешаются через все CMake roots. Поэтому это защита объявленных границ,
а не полная автоматическая проверка Clean Architecture. Изменять DEPS для
подавления существующего нарушения без архитектурного решения нельзя.

## Production and test lint

The owner selected separate production and test passes. Default pre-push Clazy
and clang-tidy check production. The registered test root is `tests/`, including
fixtures, headers and non-CDB sources. Filenames and CMake target names do not
change this category: production and examples outside `tests/` remain production.

`--code-scope production` is the default; `tests` selects tests and `all` explicitly
combines both. Git scope is independent: `--scope full`, `--full` and
`QUALITY_FULL=1` expand coverage within the selected category. Explicit files
from a different category fail. Both passes retain the same two function checks;
their limits differ only for registered test scenario sources.

Files under `tests/` ending in `_tests.cpp` and the 13 explicitly registered
scenario/measurement drivers in `scripts/run_clang_tidy.py::SCENARIO_DRIVERS` use
`tests/.clang-tidy`: at most 60 statements and cognitive complexity 25. The registry
covers application startup, block UI, FFT size, graph performance/visual comparison,
numeric loopback/preview cadence, render topology, resampler quality, the three UI
audits and UI transitions. It classifies their actual scenario responsibility;
it does not exempt their diagnostics or all C++ files by name. Every test header, including builders,
fixtures and computational oracles, and every other test C++ source use the root
configuration explicitly: 20 statements and complexity 15. For example,
`tests/sdk_consumer/main.cpp` remains strict; `tests/fft_size_benchmark.cpp` is an
explicitly registered measurement scenario.
The runner chooses the configuration for the requested source, even when a header
needs an owning scenario translation unit, and reports the policy on each task.
Registered test fragments run in their actual owning translation unit with the
strict configuration. An exact path header filter and a basename line filter
intersect to report that header alone, accepting Clang's mixed Windows separators;
the owning scenario is checked separately with its scenario policy. Compiler
errors still fail the task. Production fragment handling is unchanged.
Fixture headers may take a proven C/C++ owner compiled by an actual test target
whose include roots contain `tests/`. Such an owner supplies compiler context,
not an additional production lint job. Selected headers still use the canonical
C++ header language and strict policy; incompatible compiler flags fail rather
than being discarded. This covers the pure attribute macros used by the
component fixture's vendor C sources. Unreferenced helpers require an explicit
retirement decision instead of an arbitrary nearby owner.
Computational helpers extracted from a scenario belong in strict support headers;
the scenario allowance is not a claim that every mixed helper has been audited.

`IgnoreMacros` remains `false` for both checks and both policies. Scenario `CHECK`
macros may forward to a small reporting function, so assertion error reporting does
not add nested branches to each scenario. Conditions, loops and other actual logic
remain visible to the checks; no blanket macro exclusion is enabled.

Run the separate test lint explicitly:

```powershell
python -m pre_commit run clang-tidy-tests --all-files --hook-stage manual
python -m pre_commit run clazy-tests --all-files --hook-stage manual
```

CI test steps require an explicit workflow_dispatch with test_lint=true. Push and
pull-request CI do not run test lint. The hooks are manual-only and do not run
in the default pre-push. Test Clazy selects owning test TUs and preserves only-Qt,
level0/level1 and errors. Python quality-tool regressions remain in the existing
meta-hook for changes to quality tooling.


## Proven compilation profiles

clang-tidy no longer borrows an arbitrary nearby compilation command. Each selected
source must have a real entry in a configured compilation database. Self-contained
headers remain main files with `-x c++-header` placed before the input and use each
distinct proven literal-include owner command. Explicitly registered fragments
(FFT correctness/memory and legacy FIR benchmark headers) run in their real owner
translation unit, preserving its original declarations/include order; an anchored
header filter selects only the requested fragment, accepting both Windows separator
forms. The owner is not force-included into a second standalone copy of that header.
Unknown fragments fail compilation rather than receiving guessed includes. Real
compiler regressions verify an oversized unguarded fragment still blocks on its own
header diagnostic, without redefinition/pragma errors, and a self-contained header
still blocks with canonical header language. Supplying `ESP_CLANG_TIDY` also requires
these regressions against that actual Espressif tool; standard host meta checks use
the pinned host clang-tidy. Missing owners fail closed; distinct owning feature
variants are not silently discarded.

Use repeatable `--context-build-path <build-directory>` for additional configured
profiles; `QUALITY_CONTEXT_BUILD_PATHS` accepts the platform path separator (semicolon
on Windows). The primary database remains `cmake-build-release/compile_commands.json`.
For GMF use the existing `examples/gmf_fft_benchmark/host` composition and its pinned
`GMF_FFT_SOURCE_DIR`; firmware sources use their actual ESP-IDF build profiles.
Profiles are explicit, not discovered by searching old build directories. No synthetic
system headers or arbitrary include paths substitute for a missing platform profile.

Xtensa policy analysis uses real ESP-IDF Clang build profiles and the official
Espressif clang-tidy supplied by `--embedded-executable` or `ESP_CLANG_TIDY`.
The saved official command must identify `--target=xtensa-esp-elf` and an explicit
supported `-mcpu`; it is preserved without GCC flag or standard-library adaptation.
An unproven CPU or target fails closed. Configure these profiles separately with
`IDF_TOOLCHAIN=clang`, the same composition/options/target and their own build-local
sdkconfig. Supply their databases explicitly through `QUALITY_CONTEXT_BUILD_PATHS`.
The current IDF Clang profile selects bundled Newlib; it does not claim to reproduce
the GCC/Picolibc ABI. Native GCC object compilation remains separate acceptance for
that configuration. No hardware or benchmark performance claim follows from syntax
analysis. Compiler-only language overrides precede inputs, including header main files.

Materialized per-owner databases and provenance (original database SHA and owner)
are written to ignored `.agent-workspace/quality/compile-contexts/`. The original
configured databases are never modified by the quality runner. Materialization identity
includes the adapted arguments, so a driver-adaptation change preserves older evidence. The meta-hook pins
clang-tidy22.1.8 for its real fail-closed fragment regression; embedded analysis uses
the Espressif version required by the selected ESP-IDF installation.

The release database is the primary analysis profile. Additional real profiles fill
sources/headers without any proven primary context; they do not multiply every
portable source across every installed firmware configuration. All semantic variants
within the selected tier remain checked. Owner candidates follow the same registered
production/test and vendor/generated main-file policy as the requested category.
Emission-only output/dependency flags are normalized; defines, undefines, standards,
include order and forced includes are preserved. Process-local include-edge and
command caches avoid repeated filesystem discovery; there is no saved PASS cache.
This gate covers default production plus missing platform contexts, not every possible
platform configuration. Native cross-compilation separately covers the configured
firmware branches.

Configured GCC response files are expanded in place, recursively, with original byte
hashes saved in context provenance. GNU response-file quoting and whitespace rules
(including Windows text-mode CRLF handling) are separate from Windows command-line
parsing. Missing files, cycles and malformed quoting fail closed. Production
headers with no production owner may borrow a proven existing test-owner command as
a final metadata fallback; test sources themselves are not selected or linted.

Unused exported standalone aliases have an explicit compile-only consumer target:
configure release with `-DBUILD_QUALITY_HEADER_AUDIT=ON`. Its real target links the
modules exporting those headers. The retained FIR benchmark reference has an
ESP-IDF consumer enabled by `PORTABLE_SDK_HEADER_AUDIT=ON` together with
`PORTABLE_SDK_EXAMPLE_ENTRYPOINT=ON`; it inherits the actual stock component and
FreeRTOS context. These fixtures do not install fake platform headers or add a
runtime feature. Include their configured databases in the explicit context list.
