# Проверки C++ и границ модулей

Serena устанавливается глобально через `uv tool install -p 3.13 serena-agent`
и подключается в пользовательской конфигурации MCP Codex. Настройки проекта
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

В `.clang-tidy` настроены два правила:

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

Для C++ headers без собственной записи clang-tidy может подобрать C-команду.
Hook явно задаёт язык C++ и начальный стандарт C++23 через стандартные
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
from a different category fail. Checks and thresholds are identical in both passes.

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
