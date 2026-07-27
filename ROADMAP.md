# ETSL — Roadmap разработки

> **Назначение файла.** Это единый источник правды по проекту: контекст, принятые
> архитектурные решения, известные дефекты и поэтапный план. Файл ведётся вручную:
> чекбоксы отмечаются по мере выполнения, даты и замеры дописываются.
>
> **Инструкция для ИИ-ассистента:** НЕ проводи повторный анализ проекта — разделы
> «Принятые решения» и «Известные дефекты» содержат уже проверенные факты (см. даты
> и способы проверки). Перед задачей читай только файлы, перечисленные в этой задаче.
> После выполнения задачи отметь чекбокс `[x]` и допиши дату. Соблюдай раздел
> «Правила разработки».

---

## 1. Суть проекта

**etsl** — сокетная библиотека для сборки в **минимальный бинарный размер**.

- Реактивная модель (reactor), события доставляются колбэками.
- Windows — первая платформа (IOCP), Linux (epoll) — вторая. Код пользователя
  платформенно-нейтрален: «написал один раз — работает везде».
- Прообраз API — сокеты Qt (`QAbstractSocket`/`QTcpSocket`/`QTcpServer`),
  но **только async-часть** (никаких `waitFor*`).
- IOCP приводится к реактивному (readiness) режиму трюком с **0-byte WSARecv**.
- Основа: ETL (Embedded Template Library), C++20 modules, `noexcept`-дисциплина.

Нефункциональные требования:

1. Минимальный размер бинаря — измеряемая метрика, не декларация (см. Этап 0).
2. Библиотека не выполняет heap-аллокаций сама; объекты создаёт владелец
   (стек/статик/пул пользователя).
3. Без исключений и RTTI в коде библиотеки.
4. Платформенный код — только в явно выделенных слоях; в `tcp_socket` ноль `#ifdef`.

---

## 2. Окружение и сборка (проверено 2026-07-18)

- Тулчейн рабочий: **clang (MSYS2 mingw64) + Ninja**, каталог `cmake-build-debug-clang/`.
  Сборка зелёная. Debug exe ≈ 336 КБ (это НЕ baseline; baseline снимается на
  MinSizeRel в Этапе 0).
- MSVC (`cl`) **не поддерживается** и не планируется; проект ориентирован
  исключительно на Clang.
- ETL 20.47.1 (`external/etl`, submodule). Исключения внутри ETL выключены по
  умолчанию (`ETL_THROW_EXCEPTIONS` не определён, `external/etl/include/etl/platform.h:264`)
  — дополнительных макросов для ETL не нужно; нужно лишь компилировать сам проект
  без исключений.
- CMake ≥ 4.2, `CMAKE_CXX_SCAN_FOR_MODULES ON`. Модули перечислены в
  `FILE_SET CXX_MODULES` в `CMakeLists.txt` — **каждый новый модуль добавлять туда**.

Команды:

```bash
cmake --build cmake-build-debug-clang        # рабочая сборка
```

---

## 3. Принятые архитектурные решения (ADR)

Формат: решение → почему → что отвергнуто. Эти решения финальны, если явно не
пересмотрены с записью здесь.

### ADR-1. Платформенные границы — `reactor/impl` и `platform`, не `socket`

`reactor/impl/<platform>` содержит только demultiplexer/completion backend.
Transport-specific OS operations находятся в `platform/net/<platform>`;
`platform/net/tcp_driver.cppm` выбирает реализацию alias-ом. `tcp_socket`
пишется один раз без `#ifdef` и зависит от общего driver contract.
Platform TCP driver использует reactor для association/completion, но reactor
не зависит от driver. Размещение platform driver в `reactor` или `socket`
запрещено.

### ADR-2. Эмуляция readiness поверх IOCP

- **Чтение:** на сокет взводится ровно один pending **0-byte `WSARecv`**. Его
  completion всегда имеет `bytesTransferred == 0` (данные не копируются), поэтому
  «данные пришли» и «peer закрыл соединение» из completion неотличимы. После
  completion делается неблокирующий `recv(buf, 1, MSG_PEEK)`:
  `> 0` → событие `EV_READ`; `0` → `EV_CLOSED`; `WSAEWOULDBLOCK` → ложное
  срабатывание → перевзвести. Level-triggered: после возврата обработчика проба
  взводится заново — если пользователь не вычитал всё, событие повторится
  (пейсинг естественный, через async completion, busy loop невозможен).
- **Запись (пересмотрено 23.07.2026):** **0-byte `WSASend` НЕ работает** — он
  завершается немедленно и не сигнализирует записываемость. На IOCP используется
  proactive `WSASend`; partial/completion обрабатывает TCP driver. На epoll
  driver продолжает `send` по `EPOLLOUT`. Общий `tcp_socket` держит FIFO
  caller-owned `C_TcpWriteRequest`, но одновременно pending только одна kernel
  write. Timer backoff и внутренний TX ring buffer исключены из write-path.
- **Accept/Connect:** эмуляция не нужна — `AcceptEx`/`ConnectEx` нативно
  completion-based; их завершение мапится прямо в `onIncoming`/`onConnected`.
- **Контракт association:** generic reactor только связывает fd с backend.
  Взвод connect/read/write и обязательный nonblocking mode — ответственность
  platform TCP driver/socket factory.
- **Отвергнуто:** `WSAEventSelect` + `WSAWaitForMultipleEvents` (лимит 64 handle
  на ожидание, лишний поток); AFD poll в стиле wepoll (недокументированный API).

### ADR-3. Диспетчеризация — `etl::delegate`

```cpp
// Реактор не знает тип операции.
struct operation_iocp_s : WSAOVERLAPPED {
    etl::delegate<void(uint32_t bytes, int32_t error)> callback;
};
```

IOCP reactor доставляет raw completion делегату операции. TCP driver переводит
его в `CONNECTED/READ_READY/CLOSED/IO_ERROR/DISPOSED`; socket wrapper статически
вызывает handler. Валидный callback одновременно обозначает in-flight
operation; reactor очищает его перед вызовом, отдельного `pending` flag нет.
Виртуальных иерархий и heap type-erasure нет.

### ADR-4. Владение памятью — библиотека не аллоцирует

- Event loop — value type на стеке: `C_EventLoop loop; loop.initialize(); loop.run();`
  **Отвергнуто:** `etl::pool` + `createLoop()` + `unique_ptr` (машинерия без
  выигрыша — цикл создаётся один раз).
- IOCP operation (содержит `WSAOVERLAPPED`) — член platform driver; reactor
  хранит указатель только до completion.
- Write payload и `C_TcpWriteRequest` принадлежат пользователю и живут до
  единого completion callback. Request — переиспользуемый стабильный slot;
  payload/context/callback задаются при каждом `write`. Requests стоят в
  intrusive FIFO без аллокаций.

### ADR-5. Лайфтайм OVERLAPPED (классическая ловушка IOCP)

`WSAOVERLAPPED` обязан жить до завершения операции. IOCP TCP driver содержит
две стабильные операции: control (`ConnectEx` или read-probe) и write
(`WSASend`). `close()` заменяет callbacks активных операций на direction-specific
cancellation callbacks, вызывает `CancelIoEx` и считает completions; write path
сохраняет результат уже queued completion, а `ERROR_OPERATION_ABORTED`
означает фактическую отмену. Последний callback выдаёт `DISPOSED`.
Normal I/O paths teardown не проверяют.
Уничтожение объекта раньше запрещено.

### ADR-6. Ошибки

`etl::expected<T, int32_t>` с **OS-кодом** (WSA*/errno) внутри. Небольшая
утилита маппинга в портируемые коды (`WouldBlock`, `ConnRefused`, `ConnReset`,
`Closed`, `TimedOut`...) — только там, где логика ветвится по коду.
**Отвергнуто:** `std::error_code` (тащит `<system_error>`, vtable категорий).

### ADR-7. Потоки

v1: один цикл = один поток, внутренних блокировок нет, обработчики исполняются
на потоке цикла. Кросс-поточно разрешены только: `shutdown()` и `post()`
(через `PostQueuedCompletionStatus`). Флаг останова — атомарный. Конструкция
не препятствует мультипоточному `run()` в будущем (IOCP это умеет нативно).
`shutdown()` разрешён только после `DISPOSED` всех I/O owners: reactor не
владеет operation storage и не дренирует оставленные pending operations.

### ADR-8. Qt-подобие — только async

Копируем состояния `Unconnected/Connecting/Connected/Closing` и async events,
но write принимает reusable caller-owned request с единым completion callback
(`error == 0` — success). `bytesAvailable` удалён как TOCTOU и лишний syscall;
после `on_read` пользователь вызывает `read(span)`. **Отвергнуто:** все
`waitFor*`.

### ADR-9. Размер — измеряемая метрика

Флаги: clang → `-Os -ffunction-sections -fdata-sections -fno-exceptions
-fno-rtti -flto`, линковка `-Wl,--gc-sections -s`.
CMake-таргет `size-report` (`size -A` по секциям). Бюджет размера
устанавливается после первого замера (Этап 0), а не выдумывается заранее.

---

## 4. Целевая структура модулей

```
src/
├── core/
│   ├── error.cppm             # library errors / portable mapping
│   └── socket_types.cppm      # единственное определение socket_t      (Этап 0, из socket.cppm)
├── reactor/                   # переименование event_loop/ (Этап 1):
│   │                          # имя должно отражать readiness-семантику
│   ├── reactor.cppm           # alias C_Reactor = бэкенд по _WIN32/__linux__
│   ├── reactor_trait.cppm     # concept + static_assert бэкенда
│   └── impl/
│       ├── iocp/reactor_iocp.cppm/.cpp
│       └── epoll/reactor_epoll.cppm/.cpp  # Linux, Этап 4
├── socket/
│   ├── socket.cppm/.cpp       # RAII-хэндл + close (слить raii+independent)
│   ├── socket_factory.cppm    # модуль socket.factory (имя файла = имя модуля)
│   ├── socket_address.cppm/.cpp  # обёртка над sockaddr_storage (C_Address) (Этап 2)
│   ├── tcp_socket.cppm/.cpp   # Qt-подобный async API                  (Этап 2)
│   ├── tcp_write_request.cppm # caller-owned TCP write operation
│   └── tcp_server.cppm/.cpp   # AcceptEx                               (Этап 2)
├── timer/                       # таймеры реактора                      (Этап 2, 2.1)
│   ├── timer.cppm               # C_Timer: arm/execute; «взведён» ⇔ is_linked()
│   ├── timer_types.cppm         # clock_t, time_point_t, timer_cb_t
│   └── timer_bucket.cppm/.cpp   # сортированный intrusive-список дедлайнов
└── platform/
    ├── net/
    │   ├── tcp_driver.cppm          # platform driver alias
    │   ├── tcp_driver_types.cppm    # общий platform driver contract
    │   ├── iocp/tcp_driver_iocp.cppm/.cpp
    │   └── epoll/tcp_driver_epoll.cppm/.cpp  # Этап 4
    ├── wsa_initializer.cppm/.cpp  # только Windows; на Linux — отсутствует
    └── etl_chrono.cpp           # etl_get_steady_clock (QPC / clock_gettime) (Этап 2)
```

Соглашения: имя файла модуля == имя модуля; `export module <domain>[.<sub>]`;
namespace `etsl`; классы с префиксом `C_`, методы `snake_case` (как в текущем коде).

---

## 5. Известные дефекты текущего кода (проверено 2026-07-18)

Фиксятся в Этапе 0. Проверка повторно не требуется — факты подтверждены сборкой
и препроцессором.

- **D1.** `#if defined(WINNT)` — `WINNT` определяет **только MinGW**-тулчейн
  (проверено препроцессором); MSVC его не определяет → под `cl` ветка уходит в
  `#error "Unsupported platform"`. Заменить на `_WIN32`; `LINUX` → `__linux__`.
  Файлы: `src/reactor/event_loop.cppm:17`, `src/socket/socket.cppm:5`,
  `src/socket/socket_independent.cpp:5`. Заодно убрать stray `)` в строках
  `#error "...yet");` (`event_loop.cppm:21`, `socket_independent.cpp:9`).
- **D2.** Двойное определение `etsl::socket_t`: `src/socket/socket.cppm`
  (`SOCKET` = u64) vs `src/socket/socket_interface.cppm:11` (`int32_t`).
  При совместном импорте — ODR-конфликт, который clang проглатывает молча
  (IFNDR). Оставить одно определение в `core/socket_types`.
- **D3.** `src/socket/socket_factory.cppm:24-36`: `ioctlsocket`/`setsockopt`
  возвращают -1, а не код ошибки → нужен `WSAGetLastError()`. `TCP_NODELAY`
  ставится при любом `type` → фабрика `SOCK_DGRAM` всегда падает (ставить только
  для `SOCK_STREAM`). `SO_REUSEADDR` на Windows имеет иную семантику (разрешает
  захват адреса) — из фабрики убрать, для сервера в Этапе 2 использовать
  `SO_EXCLUSIVEADDRUSE`. `AF_INET` захардкожен — параметризовать в Этапе 2.
- **D4.** `src/reactor/impl/event_loop_iocp.cppm:18`: деструктор не делает
  `CloseHandle(iocp_)` — утечка хэндла.
- **D5.** `src/reactor/impl/event_loop_iocp.cpp:33-35`: все failed completions
  молча выбрасываются — ошибки (`WSAECONNRESET` и т.п.) не доходят до
  обработчика. Извлекать код через `WSAGetOverlappedResult`.
- **D6.** `halt_` — обычный `bool`; `shutdown()` из другого потока = data race.
  Сделать атомарным (Interlocked* / `etl::atomic`).
- **D7.** `C_ISocket` — мёртвый код (конструктор объявлен, но нигде не определён;
  класс абстрактный). Удалить вместе с `C_EventNode`, если к Этапу 1 не найдётся
  применения.
- **D8.** `LOOP_POOL`/`createLoop`/`loop_destructor_s` — избыточны (ADR-4).
  Event loop становится value type; `C_WSAInitializer` переезжает в
  `initialize()` как function-local static.

---

## 6. Roadmap

### Этап 0 — Гигиена и инфраструктура размера

**Цель:** проект собирается clang **и** MSVC без варнингов, размер измеряется
автоматически, мёртвый код удалён.

- [x] **0.1** D1: макросы `_WIN32`/`__linux__`, stray `)` в `#error`. **(18.07.2026)**
- [x] **0.2** D2: единый `socket_t` в `core/socket_types.cppm`; удалить
      `socket_interface.cppm` (C_ISocket). **(18.07.2026)**
- [x] **0.3** D3: `TCP_NODELAY` только для
      `SOCK_STREAM`, убрать `SO_REUSEADDR`; модуль переименовать в
      `socket.factory` (файл оставить `socket_factory.cppm`). **(18.07.2026)**
- [x] **0.4** D4+D6: `CloseHandle` в деструкторе IOCP-цикла; атомарный halt. **(18.07.2026)**
- [x] **0.5** D7+D8: удалить `C_EventNode`, `LOOP_POOL`, `createLoop`;
      `C_EventLoop` — value type, `initialize()` возвращает
      `etl::expected<void, int32_t>` (WSA init внутри). Обновить `main.cpp`. **(18.07.2026)**
- [x] **0.6** `CMakePresets.json`: пресеты `clang-debug`, `clang-minsize`.
      Флаги размера по ADR-9. **(18.07.2026)**
- [x] **0.7** CMake-таргет `size-report`; baseline MinSizeRel записать сюда:
      `baseline: 14 КБ (18.07.2026)`. **(18.07.2026)**
- [x] **0.8** *Не применимо — поддержка MSVC исключена.*

**DoD:** сборка Clang зелёная, `-Wall -Wextra` без новых предупреждений,
baseline размера зафиксирован.

### Этап 1 — Ядро реактора

**Цель:** работающий IOCP backend с эмуляцией read readiness по ADR-2/ADR-3.

- [x] **1.1** Первичный `EventFlags/C_EventCallback`; после generic completion
      refactor этапа 2.3 модуль удалён, общий TCP event contract находится в
      `platform/net/tcp_driver_types.cppm`. **(18.07.2026; remove 23.07.2026)**
- [x] **1.2** `reactor/reactor_trait.cppm`: concept — `initialize/run/shutdown/
      attach/detach/post`; `static_assert` на бэкенде в `reactor.cppm`.
      Переименовать `event_loop/` → `reactor/` (обновить CMakeLists, main.cpp). **(19.07.2026)**
- [x] **1.3** Первичный IOCP dispatcher и association. На этапе 2.3 обобщён:
      `operation_iocp_s` хранит completion delegate, reactor больше не знает
      вид операции; `associate/post` возвращают `etl::expected`. **(19.07.2026;
      refactor 23.07.2026)**
- [x] **1.4** Readiness-чтение по ADR-2: 0-byte `WSARecv` → `MSG_PEEK` →
      read/closed/re-arm. На этапе 2.3 перенесено из generic reactor в
      `C_TcpSocketDriverIOCP`. **(19.07.2026; move 23.07.2026)**
- [x] **1.5** `post()`/`shutdown()`: `PostQueuedCompletionStatus` с
      зарезервированным completion key (`iocp_code_e`); `run()` исполняет posted
      tasks (проверено кросс-поточным прогоном). **(19.07.2026)**
- [x] **1.6** Smoke-тест (`src/test/smoke_loopback.cpp`): loopback TCP-пара —
      listener + connect, данные доставляются через `EV_READ`, закрытие —
      через `EV_CLOSED`. Утечки хэндлов нет (GetProcessHandleCount до/после):
      +2 хэндла один раз при первом `socket()` в процессе — ленивая подгрузка
      провайдера Winsock (живут до `WSACleanup`), дельта по итерациям = 0. **(19.07.2026)**

**DoD (19.07.2026):** smoke-тест проходит; RST пира доставляется как `EV_ERROR`
с кодом `WSAECONNRESET` (10054) — проверено прогоном с `SO_LINGER{1,0}`;
размер MinSize = 18 КБ (baseline 14 КБ; рост — в основном `<iostream>` в smoke-тесте).

### Этап 2 — tcp_socket / tcp_server

**Цель:** Qt-подобный async API поверх реактора; ноль `#ifdef` в этом слое.

- [x] **2.1** Таймеры в реакторе: intrusive-список дедлайнов, таймаут
      `GetQueuedCompletionStatus` = время до ближайшего; используются для
      connect timeout и общих задач, но не TCP write. Реализация: модули `timer` (`C_Timer`) и
      `timer.bucket` (сортированный `etl::intrusive_list`), API реактора
      `addTimer/removeTimer`; пробуждение цикла — completion `ADD_TIMER`.
      Контракт: add/remove — только на потоке цикла (ADR-7). При проверке
      исправлены: инвертированный предикат сортировки вставки и коллизия
      пустого completion (таймаут GQCS) с `SHUTDOWN` (=0) в `run()`.
      Проверено прогоном: порядок и дедлайны верны при взведении не по
      порядку, `removeTimer` отменяет срабатывание, перевзведение из
      колбэка работает. **(20.07.2026)**
- [x] **2.2** `socket/socket_address.cppm`: модуль `socket.address`, класс `C_Address` —
      обёртка над `sockaddr_storage` (IPv4 сейчас, layout IPv6-ready — дёшево
      сейчас, дорого потом). Парсинг — `ParseIPV4` в `platform/net_ops`
      (`inet_pton`, ошибка через `etl::expected`). Проверено сборкой и прогоном:
      валидный IPv4 → `size()==16`, `sa_family==AF_INET`; невалидный IP → ошибка. **(20.07.2026)**
- [x] **2.3** `tcp_socket`: generic IOCP completion dispatcher;
      `C_TcpSocketDriverIOCP` внутри `platform/net/iocp`; `ConnectEx` с wildcard
      bind; persistent read-probe; `read(span)` без `bytesAvailable`;
      caller-owned `C_TcpWriteRequest`, FIFO и proactive `WSASend` без timer/ring;
      states и безопасный `DISPOSED` после отмен. Smoke: три FIFO operations на
      двух reusable request slots, включая reentrant reuse из completion,
      loopback read и teardown; отдельный close-after-write smoke проверяет
      cancellation callbacks двух одновременно активных операций.
      MinSize executable с обоими smoke: 22 194 байт. **(23.07.2026)**
- [ ] **2.4** `tcp_server`: `listen(backlog)`, пул posted `AcceptEx` (буфер
      адресов `(sizeof(sockaddr_storage)+16)*2` на accept), `onIncoming` с
      готовым `tcp_socket`, repost. `SO_EXCLUSIVEADDRUSE` вместо `SO_REUSEADDR`.
- [ ] **2.5** `examples/echo_client.cpp` + `examples/echo_server.cpp`; гонка
      ≥ 64 МБ без потерь/рассинхрона; зафиксировать размер sample:
      `echo_server: ___ КБ` → установить бюджет.

**DoD:** echo-пара работает под нагрузкой; `tcp_socket`/`tcp_server` не содержат
`#ifdef`; бюджет размера установлен.

### Этап 3 — Доводка

- [ ] **3.1** Connect timeout (таймер + `CancelIoEx`, состояние → `Unconnected`,
      `onError(TimedOut)`).
- [ ] **3.2** Graceful close: `shutdown(SD_SEND)` → у пира `EV_CLOSED`;
      полузакрытие корректно обрабатывается.
- [ ] **3.3** Backpressure: high-watermark tx-буфера, пауза взвода чтения,
      пока tx не проседает (защита от медленного пира).
- [ ] **3.4** `to_portable()` для частых кодов: `WSAEWOULDBLOCK`,
      `WSAECONNREFUSED`, `WSAECONNRESET`, `WSAECONNABORTED`, `WSAETIMEDOUT`,
      `WSAEHOSTUNREACH`.
- [ ] **3.5** DNS: решение — v1: блокирующий `getaddrinfo` до вызова connect;
      позже `GetAddrInfoEx` overlapped. Зафиксировать выбор здесь.

**DoD:** тесты на timeout/RST/backpressure/graceful close зелёные.

### Этап 4 — Linux (epoll)

**Цель:** те же examples собираются и работают на Linux **без изменений** —
проверка унитарности интерфейсов.

- [ ] **4.1** `reactor/impl/epoll/reactor_epoll.*`: `epoll_create1`, LT-режим
      и `eventfd` для `post()/shutdown()`; reactor остаётся generic
      demultiplexer без знания TCP операций.
- [ ] **4.2** `platform/net/epoll/tcp_driver_epoll.*`: `EPOLLIN` →
      `READ_READY`; active write продолжать по `EPOLLOUT`, отключая interest
      после полного request; тот же `CONNECTED/CLOSED/IO_ERROR/DISPOSED`
      contract, что у IOCP driver.
- [ ] **4.3** `platform/wsa_initializer` — на Linux отсутствует; `CloseSocket`
      → `::close`; `last_error()` → `errno`.
- [ ] **4.4** CI/пресет Linux-сборки; прогон smoke + echo на обеих ОС.

**DoD:** user-код examples идентичен на обеих платформах; отличия только в
`reactor/impl` и `platform/`.

---

## 7. Правила разработки (для человека и ИИ)

1. **Никаких новых зависимостей.** Только WinAPI/Winsock2 и ETL.
2. **Запрещено в коде библиотеки:** исключения, RTTI, `new/delete` напрямую,
   `std::function`, `std::error_code`, `iostream`, `printf`-логирование.
3. `noexcept` по умолчанию на всех публичных функциях; ошибки — только через
   `etl::expected` (ADR-6).
4. Платформенный код — только в `reactor/impl/*` и `platform/*`; transport
   drivers находятся в `platform/net/*`. Верхние слои без `#ifdef` (ADR-1).
5. Один pending OVERLAPPED на направление на сокет; лайфтайм по ADR-5.
6. Каждый новый модуль — в `FILE_SET CXX_MODULES` в `CMakeLists.txt`; имя файла
   == имя модуля.
7. После каждого этапа — прогон `size-report`, сравнение с baseline/budget.
8. Минимальные изменения: задача не должна тащить рефакторинг соседнего кода.
9. Язык проекта: RU/ENG — комментарии и документация допустимы на обоих языках.

---

## 8. Открытые вопросы (отложенные решения)

- **EV_WRITE наружу:** транслировать ли writability пользователю (таймер-подход
  или AFD poll)? v1 — нет; пересмотреть после Этапа 3, если появится спрос.
- **DNS:** блокирующий `getaddrinfo` vs overlapped `GetAddrInfoEx` — решение
  фиксируется в задаче 3.5.
- **UDP:** вне v1; `socket_factory` не должен её блокировать (см. D3).
- **TLS:** out of scope (отдельный проект/слой поверх).
- **Мультипоточный `run()`** (несколько потоков на один IOCP): конструкция
  зарезервирована (ADR-7), не реализуется в v1.
