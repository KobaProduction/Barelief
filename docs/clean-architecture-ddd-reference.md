<!-- Reference snapshot: ArthurKoba/ai-agent-workflow at 9bc1b1ff27f7204fa946253a4d5612fd80102e8f; not project authority. -->
# Clean Architecture and DDD

Универсальный справочник по Clean Architecture / DDD для проектирования и ревью приложений.

Файл описывает смысл архитектурных элементов, их границы ответственности и критерии применения. Это не backlog и не список того, что нужно обязательно реализовать.

Текущее состояние конкретного проекта всегда определяется его repository/project authority, а не этим справочником.

## 1. Главная идея

Clean Architecture разделяет систему так, чтобы бизнес-смысл не зависел от технических деталей.

Главное правило зависимостей:

```text
внешние слои зависят от внутренних,
внутренние слои не зависят от внешних.
```

Внутренние слои описывают:

- предметную область;
- бизнес-правила;
- use cases;
- контракты, которые нужны приложению.

Внешние слои описывают:

- HTTP;
- БД;
- ORM;
- messaging/API clients;
- frameworks;
- фоновые процессы;
- конкретные SDK.

Практический смысл: можно менять PostgreSQL, SQLAlchemy, messaging SDK или web framework без переписывания домена и use cases.

## 2. Слои

### Domain Layer

Domain layer — ядро предметной области.

Здесь находятся правила, которые имеют смысл даже без базы данных, HTTP, очередей и framework-ов.

Domain layer может содержать:

- `Entity`;
- `Value Object`;
- `Aggregate`;
- `Aggregate Root`;
- `Domain Service`;
- `Domain Policy`;
- `Specification`;
- `Factory`;
- `Domain Event`.

Domain layer не должен знать про:

- SQLAlchemy;
- PostgreSQL;
- HTTP;
- messaging platform;
- outbox;
- event bus;
- unit of work;
- repositories implementations;
- Pydantic как формат входа/выхода.

Пример:

```text
User
InviteRelation
InviteRegistered
InviteRelationFactory
InviteActivationPolicy
Notification
NotificationType
NotificationStatus
```

### Application Layer

Application layer реализует сценарии приложения.

Он отвечает не за отдельные бизнес-правила, а за orchestration:

```text
принять command/query
открыть транзакционную границу
загрузить данные
вызвать доменные правила
сохранить изменения
зафиксировать события
вернуть результат
```

Application layer может содержать:

- `Use Case`;
- `Application Service`;
- `Interactor`;
- `Command`;
- `Query`;
- `Command Handler`;
- `Query Handler`;
- `Input Port`;
- `Output Port`;
- `Request Model`;
- `Response Model`;
- `Application Collaborator`;
- `Unit of Work`;
- application ports;
- event handlers;
- workers;
- retry policies;
- event bus registry;
- outbox dispatcher.

Application layer знает про абстракции, но не должен зависеть от конкретной БД, ORM, messaging SDK или web framework.

Пример:

```text
RegisterUserService
RegisterOrGetUserService
ActivateInviteService
UserRegistrationCollaborator
InviteRegistrationCollaborator
NotificationService
CreateNotificationOnInviteRegistered
OutboxDispatcher
EventDeliveryWorker
NotificationWorker
```

### Interface Adapters

Interface adapters переводят данные между внешним миром и application/domain моделью.

Сюда относятся:

- `Controller`;
- `Presenter`;
- `ViewModel`;
- `Repository Implementation`;
- `Gateway Adapter`;
- `Mapper`;
- `Serializer`;
- `Deserializer`;
- `Validator`.

Controller не должен содержать бизнес-сценарий. Он адаптирует внешний запрос к command/query и вызывает use case.

Presenter не должен менять бизнес-смысл результата. Он адаптирует response model к формату клиента.

Repository implementation не должна протекать в application layer. Application layer видит repository port, infrastructure предоставляет реализацию.

### Frameworks And Drivers

Самый внешний слой.

Сюда относятся:

- FastAPI или другой web framework;
- SQLAlchemy;
- PostgreSQL;
- asyncpg;
- Redis/RabbitMQ/Kafka и другие message brokers;
- messaging SDK;
- Docker;
- CLI entrypoint;
- worker runner;
- dependency injection / composition root.

Frameworks and drivers — детали доставки и исполнения. Они подключают систему, но не определяют ее бизнес-смысл.

## 3. Domain элементы

### Entity

`Entity` — объект с identity и жизненным циклом.

Две entity могут иметь одинаковые поля, но быть разными объектами, если у них разные идентификаторы.

Используется, когда важна история и тождественность объекта.

Примеры:

```text
User
InviteRelation
Notification
```

### Value Object

`Value Object` — объект без identity, определяемый значением.

Используется, когда важно не "какой именно объект", а "какое значение".

Value object обычно:

- immutable;
- валидирует собственные инварианты;
- сравнивается по значению.

Примеры:

```text
Email
Money
InviteCodeValue
NotificationIdempotencyKey
NotificationType
NotificationStatus
```

Если значение хранится долго и участвует в контрактах, лучше использовать enum/value object, а не свободную строку.

### Aggregate

`Aggregate` — группа доменных объектов, которые изменяются как единая consistency boundary.

Aggregate нужен, когда несколько объектов должны сохранять совместные инварианты.

Правило:

```text
извне aggregate изменяется только через aggregate root.
```

Не всякая связь entity автоматически является aggregate. Если инвариант можно проверить отдельно и нет необходимости загружать весь граф объектов, aggregate не стоит расширять.

### Aggregate Root

`Aggregate Root` — главный объект aggregate.

Только root доступен извне напрямую. Внутренние объекты aggregate изменяются через методы root.

Смысл: не дать внешнему коду нарушить инварианты aggregate обходным путем.

### Domain Service

`Domain Service` — доменная операция, которая не принадлежит естественно одной entity или value object.

Domain service:

- содержит бизнес-правило;
- не работает с repositories;
- не открывает транзакции;
- не отправляет сообщения;
- не знает про БД, outbox, event bus.

Если объект использует `UnitOfWork`, repositories или outbox, это уже application collaborator, а не domain service.

### Domain Policy

`Policy` — объект, который принимает бизнес-решение.

Policy удобна, когда правило:

- может усложняться;
- используется в нескольких сценариях;
- должно быть явно названо;
- не принадлежит одной entity естественно.

Пример:

```text
InviteActivationPolicy
  -> можно ли активировать код
  -> повторная активация того же кода это no-op или ошибка
  -> можно ли активировать другой код при существующей связи
```

### Specification

`Specification` — объект-условие.

Подходит для сложных проверок, которые нужно комбинировать, переиспользовать или явно тестировать.

Пример:

```text
CanActivateInviteCode
CanReceiveNotification
CanSendOperatorMessage
```

Specification не обязательна для каждого `if`. Она нужна, когда условие стало самостоятельным понятием.

### Factory

`Factory` создает доменные объекты и гарантирует инварианты создания.

Factory полезна, когда создание:

- требует нескольких зависимых значений;
- должно проверять правила;
- не должно размазываться по use cases.

Factory не должна сохранять объект в БД.

Пример:

```text
InviteRelationFactory.create_relation(...)
```

### Domain Event

`Domain Event` — факт, который уже произошел в домене.

Он должен называться в прошедшем времени или как совершившийся факт:

```text
InviteRegistered
UserCreated
NotificationQueued
```

Domain event не является командой.

Плохо:

```text
SendExternalMessage
CreateNotification
```

Хорошо:

```text
InviteRegistered
```

Событие говорит "что произошло", а не "что теперь надо сделать".

## 4. Application элементы

### Use Case / Application Service

`Use Case` или `Application Service` реализует один сценарий приложения.

Он отвечает за порядок действий:

```text
получить command
начать UoW
загрузить нужные объекты
вызвать domain model / policy / factory
вызвать collaborators
сохранить результат
вернуть response
```

Application service не должен:

- содержать сложные доменные правила;
- напрямую работать с SQLAlchemy session;
- отправлять сообщения во внешний канал внутри бизнес-транзакции;
- вызывать другой application service;
- знать детали JSONB/ORM/HTTP.

Application service может использовать:

- domain service;
- domain policy;
- domain factory;
- application collaborator;
- ports;
- UoW.

Пример:

```text
RegisterUserService
ActivateInviteService
```

### Interactor

`Interactor` — альтернативное название реализации use case.

В некоторых стилях Clean Architecture:

```text
Input Port -> Interactor -> Output Port
```

Термин `Application Service` можно использовать как синоним реализации use case, если это соответствует словарю проекта.

### Command

`Command` — входная модель операции, которая меняет состояние.

Command должен содержать данные от вызывающей стороны, но не технические значения, которые система должна сгенерировать сама.

Плохо:

```text
user_uuid
invite_relation_uuid
event_uuid
```

в command регистрации.

Хорошо:

```text
external_user_id
inviter_invite_code
```

если это реальные входные данные сценария.

### Query

`Query` — входная модель операции чтения.

Query не должна менять состояние системы.

Если проект придет к CQRS, query handlers можно отделить от command handlers. Но CQRS не нужно вводить заранее ради терминологии.

### Input Port

`Input Port` — интерфейс входа в use case.

Нужен, когда presentation layer должен зависеть от абстракции use case, а не от конкретной реализации.

В Python часто достаточно Protocol, если это дает реальную пользу.

### Output Port

`Output Port` — интерфейс, через который use case отдает результат наружу, например presenter-у.

Нужен не всегда. В web backend часто достаточно вернуть response model из use case, если нет сложной presentation boundary.

### Request / Response Model

`Request Model` — DTO входа в use case.

`Response Model` — DTO выхода из use case.

Они не равны domain entity.

Причина: domain entity описывает бизнес-смысл, а request/response model описывает контракт конкретного сценария.

### Application Collaborator

`Application Collaborator` — помощник application service, которому use case делегирует повторяемый application-level блок.

Это роль, а не слой.

Collaborator уместен, когда:

- несколько use cases используют один и тот же блок сценария;
- блок работает с repositories/UoW/outbox;
- это не чистое доменное правило;
- вынос делает use case проще, но не скрывает бизнес-смысл.

Примеры:

```text
UserRegistrationCollaborator
InviteRegistrationCollaborator
NotificationService
```

Важно:

```text
если объект знает про UoW/repositories/outbox,
он не domain service.
```

### Unit of Work

`UnitOfWork` задает транзакционную границу.

Он объединяет изменения, которые должны быть сохранены атомарно.

Пример смысла:

```text
создать User
создать InviteRelation
записать InviteRegistered в outbox
commit одной транзакцией
```

Если транзакция откатилась, не должно остаться ни relation без события, ни события без relation.

В текущем стиле:

```text
async with uow_factory() as uow:
    ...
```

означает:

```text
успешный выход -> commit
исключение -> rollback
```

### Repository Port

`Repository Port` — application/domain контракт доступа к данным.

Он описывает, что нужно сценарию:

```text
add(user)
get_by_uuid(user_uuid)
get_by_external_user_id(external_user_id)
```

Он не описывает, как именно это сделано в SQL.

Repository implementation живет во внешнем слое.

### Gateway Port

`Gateway` — порт для внешней системы.

Примеры:

```text
MessagingGateway
ProviderGateway
EmailGateway
StorageGateway
```

Application layer зависит от gateway interface, infrastructure реализует вызов SDK/API.

### Clock / UUID Generator / Code Generator

Такие объекты — application ports для недетерминированных значений.

Они полезны, когда важно:

- тестировать без monkeypatch;
- явно контролировать время;
- генерировать ids внутри системы;
- не передавать технические значения в command.

Пример:

```python
class Clock(Protocol):
    def now(self) -> datetime:
        ...
```

## 5. Interface adapter элементы

### Controller

Controller принимает внешний запрос и вызывает application use case.

Он может:

- прочитать request;
- провалидировать boundary schema;
- собрать command/query;
- вызвать use case;
- вернуть response.

Он не должен:

- обращаться напрямую к repositories;
- открывать транзакции;
- содержать бизнес-правила;
- создавать domain events.

### Presenter

Presenter адаптирует response model к формату клиента.

Он уместен, когда один use case может отдавать результат в разные представления:

- JSON API;
- HTML/UI;
- CLI;
- bot message.

### Mapper

Mapper преобразует модели между слоями.

Типичные направления:

```text
ORM model <-> Domain entity
Domain entity -> Response DTO
Request schema -> Command
```

Mapper не должен становиться местом бизнес-правил.

### Serializer / Deserializer

Serializer превращает объект в формат хранения/передачи.

Deserializer восстанавливает объект из формата хранения/передачи.

Для event bus это особенно важно:

```text
Domain Event <-> event_type + JSONB payload
```

Если сериализация события размазана по repositories/handlers, позже сложнее добавлять новые типы событий.

### Validator

Validator проверяет входные данные на границе.

Boundary validation отличается от domain validation:

- boundary validation: формат, тип, обязательность, min/max length;
- domain validation: бизнес-инварианты.

Pydantic хорошо подходит для boundary validation, но не должен становиться основой доменной модели.

## 6. Cross-cutting элементы

### Event Bus

`EventBus` — механизм доставки событий обработчикам.

Он отвечает за:

- регистрацию handlers;
- сопоставление event type -> handlers;
- вызов handlers;
- в надежной схеме — связь с outbox/deliveries/workers.

Event bus не обязан быть внешним брокером. В небольшой системе он может быть DB-backed через outbox.

### Outbox

`Outbox` — надежное хранилище событий в той же БД и транзакции, что бизнес-изменение.

Смысл:

```text
если бизнес-изменение закоммичено,
событие тоже сохранено.
```

Это защищает от ситуации:

```text
relation создана,
а событие потерялось из-за падения процесса.
```

Outbox — не сама шина. Это durable source of events.

### Event Delivery

`EventDelivery` — доставка одного event конкретному handler.

Нужна, когда у события может быть несколько обработчиков.

Пример:

```text
InviteRegistered
  -> CreateNotificationOnInviteRegistered
  -> AccrueBonus
  -> UpdateStats
```

Если один handler упал, это не должно откатывать успешную обработку другим handler.

Идемпотентность delivery:

```text
UNIQUE(event_uuid, handler_name)
```

### Inbox

`Inbox` — защита от повторной обработки входящих внешних сообщений.

Outbox решает проблему исходящих событий.

Inbox решает проблему входящих событий:

```text
получили external_message_id
если уже обработан -> no-op
если нет -> обработать и записать как processed
```

Нужен, если система начнет принимать события из внешнего брокера/API.

### Message Bus

`Message Bus` — более общий механизм маршрутизации сообщений.

Он может маршрутизировать:

- commands;
- domain events;
- integration events;
- background jobs.

Вводить message bus стоит, когда прямые вызовы handlers стали плохо масштабироваться организационно.

### Pipeline

`Pipeline` — цепочка обработки вокруг handler/use case.

Примеры шагов:

```text
validation
authorization
idempotency
logging
metrics
transaction
retry
handler
```

Pipeline полезен, когда одинаковое поведение начинает повторяться вокруг многих handlers.

### Middleware

`Middleware` — framework-level pipeline.

Обычно работает до controller:

- request id;
- logging;
- auth extraction;
- CORS;
- exception mapping.

Middleware не должно содержать бизнес-сценарий.

### Decorator

`Decorator` добавляет поведение вокруг объекта с тем же интерфейсом.

Примеры:

```text
LoggingMessagingGateway
RetryingMessagingGateway
MetricsUserRepository
CachedQueryHandler
```

Decorator хорош, когда нужно добавить техническое поведение без изменения основного класса.

### Adapter

`Adapter` приводит внешний API к внутреннему port.

Пример:

```text
Messaging SDK -> MessagingGateway
SQLAlchemy -> UserRepository
```

Adapter изолирует внешний контракт от application layer.

### Facade

`Facade` дает простой интерфейс к сложной подсистеме.

Использовать стоит, когда за фасадом действительно есть несколько объектов/шагов, а вызывающему коду нужен простой вход.

Facade не должен скрывать важные бизнес-решения так, что use case становится нечитабельным.

### Proxy

`Proxy` контролирует доступ или откладывает реальный вызов.

Примеры:

- lazy initialization;
- remote proxy;
- access control;
- rate limiting wrapper.

### Anti-Corruption Layer

`Anti-Corruption Layer` защищает внутреннюю модель от чужой модели.

Нужен, когда внешняя система имеет свои термины, статусы и структуру данных, которые не должны протечь в домен.

Пример:

```text
webhook/provider event
  -> ACL translator
  -> internal command/event/value object
```

### Idempotency Key

`Idempotency Key` — стабильный ключ операции, который позволяет безопасно повторять обработку.

Примеры:

```text
new_invite:{inviter_user_uuid}:{invited_user_uuid}
delivery_batch:{batch_uuid}:{recipient_uuid}:{channel}
system_message:{operation_id}:{recipient_uuid}
```

Idempotency key должен отражать бизнес-смысл уникальности операции.

### Lease / Lock

`Lease` — временное владение задачей worker-ом.

Типичные поля:

```text
locked_by_worker
locked_until
attempts
```

Смысл:

```text
worker забрал задачу
если worker умер, lease истечет
другой worker сможет забрать задачу снова
```

Ack/fail операции должны проверять владельца lease:

```text
WHERE locked_by_worker = :worker_name
```

## 7. Outbox, deliveries и workers

Надежная схема обработки событий:

```text
Use Case
  -> business changes
  -> outbox event в той же транзакции
  -> commit

OutboxDispatcher
  -> читает undispatched events
  -> создает event deliveries для handlers
  -> mark event dispatched

EventDeliveryWorker
  -> claim pending deliveries
  -> вызывает handler
  -> mark processed/failed
```

Атомарный claim обычно строится на:

```text
FOR UPDATE SKIP LOCKED
locked_by_worker
locked_until
attempts
```

Смысл SQL-паттерна:

```sql
WITH picked AS (
    SELECT delivery_uuid
    FROM event_deliveries
    WHERE status = 'pending'
      AND available_at <= now()
      AND (locked_until IS NULL OR locked_until < now())
    ORDER BY available_at, created_at
    LIMIT :limit
    FOR UPDATE SKIP LOCKED
)
UPDATE event_deliveries d
SET status = 'processing',
    locked_by_worker = :worker_name,
    locked_until = :lock_until,
    attempts = attempts + 1,
    last_error = NULL
FROM picked
WHERE d.delivery_uuid = picked.delivery_uuid
RETURNING d.*;
```

`FOR UPDATE SKIP LOCKED` позволяет нескольким workers безопасно забирать разные задачи.

## 8. Notifications как пример разделения concerns

Invite-сценарий не должен напрямую отправлять сообщение во внешний канал и не должен сам собирать notification payload.

Правильное разделение:

```text
InviteRegistrationCollaborator
  -> создать InviteRelation
  -> добавить InviteRegistered в outbox

CreateNotificationOnInviteRegistered
  -> получить InviteRegistered
  -> вызвать NotificationService

NotificationService
  -> создать Notification с type/status/source/idempotency_key/payload

NotificationWorker
  -> отправить pending/retry notifications через MessagingGateway
```

Смысл:

- invitation context отвечает за invitation relation;
- event фиксирует факт;
- notification module решает, какое уведомление создать;
- worker занимается доставкой;
- gateway изолирует внешний API.

## 9. Внешняя отправка и exactly-once

БД может обеспечить идемпотентность своего состояния.

Внешний API обычно не дает строгий exactly-once.

Например, при отправке во внешний канал возможна ситуация:

```text
запрос ушел
внешний provider принял/создал сообщение
ответ потерялся по timeout
worker считает попытку неуспешной
retry может создать дубль во внешней системе
```

Что можно сделать:

- не отправлять внешний запрос внутри бизнес-транзакции;
- хранить status/attempts/last_error;
- использовать idempotency keys внутри своей системы;
- фиксировать внешний message id после успешного ответа;
- разделять retryable и permanent failures;
- принимать, что внешняя доставка обычно at-least-once, а не exactly-once.

## 10. Pydantic, dataclass и persistence

Pydantic уместен на границах:

- application commands;
- request/response schemas;
- JSONB payload contracts;
- worker DTO;
- event bus registration DTO;
- persistence-facing records.

Domain model лучше держать как dataclass / обычные Python-объекты, если Pydantic не нужен для доменного смысла.

Причина:

```text
домен описывает бизнес-инварианты,
а не формат HTTP/JSONB/ORM.
```

SQLAlchemy models — persistence model, не domain model.

Нормально иметь преобразование:

```text
SQLAlchemy model <-> domain entity
domain event <-> event_type + JSONB payload
```

## 11. Composition Root

`Composition Root` — место, где собирается граф зависимостей.

Там создаются:

- repositories;
- unit of work factory;
- gateways;
- services;
- collaborators;
- use cases;
- event handlers;
- workers;
- controllers.

Внутри use case не нужно вручную создавать infrastructure dependencies.

Плохо:

```text
RegisterUserService сам создает SqlAlchemyUserRepository
```

Хорошо:

```text
composition root создает RegisterUserService с нужными ports/collaborators
```

## 12. Практические правила

1. Domain не знает про infrastructure.
2. Application layer зависит от ports, а не от implementations.
3. Application service не вызывает другой application service.
4. Application service может использовать domain policy/factory/service и application collaborator.
5. Domain service/factory/policy не знает про UoW, repositories, outbox, sessions, database, event bus.
6. Application collaborator может работать с UoW, repositories и outbox.
7. Если объект работает с UoW/repositories/outbox, это не domain service.
8. Событие должно описывать факт, а не команду внешней системе.
9. Внешние API не вызывать внутри бизнес-транзакции.
10. Технические uuid генерируются системой, а не приходят в command без причины.
11. Внутренние identifiers называть `uuid` / `*_uuid`, не `id` / `*_id`.
12. `id` допустим для внешних контрактов вроде `external_user_id`, `request_id`, `operation_id`, `source_id`.
13. Pydantic использовать на границах, домен держать независимым от transport/persistence formats.
14. Не добавлять паттерн только потому, что он существует в Clean Architecture.
15. Добавлять архитектурный элемент тогда, когда он защищает границу, инвариант, идемпотентность или уменьшает реальное дублирование.

## 13. Пример терминологии

```text
RegisterUserService — application service / use case
RegisterOrGetUserService — application service / use case
ActivateInviteService — application service / use case

UserRegistrationCollaborator — application collaborator
InviteRegistrationCollaborator — application collaborator
NotificationService — application collaborator notification-модуля

InviteRelationFactory — domain factory
InviteActivationPolicy — domain policy
InviteRelation — domain entity
InviteRegistered — domain event
InviteCodeGenerator / InviteCodeResolver — application ports для формата invite code

OutboxRepository — application port
EventBus registry — application event bus registry
OutboxDispatcher — application worker/service
EventDeliveryWorker — application worker/service
NotificationWorker — application worker/service

SqlAlchemy repositories — infrastructure adapters
SqlAlchemyUnitOfWork — infrastructure adapter
MessagingGateway implementation — infrastructure adapter
```

