# Видимый план в Codex

Проверено **2026-10-07, Europe/Moscow**, в отдельном workflow-чате текущего проекта.
Цель — один видимый overview Foundation → V1 → V2 → V3 → V4 → финальная приёмка,
который сохраняет completed/pending/in-progress и переживает compaction.

## Что подтверждено

Официальный [Codex Prompting Guide](https://developers.openai.com/cookbook/examples/gpt-5/codex_prompting_guide)
описывает настоящий `update_plan`: список `{step, status}` со статусами `pending`,
`in_progress`, `completed`, optional explanation и максимум одним in-progress.
Это описание инструмента, а не обещание его доступности в каждом desktop/Work чате.

[App Server](https://learn.chatgpt.com/docs/app-server) документирует уведомление
`turn/plan/updated` с turnId, explanation и plan; в протоколе статус пишется
`inProgress`. Это событие сервера, а не предоставленная здесь команда записи плана.
Отдельный item `plan` содержит предлагаемый текст Plan Mode; он не равен TODO tracking.

Проверены предоставленные прямые инструменты и полный `ALL_TOOLS` workflow-чата:
настоящего `update_plan` нет; есть `create_goal`, `get_goal`, `update_goal`.
Координатор сообщил такую же проверку своего чата. Goal задаёт цель и её lifecycle,
не список шагов. Наличие desktop оболочки и local executor не означает, что native
Codex chat и Work parent имеют одинаковый tool surface. Причина отсутствия здесь
не установлена; переносимость capability между чатами не доказана.

Открытая [Configuration Reference](https://learn.chatgpt.com/docs/config-file/config-reference)
не установила supported настройку включения `update_plan` в существующем чате.
Config не изменялся. Не проверялись и не обещаются undocumented flags/slash commands,
переключение истории/контекста, прямой доступ к app-server или internal app DB.

## Применённый workflow

[implementation-plan](implementation-plan.md) задаёт canonical-only scope,
[vertical-delivery-plan](vertical-delivery-plan.md) — автономную поставку/ownership,
[progress-checklist](progress-checklist.md) — durable текущую сводку.
Старые legacy compatibility/migrations/schema1 gates не являются текущими задачами.

Владелец самостоятельно завершает большой блок и передаёт один финальный отчёт
или настоящий блокер. Общий API/ownership согласуется напрямую с владельцем.
Root не получает status spam и не утверждает каждый шаг. I единолично выполняет
shared CMake/Ninja, composition и build без blanket freeze группы.
Независимое code review готовых блоков сохраняется. Новые тесты/retained new oracles
и согласованные полные проверки проходят FINAL после общей интеграции.

Overview раздельно отражает implementation/integration/independent acceptance,
имеет максимум один in_progress и обновляется по законченной крупной evidence,
не по мелким командам. Whole V1–V4 pending; исторически приняты только bounded
основания и CONTROL mask66. Compatibility PASS не закрывает новый numeric scope.
После compaction читаются актуальные документы; существующие чаты сохраняют контекст.

Если настоящий native update_plan предоставлен координаторскому чату, он вызывается
с тем же overview и поддерживается по крупным результатам. Plan Mode, goal, custom MCP
и Markdown не подменяют native вызов. Межчатовой записи native plan имеющимися tools нет.
Отсутствие инструмента сообщается прямо; Markdown UI fallback не открывается.
Реализация не останавливается ради повторных диагностических циклов.
## История проверки UI и отказ от fallback

Ранее предложенный Markdown preview пользователь явно отверг: требуется только
встроенный checklist. Файл остаётся durable evidence-записью, но не выдаётся за
решение UI-проблемы и повторно не открывается как fallback. Исторический вызов
`open_in_codex` для workflow-чата вернул `status: queued`, threadId
`01a116b5-1f55-7223-9591-e7806bfe6a2c`: панель поставлена в очередь, визуальное
отображение тогда не подтвердилось. Координатор позднее открыл файл в главном
чате; это не создало native checklist. В отдельной проверке пользователь включил
Plan Mode, а screenshot подтвердил нативную карточку `<proposed_plan>`.
После выхода в Default `update_plan` не появился; оба диагностических сабагента
также сообщили отсутствие инструмента. Карточка proposed plan не подтверждает
создание структурированного списка выполнения. Причина отсутствия не установлена.

Если tool surface в дальнейшем действительно предоставит native инструмент,
координатор инициализирует его из checklist в существующем чате и фиксирует факт
успешного вызова. До этого никакого confirmed способа activation native plan здесь
нет; не нужно останавливать реализацию ради Plan Mode или создавать второй goal.

Подход с живым файлом и Progress checkboxes описан в официальном
[ExecPlans recipe](https://developers.openai.com/cookbook/articles/codex_exec_plans).
В проект перенесены только durable progress и синхронизация через AGENTS;
остальные рекомендации recipe, включая commits, не отменяют правила проекта.
