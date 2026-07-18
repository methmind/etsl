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

### ADR-1. Точка разделения платформ — реактор, не сокет

Единый фасад реактора с одинаковой семантикой событий на всех ОС; `tcp_socket`
пишется один раз без `#ifdef`. Сейчас платформа протекает на всех уровнях
(`WINNT` в пяти файлах) — это исправляется слоением, а не новыми `#ifdef`.

### ADR-2. Эмуляция readiness поверх IOCP

- **Чтение:** на сокет взводится ровно один pending **0-byte `WSARecv`**. Его
  completion всегда имеет `bytesTransferred == 0` (данные не копируются), поэтому
  «данные пришли» и «peer закрыл соединение» из completion неотличимы. После
  completion делается неблокирующий `recv(buf, 1, MSG_PEEK)`:
  `> 0` → событие `EV_READ`; `0` → `EV_CLOSED`; `WSAEWOULDBLOCK` → ложное
  срабатывание → перевзвести. Level-triggered: после возврата обработчика проба
  взводится заново — если пользователь не вычитал всё, событие повторится
  (пейсинг естественный, через async completion, busy loop невозможен).
- **Запись:** **0-byte `WSASend` НЕ работает** — по MSDN он завершается немедленно
  и не является сигналом записываемости. Вместо этого: оптимистичная запись сразу;
  при `WSAEWOULDBLOCK` остаток копится в tx ring buffer, повтор flush по
  backoff-таймеру реактора. Наружу `EV_WRITE` в v1 не транслируется (как в Qt,
  где `bytesWritten` эмитится из внутреннего буфера).
- **Accept/Connect:** эмуляция не нужна — `AcceptEx`/`ConnectEx` нативно
  completion-based; их завершение мапится прямо в `onIncoming`/`onConnected`.
- **Отвергнуто:** `WSAEventSelect` + `WSAWaitForMultipleEvents` (лимит 64 handle
  на ожидание, лишний поток); AFD poll в стиле wepoll (недокументированный API).

### ADR-3. Диспетчеризация — `etl::delegate`, единый колбэк событий

```cpp
// Флаги событий — общие для всех платформ, 1:1 мапятся на epoll позже
enum EventFlags : uint8_t {
    EV_READ   = 0x1,
    EV_WRITE  = 0x2,
    EV_CLOSED = 0x4,
    EV_ERROR  = 0x8,
};
using C_EventCallback = etl::delegate<void(uint8_t events, int32_t error)>;
```

Один делегат (16 байт, без хипа, принимает лямбды) вместо виртуальных иерархий.
**Отвергнуто:** `C_IEventHandler` (три виртуальных метода), `C_EventNode`,
`C_ISocket` — удаляются в Этапе 0/1.

### ADR-4. Владение памятью — библиотека не аллоцирует

- Event loop — value type на стеке: `C_EventLoop loop; loop.initialize(); loop.run();`
  **Отвергнуто:** `etl::pool` + `createLoop()` + `unique_ptr` (машинерия без
  выигрыша — цикл создаётся один раз).
- Registration-структура реактора (содержит `WSAOVERLAPPED`) — **член объекта-
  владельца** (например, `tcp_socket`). Реактор хранит только указатель.
- Буферы сокета — фиксированный ring buffer, ёмкость — шаблонный параметр.

### ADR-5. Лайфтайм OVERLAPPED (классическая ловушка IOCP)

`WSAOVERLAPPED` обязан жить до завершения операции. Правило v1:
`detach()`/деструктор владельца = `CancelIoEx` + ожидание aborted-completions
**на потоке цикла**. Уничтожение объекта с pending-операциями вне этой
процедуры — запрещено контрактом API.

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

### ADR-8. Qt-подобие — только async

Копируем из `QAbstractSocket`: состояния
(`Unconnected/Connecting/Connected/Closing`), async `connectToHost`,
`read/bytesAvailable/write`, колбэки `onReadyRead/onConnected/onDisconnected/
onError`. **Отвергнуто:** все `waitFor*` — главный источник веса в Qt и
антипаттерн для реактивной модели.

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
│   ├── events.cppm            # EventFlags, C_EventCallback            (новое, Этап 1)
│   ├── error.cppm             # last_error(), to_portable()            (новое, Этап 1)
│   └── socket_types.cppm      # единственное определение socket_t      (Этап 0, из socket.cppm)
├── reactor/                   # переименование event_loop/ (Этап 1):
│   │                          # имя должно отражать readiness-семантику
│   ├── reactor.cppm           # alias C_Reactor = бэкенд по _WIN32/__linux__
│   ├── reactor_trait.cppm     # concept + static_assert бэкенда
│   └── impl/
│       ├── reactor_iocp.cppm/.cpp   # Windows: ADR-2                (Этап 1)
│       └── reactor_epoll.cppm/.cpp  # Linux                          (Этап 4)
├── socket/
│   ├── socket.cppm/.cpp       # RAII-хэндл + close (слить raii+independent)
│   ├── socket_factory.cppm    # модуль socket.factory (имя файла = имя модуля)
│   ├── address.cppm           # обёртка над sockaddr_storage            (Этап 2)
│   ├── tcp_socket.cppm/.cpp   # Qt-подобный async API                  (Этап 2)
│   └── tcp_server.cppm/.cpp   # AcceptEx                               (Этап 2)
└── platform/
    └── wsa_initializer.cppm/.cpp  # только Windows; на Linux — отсутствует
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
  Файлы: `src/event_loop/event_loop.cppm:17`, `src/socket/socket.cppm:5`,
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
- **D4.** `src/event_loop/impl/event_loop_iocp.cppm:18`: деструктор не делает
  `CloseHandle(iocp_)` — утечка хэндла.
- **D5.** `src/event_loop/impl/event_loop_iocp.cpp:33-35`: все failed completions
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

**Цель:** работающий IOCP-реактор с эмуляцией readiness по ADR-2/ADR-3.

- [x] **1.1** `core/events.cppm`: `EventFlags`, `C_EventCallback` (сигнатуры из ADR-3). **(18.07.2026)**
- [ ] **1.2** `reactor/reactor_trait.cppm`: concept — `initialize/run/shutdown/
      attach/detach/post`; `static_assert` на бэкенде в `reactor.cppm`.
      Переименовать `event_loop/` → `reactor/` (обновить CMakeLists, main.cpp).
- [ ] **1.3** `reactor/impl/reactor_iocp.*`: registration struct
      `{ SOCKET fd; uint8_t interest; C_EventCallback cb; WSAOVERLAPPED read_ov; state; }`
      (живёт у владельца, ADR-4); `attach` = `CreateIoCompletionPort((HANDLE)fd,
      iocp, (ULONG_PTR)&reg, 1)`; dispatch через `CONTAINING_RECORD`; ошибки —
      `WSAGetOverlappedResult` → `EV_ERROR` + код (D5).
- [ ] **1.4** Readiness-чтение по ADR-2: arm 0-byte `WSARecv` → completion →
      `MSG_PEEK` проба → `EV_READ`/`EV_CLOSED`/re-arm; level-triggered перевзвод.
- [ ] **1.5** `post()`/`shutdown()`: `PostQueuedCompletionStatus` с
      зарезервированным completion key; `run()` исполняет posted tasks.
- [ ] **1.6** Smoke-тест (`tests/smoke_loopback.cpp`): loopback TCP-пара —
      listener + connect, данные доставляются через `EV_READ`, закрытие —
      через `EV_CLOSED`. Проверить отсутствие утечки хэндлов
      (GetProcessHandleCount до/после).

**DoD:** smoke-тест проходит; ошибка (RST пира) доставляется как `EV_ERROR`
с корректным кодом; размер не вырос > baseline + согласованный порог.

### Этап 2 — tcp_socket / tcp_server

**Цель:** Qt-подобный async API поверх реактора; ноль `#ifdef` в этом слое.

- [ ] **2.1** Таймеры в реакторе: intrusive-список дедлайнов, таймаут
      `GetQueuedCompletionStatus` = время до ближайшего; нужны для backoff
      записи и connect timeout.
- [ ] **2.2** `socket/address.cppm`: обёртка над `sockaddr_storage`
      (IPv4 сейчас, layout IPv6-ready — дёшево сейчас, дорого потом).
- [ ] **2.3** `tcp_socket`: состояния по ADR-8; `ConnectEx` (гоча: перед
      `ConnectEx` сокет обязан быть `bind()` к wildcard); `read(span)`/
      `bytesAvailable()`/`onReadyRead`; `write(span)` → оптимистичный send →
      остаток в tx ring buffer → flush по backoff-таймеру (ADR-2); колбэки
      `onConnected/onDisconnected/onError(int32_t)`; член `C_Registration`.
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

- [ ] **4.1** `reactor/impl/reactor_epoll.*`: `epoll_create1`, LT-режим
      (соответствует level-triggered семантике ADR-2), `EPOLLIN/EPOLLOUT/
      EPOLLHUP/EPOLLERR` → те же `EventFlags`; `eventfd` для `post()/shutdown()`.
- [ ] **4.2** `C_Registration` epoll-версии — худощавая (без OVERLAPPED);
      layout разный, тип выбирается alias'ом — код владельцев не меняется.
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
4. Платформенный код — только в `reactor/impl/*` и `platform/*`; верхние слои
   без `#ifdef` (ADR-1).
5. Один pending OVERLAPPED на направление на сокет; лайфтайм по ADR-5.
6. Каждый новый модуль — в `FILE_SET CXX_MODULES` в `CMakeLists.txt`; имя файла
   == имя модуля.
7. После каждого этапа — прогон `size-report`, сравнение с baseline/budget.
8. Минимальные изменения: задача не должна тащить рефакторинг соседнего кода.
9. Комментарии и идентификаторы — английский; этот файл — на русском.

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
