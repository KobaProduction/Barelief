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

```powershell
./.agent-workspace/quality-venv/Scripts/python.exe -m pre_commit run --all-files --hook-stage pre-push
```

Pre-push проверяет отправляемый набор файлов; содержимое берётся из checkout.
Незакоммиченные правки могут отличаться от отправляемого commit. CI выполняет
проверку в своей чистой редакции. Созданный workflow не означает успешный
серверный запуск или настроенные branch protections.

Исключения vendor/generated и пользовательского `output/` перечислены в
`.pre-commit-config.yaml`. Старые нарушения не получают baseline.
Линтер не заменяет проверку контрактов, архитектурное ревью и тесты.

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
