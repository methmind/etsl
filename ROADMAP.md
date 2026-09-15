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

- Реактивная модель (reactor), события доставляются колбэками делегата.
- Windows — первая платформа (IOCP), Linux (epoll) — вторая. Код пользователя
  платформенно-нейтрален: «написал один раз — работает везде».
- Прообраз API — сокеты Qt (`QAbstractSocket`/`QTcpSocket`/`QTcpServer`),
  но **только async-часть** (никаких `waitFor*`); эволюционировал в делегатную
  модель без event-enum (ADR-3).
- **Полу-проактивная модель I/O** (ADR-2): чтение — проактивная 0-byte
  `WSARecv`-проба + синхронный `read()` в колбэке; запись — полностью
  проактивные `WSASend` с caller-owned операциями. Идея эмуляции readiness
  таймерами отвергнута.
- Основа: ETL (Embedded Template Library), C++20 modules, `noexcept`-дисциплина.

Нефункциональные требования:

1. Минимальный размер бинаря — измеряемая метрика, не декларация (см. Этап 0).
2. Библиотека не выполняет heap-аллокаций сама; объекты создаёт владелец
   (стек/статик/пул пользователя).
3. Без исключений и RTTI в коде библиотеки.
4. ОС-специфичный код — только в партициях `<feature>/<os>/` и ос-шинах;
   контракты (defs/delegate/alias) не содержат вызовов ОС (ADR-10).

---

## 2. Окружение и сборка (обновлено 11.09.2026)

- Основной режим — **кросс-компиляция из Linux под Windows** через
  **MinGW-тулчейн** `~/Документы/toolchain/windows-clang-mingw.cmake` (clang,
  target `x86_64-w64-mingw32`, sysroot `/usr/x86_64-w64-mingw32/sys-root/mingw`),
  каталог сборки `cmake-build-debug-clang-mingw-windows/` (Ninja, Debug; таргеты
  `etsl_core`, `etsl`, `tcp_connection_iocp_test`). Прежний MSVC-target тулчейн
  (`windows-clang.cmake`, target `x86_64-pc-windows-msvc`, SDK в `~/winsdk`) на
  машине **отсутствует** (проверено 11.09.2026): каталоги
  `cmake-build-debug-clang-windows/` и `cmake-build-minsize-clang-windows/`
  сконфигурированы под него и не реконфигурируются — **MinSizeRel-сборки и
  `size-report` сейчас нет**, пока не заведён MinSizeRel-каталог на MinGW
  (замеры ADR-9 сделаны на MSVC-target и с MinGW напрямую не сравнимы).
  Пресеты `clang-debug`/`clang-minsize` из `CMakePresets.json` рассчитаны на
  MSYS2-окружение и в Linux-кросс-сборке не используются.
- Запуск и отладка — Windows-ВМ `win-dev` (192.168.122.51, ключ
  `~/.ssh/win-dev`, пользователь `sexey`, каталог `C:/dev/`); POST_BUILD-хук
  таргета `etsl` деплоит exe по SSH/scp (D9 закрыт). Если ВМ не отвечает,
  `ninja … etsl` падает **после** успешной линковки — для проверки сборки
  собирать `etsl_core` и тест отдельно.
- MSVC (`cl`) **не поддерживается** и не планируется; проект ориентирован
  исключительно на Clang.
- ETL **20.49.0** (`external/etl`, submodule; обновлён 12.09.2026 с 20.48.1 —
  закрывает D14, `expected::operator*() const&`/`&&`; на размер повлиял на
  −32 байта, прогон после бампа 44/44 ×3 + обе нагрузки echo. Ранее: 20.47.1 →
  20.48.1 застейджен 28.08.2026, тот бамп дал −1 472 байта, см. ADR-9).
  Исключения внутри ETL выключены по умолчанию (`ETL_THROW_EXCEPTIONS`
  не определён, `external/etl/include/etl/platform.h:287`)
  — дополнительных макросов для ETL не нужно; нужно лишь компилировать сам проект
  без исключений.
- CMake ≥ 4.2, `CMAKE_CXX_SCAN_FOR_MODULES ON`. Модули перечислены в
  `FILE_SET CXX_MODULES` в `CMakeLists.txt` — **каждый новый модуль добавлять туда**.
- **Тесты:** GoogleTest v1.17.0 (FetchContent), опция `ETSL_BUILD_TESTS`; набор
  `test/tcp_connection_iocp_test.cpp` — 49 тестов, Windows/IOCP-only (на
  не-Windows сборка suite отключена). Из-за `CMAKE_CROSSCOMPILING` тест
  регистрируется одним `add_test` без `gtest_discover_tests`; прогон —
  скопировать exe на ВМ и запустить. *(07.09.2026: прогон 35/35 PASSED после
  закрытия D10; рост с 30 тестов — покрытие adopt, empty-span send,
  read-WOULDBLOCK. 10.09.2026: набор переписан на связку C_TCPAcceptor +
  C_TCPConnection — loopback «библиотека против библиотеки» на одном реакторе,
  однопоточно: слушающая сторона — акцептор, принятые сокеты раздаются
  peer-соединениям через `adopt()`; самодельный blocking-LocalListener и
  фоновые потоки удалены. 40/40 PASSED (3 прогона подряд, Debug, MinGW-тулчейн
  `windows-clang-mingw.cmake`, каталог `cmake-build-debug-clang-mingw-windows/`
  — прежний каталог `cmake-build-debug-clang-windows/` остался на удалённом
  msvc-тулчейне). 35 тестов наследованы (контракт connection), +5 акцепторных
  — формальное закрытие фазы №3, см. 2.4. 11.09.2026 (вечер): **44 теста,
  44/44 ×3** — +`AcceptorDisposeFromOnIncomingDrainsBacklogAndKeepsSession`,
  `AcceptorSingleSlotBacklogTeardownThroughLastOperation`,
  `AcceptorReuseAfterDisposedReinitializesCleanly`. Ранее в тот же день: 41 тест (+покрытие
  адреса пира); после финальной правки акцептора и обновления ожиданий двух
  акцепторных тестов — **41/41 ×3**; +тест `dispose()` из `onIncoming` —
  **42/42 ×3**; +2 теста (пул на 1 слот, реюз после `onDisposed`) —
  **44/44 ×3**, см. 2.4. 13–14.09.2026: после перевода подачи на
  `Assign`/`inFlight` (ADR-11) и интеграции `suspendReading`/`resumeReading` —
  **44/44** (Debug, MinGW); новые пути тестами не покрыты, см. 3.3/3.6.
  14.09.2026 (позже): +4 теста на `suspendReading`/`resumeReading` (guard-коды,
  пауза из `onReadyRead`, suspend из таймера, resume при пробе в полёте) —
  **48/48 ×3**, см. 3.3. 14.09.2026 (ночь):
  +`SuspendedDataDrainedByReadThenResumeRearmsProbe` (проверка «кандидата в
  дефекты» из прогонов 3.3 — не подтвердился) — **49/49 ×3**.)*
- Тестовый exe на ВМ deploy-хуком **не** обновляется (хук есть только у
  `etsl`) — перед прогоном заливать свежую сборку (команды ниже), иначе
  запускается устаревший бинарь.
- Результаты ревью кода — `review/result.md` (ревью акцептора 11.09.2026:
  находки, прогоны, статусы).

Команды:

```bash
B=cmake-build-debug-clang-mingw-windows
ninja -C $B tcp_connection_iocp_test                      # тесты
ninja -C $B etsl_core CMakeFiles/etsl.dir/main.cpp.obj    # сборка без deploy-хука
ninja -C $B etsl                                          # exe + deploy-хук
# прогон на ВМ:
scp -i ~/.ssh/win-dev $B/test/tcp_connection_iocp_test.exe sexey@192.168.122.51:C:/dev/
ssh -i ~/.ssh/win-dev sexey@192.168.122.51 "C:\dev\tcp_connection_iocp_test.exe --gtest_brief=1"
```

Грабли: после отката исходника через `cp -p` mtime старый — ninja не
пересобирает; делать `touch`.

---

## 3. Принятые архитектурные решения (ADR)

Формат: решение → почему → что отвергнуто. Эти решения финальны, если явно не
пересмотрены с записью здесь.

### ADR-1. Платформенные границы — `reactor/<platform>` и `platform`, не `socket`

> **Пересмотрено 25.08.2026 (ADR-10):** каталог `platform/` ликвидируется;
> структурные границы переопределены как фича-партиции `<feature>/<os>/`.
> Ниже — историческая запись прежней политики (пути `platform/net/tcp/…`
> после задачи 2.7 читать как `net/tcp/…`).

`reactor/<platform>` (фактически `reactor/iocp/`) содержит только
demultiplexer/completion backend. Transport-specific OS operations находятся в
`platform/net/tcp/<platform>`; `platform/net/tcp/tcp_connection.cppm`
выбирает реализацию alias-ом (`C_TCPConnection<T>` = IOCP-реализация).
Отдельный generic `tcp_socket`-слой поверх соединения отсутствует:
пользовательский делегат удовлетворяет `TCPConnectionDelegate` (ADR-3) и сам по
себе платформенно-нейтрален. Platform TCP connection использует reactor для
association/completion, но reactor не зависит от connection. Размещение
platform connection в `reactor` или `socket` запрещено.

### ADR-2. Полу-проактивная модель I/O (пересмотрено 2026-08-20 и 07.09.2026)

Идея приводить IOCP к чисто реактивному (readiness) режиму отвергнута, как и
эмуляция readiness таймерами. Принята полу-проактивная модель:

- **Чтение:** на сокет взводится ровно один pending **0-byte `WSARecv`**
  (read-probe). Его completion означает «данные доступны» и транслируется в
  `onReadyRead()`. Пользователь читает синхронно из колбэка: `read(span)` →
  `recv`; success-канал несёт только реальный счётчик прочитанных байтов;
  `WSAEWOULDBLOCK` → `unexpected(WSAEWOULDBLOCK)` без teardown («данных нет»;
  пересмотрено 07.09.2026 — прежде возвращался success 0); `recv() == 0` →
  EOF → teardown → `onDisconnect(0)`; прочие ошибки `recv` → teardown +
  информационный sync-код, терминальный колбэк доедет отложенно. После
  возврата `onReadyRead()` проба взводится заново — level-triggered
  семантика, пейсинг через async completion. *Уточнено 12.09.2026:* «busy loop
  невозможен» верно, **только пока `onReadyRead` дочитывает сокет до
  `WSAEWOULDBLOCK`**. Если положить данные некуда (все буферы пользователя
  заняты незавершёнными `WSASend`), безусловный перевзвод пробы даёт холостой
  цикл: замер с неотвечающим пиром — 16 091 573 вызова `onReadyRead` за ~10 с,
  из них 16 091 403 без единого прочитанного байта (ядро под 100%); у
  здорового клиента — 2–10 таких вызовов на сессию. Лечится паузой взвода
  чтения (3.3; `suspendReading()`/`resumeReading()` интегрированы 14.09.2026,
  D18 закрыт).
  **Пауза чтения (контракт, 14.09.2026):** `suspendReading()` отменяет только
  перевзвод — уже взведённую пробу не отзывает; её завершение на паузе
  подавляется в `invokeReadyRead` (`onReadyRead` **не приходит**, проба не
  перевзводится). `resumeReading()` снимает паузу и взводит пробу; если проба
  ещё в полёте — успех без второго взвода. Данные, пришедшие на паузе, после
  resume доставляются сразу (0-byte `WSARecv` на непустом буфере завершается
  немедленно, пакет всё равно ставится в порт); данные, прочитанные на паузе
  вручную через `read()`, повторно не уведомляются (level-triggered). EOF и
  RST на паузе: пока проба ещё взведена, RST приходит завершением с ошибкой →
  teardown, а EOF (успешное завершение) подавляется; после подавленного
  завершения пробы нет вовсе — EOF/RST обнаруживаются после `resumeReading()`
  или на отказе `send` (как `uv_read_stop`). **Отвергнуто:** `CancelIoEx` в
  `suspendReading()` — отмена асинхронна (завершение может уже лежать в
  порту, фильтр нужен всё равно), стоит syscall и гарантированный пакет
  `ERROR_OPERATION_ABORTED` на каждую паузу и требует отличать такую отмену
  от teardown в `onReadinessOperation`.
  `MSG_PEEK`-фильтрация удалена: на IOCP probe срабатывает на реальный приём,
  EOF детектится через `read()` → `unexpected(0)`.
- **Запись (полностью проактивная):** `send(span, send_operation_t&)` немедленно
  порождает `WSASend`; частичные завершения соединение дозавершает сам
  (`transferred +=`, досылка остатка). Каждая операция — caller-owned контекст
  `send_operation_t` (наследует `operation_t`/`WSAOVERLAPPED`); параллельно
  допустимо произвольное число операций, каждая завершается ровно одним
  терминальным `onCommit(operation, error)`. Порядок доставки гарантирует TCP
  (порядок вызовов `WSASend` на сокете). FIFO, TX ring buffer и timer backoff
  в соединении отсутствуют — очередь операций при необходимости держит
  пользователь. Контракт ошибок (07.09.2026): usage-отказы — `WSAEINVAL`
  (пустой span) и `WSAENOTCONN` (не `CONNECTED`) — синхронный `unexpected`
  без side-эффектов; синхронный отказ взвода `WSASend` терминален:
  `beginTeardown` + информационный sync-код, операция не взведена, `onCommit`
  по ней не придёт (симметрично терминальным отказам `read`). На epoll write
  продолжается по `EPOLLOUT` (Этап 4).
- **Accept/Connect:** эмуляция не нужна — `AcceptEx`/`ConnectEx` нативно
  completion-based; их завершение мапится прямо в `onIncoming`/`onConnect`.
- **Контракт association:** generic reactor только связывает fd с backend.
  Взвод connect/read/write и обязательный nonblocking mode — ответственность
  platform TCP connection/socket factory.
- **Отвергнуто:** эмуляция readiness таймерами (таймеры реактора — только
  пользовательские задачи: connect timeout, периодика); `MSG_PEEK` после probe;
  0-byte `WSASend` (завершается немедленно, writability не сигнализирует);
  `WSAEventSelect` + `WSAWaitForMultipleEvents` (лимит 64 handle на ожидание,
  лишний поток); AFD poll в стиле wepoll (недокументированный API).

### ADR-3. Диспетчеризация — `etl::delegate` + делегат соединения

```cpp
// Реактор не знает тип операции. Подача — только C_Reactor::Assign (ADR-11).
struct operation_iocp_s : WSAOVERLAPPED {
    etl::delegate<void(operation_iocp_s& operation, uint32_t bytes, int32_t error)> callback;
    bool inFlight; // ставит Assign, снимает run() до callback; последним — ADR-11
};
```

IOCP reactor доставляет raw completion делегату операции. Generic-слоя
`tcp_socket` с event-enum (`EV_*`) больше нет: platform connection
`C_TCPConnectionIOCP<T>` вызывает методы делегата пользователя напрямую.
Контракт проверяется концептом `TCPConnectionDelegate` (`static_assert` в
конструкторе соединения):

- `onConnect(int32_t error)` — завершение `ConnectEx` (или неудачный коннект
  после teardown). `adopt()` его не вызывает: сокет принят уже подключённым,
  вызывающий знает это синхронно, — первое событие делегата принятого сокета
  `onReadyRead` либо `onDisconnect`. **Пересмотрено 12.09.2026:** любой отказ
  `adopt()` синхронен и терминальных колбэков не порождает — при неудаче взвода
  read-пробы соединение откатывается (сокет закрыт, состояние `NONE`), как в
  `connect()`. Прежняя запись 07.09 («придёт `onDisconnect(err)`») недействительна;
- `onReadyRead()` — данные доступны; пользователь зовёт `read(span)`; пока
  чтение на паузе (`suspendReading()`), не приходит (ADR-2);
- `onCommit(operation_t&, int32_t error)` — терминальное завершение
  send-операции, включая отменённую (`ERROR_OPERATION_ABORTED` при dispose —
  сигнал освобождать контекст);
- `onDisconnect(int32_t error)` — разрыв после установленного соединения
  (EOF/RST; error == 0 при graceful);
- `onDisposed()` — teardown завершён после явного `dispose()`.

Акцептор (`C_TCPAcceptorIOCP<T>`, концепт `TCPAcceptorDelegate`, 11.09.2026):

- `onIncoming(C_Socket fd, const C_Address& remoteAddr)` — принятый сокет
  (передача владения, вызов `onIncoming(std::move(fd), …)`) и адрес пира,
  разобранный `GetAcceptExSockaddrs` и скопированный в `C_Address` до перевзвода
  (буфер операции переиспользуется). Контракт — **только адрес пира**: его
  нативно отдаёт и `accept4` на epoll; локальный адрес там стоит `getsockname`;
- `onError(int32_t error)` — некритичная ошибка, акцептор продолжает работу:
  отказ completion одного соединения (напр. RST до accept; код через
  `TranslateError`), отказ `SO_UPDATE_ACCEPT_CONTEXT`/разбора адреса, отказ
  перевзвода слота. Решение о сносе при устойчивых ошибках — за делегатом
  (`dispose()` из колбэка допустим). Отмены при `dispose()` в `onError` **не**
  попадают;
- `onDisposed(int32_t reason)` — терминальный колбэк; `reason == -1`
  (`EXPLICIT_DISPOSE`) после явного `dispose()`, иначе первая критичная
  причина (пул опустел). Если пул опустел из-за отказа перевзвода последнего
  слота, делегат получает сначала `onError(err)` (некритичный отказ слота),
  затем `onDisposed(err)` (критичное опустение пула) — два разных события,
  а не дублирование.

*Пересмотрено 13.09.2026:* прежняя запись («валидный callback одновременно
обозначает in-flight operation; reactor очищает его перед вызовом, отдельного
`pending` flag нет») с кодом разошлась — реактор callback не очищает, акцептор
по `is_valid()` лишь лениво создаёт делегат. Состояние «в полёте» — явный флаг
`inFlight`, подача — `C_Reactor::Assign` (ADR-11). Виртуальных иерархий и heap
type-erasure нет.

### ADR-4. Владение памятью — библиотека не аллоцирует

- Event loop (reactor) — value type на стеке: `C_Reactor loop; loop.initialize(); loop.run();`
  **Отвергнуто:** `etl::pool` + `createLoop()` + `unique_ptr` (машинерия без
  выигрыша — цикл создаётся один раз).
- IOCP operation (содержит `WSAOVERLAPPED`) — член platform connection; reactor
  хранит указатель только до completion.
- Write payload и `send_operation_t` принадлежат пользователю и живут до
  единственного терминального `onCommit`; переиспользование или разрушение
  контекста до завершения запрещено (реактор пишет в его `WSAOVERLAPPED` через
  `Assign`, ADR-11; повторная подача операции в полёте — D17).
  Соединение не ведёт очередей: пользователь может держать несколько независимых
  операций одновременно (ADR-2), аллокаций нет.

### ADR-5. Лайфтайм OVERLAPPED и teardown (обновлено 2026-08-20)

`WSAOVERLAPPED` обязан жить до завершения операции. Соединение хранит стабильные
`readinessOperation_` (ConnectEx/read-probe) и `disposeOperation_`;
send-операции — caller-owned (`send_operation_t`). Счётчик `pendingOps_`
отслеживает активные операции. Teardown — единый путь `beginTeardown(reason)`:
RAII-закрытие сокета (`C_Socket::dispose()` → `closesocket`) автоматически
отменяет все pending I/O (completions приходят с `ERROR_OPERATION_ABORTED`);
когда `pendingOps_` обнуляется, в реактор постится `disposeOperation_`
(`detach`), по завершении которого соединение возвращает состояние в `NONE` и
выдаёт терминальный колбэк: `EXPLICIT_DISPOSE` → `onDisposed()`; иначе
`wasConnected_ ? onDisconnect(reason) : onConnect(reason)`. Отменённая
send-операция всё равно получает терминальный `onCommit(ERROR_OPERATION_ABORTED)`
— пользователь может освободить контекст. `CancelIoEx` и подмена callback на
cancellation-хэндлеры не используются. Уничтожение объекта раньше терминального
колбэка запрещено (assert на `pendingOps_` в деструкторе).

Акцептор (11.09.2026) — тот же паттерн: роль `pendingOps_` играет
`backlog_.size()` (пул `accept_operation_t`). Каждый completion сначала
**освобождает слот**, потом решает: успешный перевзвод — операция
переиспользуется (без `destroy`/`create`); отказ перевзвода — `destroy`;
затем единый хвост `armAcceptBacklog()` (восполняет свободные слоты; вне
`LISTENING` gateway нет — возвращает `WSA_OPERATION_ABORTED`) → при ошибке
`beginTeardown`. `beginTeardown` вызывается на каждом отменённом completion —
это штатно: проверка «операций не осталось → `detach`» живёт только внутри
него, повторы безопасны (кеш причины, `is_linked()`). Код
`WSA_OPERATION_ABORTED` внутри акцептора означает «не `LISTENING`» и
отфильтровывается перед `onError`. Порядок «освободить → решить» обязателен:
проверка пустоты пула до `destroy` теряет `onDisposed` (D12).

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

Копируется async-семантика состояний, но API — делегатный (ADR-3): запись —
`send(span, send_operation_t&)` с единственным терминальным `onCommit`
(`error == 0` — успех); после `onReadyRead()` пользователь вызывает
`read(span)`. `bytesAvailable` удалён как TOCTOU и лишний syscall.
**Отвергнуто:** все `waitFor*`.

### ADR-9. Размер — измеряемая метрика

Флаги: clang → `-Os -ffunction-sections -fdata-sections -fno-exceptions
-fno-rtti -flto`; линковка — DCE/схлопывание: в MSYS2 (GNU ld) это
`-Wl,--gc-sections -s`. CMake-таргет `size-report` (`size -A` по секциям);
сводная метрика — `dec` из `size` (text+data+bss). Бюджет размера
устанавливается после первого замера (Этап 0), а не выдумывается заранее.

Особенность кросс-сборки (проверено 20.08.2026): линкер lld-link
(MSVC-таргет) **молча игнорирует** GNU-опции `--gc-sections`/`-s` — без DCE
замер фиктивен (etsl.exe: text 404 890 байт); рабочие опции —
`/OPT:REF` + `/OPT:ICF`. MinSizeRel-конфигурация кросс-сборки —
каталог `cmake-build-minsize-clang-windows/`; size-флаги передаются через
`CMAKE_CXX_FLAGS_MINSIZEREL`/`CMAKE_EXE_LINKER_FLAGS_MINSIZEREL` (базовые
CFLAGS/LINKER_FLAGS задаются toolchain-файлом обычными переменными и cache
`-D` не перекрывают).

Раскладка веса (etsl.exe MinSizeRel, dec, 20.08.2026) — выбор CRT решает всё:

| Рантайм (toolchain) | dec | Примечание |
|---|---|---|
| `MultiThreadedDLL` (динамический CRT) | **12 647 байт** | сопоставим с июльской серией MSYS2 (baseline 14 КБ, 22 194 байт со smoke в 2.3); требует `vcruntime140.dll` на цели (`ucrtbase.dll` есть в Win10+) |
| `MultiThreaded` (статический CRT, текущий toolchain) | 97 091 байт | из них 91 366 байт — CRT-подложка пустого `int main(){}` с теми же флагами; **вклад кода etsl ≈ 5,7 КБ** |

Baseline для сопоставления с июльской серией — **12 647 байт (DLL CRT)**:
библиотека после августовских рефакторингов не распухла (меньше всех прежних
замеров — текущий `main.cpp` крошечный, smoke-тестов с `<iostream>` больше нет).
Статическая CRT — цена самодостаточного exe, а не роста библиотеки; выбор
рантайма остаётся за владельцем toolchain. Замер после реструктуризации 2.7
(25.08.2026): dec **96 371** (−720 к baseline — переезд и префиксы имён
модулей размера не добавили). Замер 28.08.2026 (разделение ос-шин net/init
на `<os>/` impl-юниты, см. §4/§7.4): dec **97 299**, при этом вклад самого
разделения — **0 байт** (в каталогах сборки одинаковой длины пути HEAD и
разделение дают идентичный dec 97 251, .text совпадает байт-в-байт).
Прирост к эталону 96 371 разлагается на: ~+2,4 КБ — пост-замерные изменения
дерева, вошедшие в коммит 728756e (timer-реструктуризация, модуль init,
акцептор); −1 472 — незакоммиченный бамп сабмодуля ETL 20.47.1 → 20.48.1.
Метрический артефакт `dec`: для PE колонка text включает .rdata с absolute-
путём к PDB (debug-директория) — длина имени каталога сборки сама по себе
меняет dec (~50 байт, UTF-8 кириллица = 2 байта/символ); сравнивать сборки
из каталогов одинаковой длины пути.

### ADR-10. Структура — вертикальные фича-компоненты; `platform/` ликвидируется (25.08.2026)

Пересмотр исходной концепции «`platform/` — тонкие платформенные обёртки,
выше — высокоуровневая абстракция над ними». Факты на момент пересмотра:
`src/platform/` вмещает 17 из 30 файлов библиотеки (~64% строк), включая
крупнейшие (`tcp_connection_iocp.cppm`, 388 строк; `tcp_acceptor_iocp.cppm`,
131); обёрток над этими компонентами нет — `main.cpp` и gtest используют
`C_TCPConnection`/`C_TCPAcceptor` напрямую.

**Решение.** Раскладка по фичам, не по слоям. Каждая фича — вертикальный срез:

- корень фичи — **контракт**: `*_defs` (типы операций), delegate-концепт,
  alias-umbrella (`C_TCPConnection<T>` = `C_TCPConnectionIOCP<T>` по
  `_WIN32`). Ноль вызовов ОС;
- `<feature>/<os>/` — **самодостаточные бекенды** (`iocp/` сейчас, `epoll/` —
  Этап 4): полная реализация — стейт-машина, операции, teardown. Generic-слоя
  между контрактом и бекендом нет и не планируется;
- переиспользование между ОС — только контракт и ос-шины (`etsl.net:*`;
  ветвление ОС — в global module fragment).

Безфичевое OS-клей: `win.wsa` (единственный потребитель — `reactor_iocp.cpp`)
→ `reactor/iocp/`; `etl_chrono.cpp` (QPC/`clock_gettime` для ETL) → `util/`.
Целевое дерево — §4; физический переезд — задача 2.7. Имена модулей и
партиций от путей не зависят (`net.tcp_connection:iocp` и т.д.) — переезд
меняет только `FILE_SET` в CMakeLists.txt.

**Почему.** IOCP (проактор) и epoll (реактор) фундаментально различны: чтение —
0-byte `WSARecv`-проба + синхронный `read()` из колбэка (ADR-2) против LT
`EPOLLIN`-readiness; запись — проактивные `WSASend` против досылки по
`EPOLLOUT`; teardown — `closesocket`-отмена pending ops против
`EPOLLRDHUP`/`EPOLLHUP`. Реально совпадает у будущих реализаций только
контракт (у connection — ~65 строк: defs + delegate + alias). «Низ» и «верх»
с нужной унитарностью не разделяются: `C_TCPConnectionIOCP` — цельный
компонент, обёртка поверх не добавляет ничего. `reactor/` — компонент той же
природы — уже живёт во фича-раскладке вне `platform/`; два конкурирующих
паттерна для однородных компонентов недопустимы.

**Отвергнуто:** оставить `platform/` домом компонентов — название врёт (не
«тонкие обёртки», а две трети кода); generic базовый класс connection с
виртуальными хуками — vtable/type-erasure против ADR-3 и бюджета размера
(ADR-9); CRTP-generic connection поверх тонких шим — требует унитарности,
которой нет (см. «Почему»).

**Уточнение (та же дата, до выполнения 2.7).** Слой `socket` ликвидирован
слиянием в `net` — `C_Socket`/`CreateSocket` это словарь сети, отдельного
umbrella слой не заслуживает (`etsl.net:socket`/`etsl.net:factory`).
`tcp` вынесен из `net` на верхний уровень: `net` — только словарь и ос-шины,
`tcp` — транспорт-фича над `net`+`reactor` (`etsl.tcp.connection`,
`etsl.tcp.acceptor`; будущие соседи — `udp` и т.п.). Публичные модули получили
префикс `etsl.` и собраны в корневом umbrella `src/etsl.cppm` — пользователь
пишет `import etsl;`. Префикс — только у публичных модулей (их список =
содержимое `etsl.cppm`); внутренний `win.wsa` и все партиции — без префикса.
Префикс введён сразу при переезде, а не «на будущее»: импорты и так правились
— один проход по всем файлам вместо двух.

### ADR-11. Жизненный цикл IOCP-операции — `inFlight` + `C_Reactor::Assign` (13.09.2026)

**Решение.** Факт «операция взведена» хранится в самой операции — `bool
inFlight` в `operation_iocp_s`. Подача любой IOCP-операции — только через
`C_ReactorIOCP::Assign(op, post)`: шаблон по типу операции
(`requires std::derived_from<op_t, operation_t>`) и по вызываемому
`post(op_t&) -> etl::expected<void, int32_t>`. Порядок: `inFlight` уже стоит →
`WSAEALREADY`; flush ровно `sizeof(WSAOVERLAPPED)`; `post(operation)`; при
успехе `inFlight = true`. `run()` снимает флаг **до** вызова `callback` — иначе
штатный перевзвод изнутри колбэка упирается в `WSAEALREADY`. `FlushOperation`
из публичного API удалён (обойти flush нельзя). Выставлять флаг после
успешного `post` корректно: цикл однопоточный (ADR-7), завершение не может быть
доставлено до возврата из `Assign`, а на пути отказа снимать нечего.

Потребители флага: `invokeReadyRead()` не взводит вторую пробу, если
`resumeReading()` уже взвёл её изнутри `onReadyRead`; защита от повторной
подачи той же `send_operation_t` (D17).

**Контракт `post`.** Возвращает `expected`, а не `bool`: обёртки
`ConnectEx`/`AcceptEx` уже возвращают `expected`, `GetExtensionFunction`
падает до подачи без `WSAGetLastError`, а `PostQueuedCompletionStatus`
отдаёт код через `GetLastError()`. Операция передаётся в `post` параметром: подаётся ровно та
операция, которую пометили, и лямбда видит настоящий тип (`send_operation_t&`,
`accept_operation_t&`) без `static_cast` вниз.

**Не в `ReactorTrait`.** `Assign` — семантика completion-модели (OVERLAPPED во
владении ядра). У epoll-реактора подачи операции нет (`epoll_ctl`, досылка по
`EPOLLOUT`, ADR-10) — трейт требовал бы фиктивный метод; концепт к тому же не
проверит шаблон от произвольного вызываемого. Как `TranslateError` и сам
`operation_t` — деталь IOCP-бекенда.

**Замеры (13.09.2026; mingw x86_64, синтетика в scratch + Windows-ВМ):**

- Размер: `WSAOVERLAPPED` 32 + `etl::delegate` 16 = 48 без хвостового
  паддинга, поэтому `bool` стоит +8: `operation_iocp_s` 48 → 56,
  `send_operation_s` 72 → 80, `accept_operation_s` 344 → 352.
- Порядок полей — **флаг последним**. Itanium ABI (GCC/clang на mingw) отдаёт
  хвостовой паддинг базы наследникам: при `transferred` (`uint32_t`) перед
  `content` `send_operation_s` = **72**, как без флага; при флаге первым
  паддинг (33–39) остаётся внутри базы и мёртв — 80 при любом порядке.
  `accept_operation_s` — 352 при любом порядке (буфер 288 байт кратен 8). У
  MSVC/clang-cl хвостовой паддинг не переиспользуется — оба порядка 80, флаг
  последним не проигрывает. Заодно флаг не граничит с концом `WSAOVERLAPPED`:
  ошибочно расширенный flush сначала заденет делегат, а не метку.
- Код: 5 мест подачи, `-Os`: 1 024 → 1 104 байт `.text` (+16 на место),
  итого порядка +120 байт; `Assign` встраивается полностью.
- Перезапись `callback` в каждом `send()`: при `-O2` — `lea` + 2 store,
  **0,16 нс** на подачу против ~3 190 нс реального цикла `WSASend` + GQCS
  (0,005 %). Убирать нельзя: `send_operation_t` не привязана к соединению —
  без перезаписи (или с `if (!is_valid())`, как в акцепторе, где пул
  собственный) завершение уйдёт прежнему соединению: чужой `pendingOps_`, UAF.

**Отвергнуто:**

- интрузивный список операций в реакторе: обходить его некому (`CancelIoEx`
  адресуется хендлом — для graceful shutdown нужен список владельцев, а не
  операций); +16 байт на операцию; синхронный отказ подачи оставляет висячий
  узел в структуре реактора; `etl::intrusive_list` без обёртки не умеет O(1)
  снятие узла по ссылке (`disconnect_link` — protected);
- предикат `Internal == STATUS_PENDING` (0 байт; проверено пробниками на ВМ):
  при немедленном завершении `WSARecv` поле сразу 0, хотя пакет ещё в очереди
  → resume из `onReadyRead` даёт 2 завершения вместо 1 и удвоение
  `onReadyRead` навсегда; после жёсткого отказа подачи поле остаётся
  `STATUS_PENDING` без пакета; `PostQueuedCompletionStatus` поле не трогает.
  (Подтверждено и обратное: пока пакет асинхронного завершения не извлечён,
  поле держит `0x103`, GQCS его перезаписывает.);
- метка в `Offset`/прочих полях OVERLAPPED — недокументированное поведение (по
  той же причине, что AFD poll в ADR-2);
- `#pragma pack(1)`: `etl::pool` округляет слот до 8 (`pool<49,8>` =
  `pool<56,8>` = 480 байт), выигрыш — 7 байт только на `readinessOperation_`;
  цена — невыровненный OVERLAPPED, в который пишет ядро (split lock), и UB
  `reinterpret_cast` в `run()`;
- `etl::delegate` вместо шаблонного `post`: не принимает захватывающие
  лямбды, косвенный вызов не встраивается.

**Инвариант и охрана.** Flush обнуляет строго `sizeof(WSAOVERLAPPED)` —
`callback` и `inFlight` обязаны его пережить. На 14.09.2026 операция —
открытая структура: флаг может писать кто угодно, а публичная база
`WSAOVERLAPPED` позволяет подать операцию в обход `Assign` (флаг останется
`false`, защита тихо не сработает). Инкапсуляция — задача 3.6.

**Фактическое состояние кода (14.09.2026).** `Assign` — статический метод
`C_ReactorIOCP` (шаблон в интерфейсе `:iocp`), все места подачи connection и
acceptor (`ConnectEx`, read-проба, `WSASend`, `AcceptEx`) идут через него.
Флаг в `operation_iocp_s` уже стоит последним; `transferred` в
`send_operation_s` по-прежнему после `content` — структура 80 байт, а не 72.
`post(operation_t&)` (алиас `task_t` удалён, `ReactorTrait` принимает
`operation_t&`) идёт **мимо** `Assign`: флаг не ставит (снятие в `run()`
безвредно), повторный `post` того же таска, пока он в очереди, не ловится.

---

## 4. Структура модулей (фактическая после 2.7, обновлено 11.09.2026)

```
src/
├── etsl.cppm                         # module etsl — umbrella: export import
│                                     # всех публичных модулей (import etsl;)
├── init/                             # инициализация подсистемы (etsl.init)
│   ├── initializer.cppm              # контракт: декларация Initialize(), без ОС
│   └── win/                          # impl-юнит: WSAStartup (без WSACleanup —
│                                     # плоский static bool, D16)
├── net/                              # словарь сети + ос-шины, без фич внутри
│   ├── net.cppm                      # etsl.net: export import :defs :ops
│   │                                 # :address :socket :factory
│   ├── net_defs.cppm                 # socket_t, INVALID_SOCKET, sockaddr-алиасы
│   ├── net_ops.cppm                  # декларации ParseIPV4, CloseSocket,
│   │                                 # Shutdown, SetNonBlocking; шаблон
│   │                                 # GetExtensionFunction<GUID> (кеш на GUID)
│   ├── net_address.cppm/.cpp         # C_Address
│   ├── net_socket.cppm               # etsl.net:socket — C_Socket (RAII)
│   ├── net_socket_factory.cppm       # etsl.net:factory — декларация CreateSocket
│   └── win/                          # impl-юниты ос-шин: net_ops_win,
│                                     # net_socket_factory_win; linux/ — Этап 4
├── tcp/                              # транспорт-фича над net+reactor
│   ├── connection/                   # контракт в корне, бекенды — в <os>/
│   │   ├── tcp_connection.cppm                 # etsl.tcp.connection — alias C_TCPConnection<T>
│   │   ├── tcp_connection_defs.cppm            # send_operation_t
│   │   ├── tcp_connection_delegate_trait.cppm  # concept TCPConnectionDelegate
│   │   └── iocp/                     # C_TCPConnectionIOCP + defs_iocp
│   └── acceptor/
│       ├── tcp_acceptor.cppm                   # etsl.tcp.acceptor — alias C_TCPAcceptor
│       ├── tcp_acceptor_delegate_trait.cppm    # concept TCPAcceptorDelegate
│       └── iocp/                     # C_TCPAcceptorIOCP + :defs_iocp
│                                     # (accept_operation_s, состояния, буфер AcceptEx)
├── reactor/
│   ├── reactor.cppm                  # etsl.reactor — alias C_Reactor
│   ├── reactor_trait.cppm            # concept ReactorTrait: initialize/run/
│   │                                 # shutdown/associate/detach/post/
│   │                                 # addTimer/removeTimer
│   └── iocp/                         # reactor_iocp*; epoll/ — Этап 4
├── timer/                            # etsl.timer, etsl.timer:types,
│                                     # etsl.timer.bucket — пользовательские
│                                     # таймеры реактора (ADR-2)
└── util/                             # noncopyable.h, etl_chrono.cpp

test/tcp_connection_iocp_test.cpp  # gtest-набор connection + acceptor (49 тестов)
examples/echo_server.cpp           # 2.5: эхо-сервер (акцептор + сессии)
examples/echo_client.cpp           # 2.5: нагрузочный клиент со сверкой потока
review/result.md                   # результаты ревью (не код)
review/next-steps.md               # план фиксов и размышления
```

Соглашения: имя файла модуля == имя модуля; `export module <domain>[.<sub>]`;
namespace `etsl`; классы с префиксом `C_`, методы `snake_case`. Ос-шины свободных
функций (`etsl.init`, `etsl.net:ops`/`:factory`, 28.08.2026) оформлены паттерном
«контракт + impl-юнит»: `.cppm` — только декларации (ноль ifdef в телах), тела —
в `<feature>/<os>/*.cpp` (`module <mod>;`), файл выбирает CMake по платформе
(`if(WIN32)`-блок); umbrella `#ifdef → export import :iocp` — только для модулей,
экспортирующих типы (tcp, reactor). Исключение (11.09.2026): шаблоны обязаны
жить в интерфейсе — `GetExtensionFunction<GUID>` (тело в `net_ops.cppm`,
нешаблонная часть `GetExtensionFunctionImpl` — в `net/win/`, не экспортируется).
Запланированные
ранее `core/error.cppm`, `socket/socket_address`, `socket/tcp_socket`,
`socket/tcp_write_request` не появились: их роли заняли `net:*`,
делегатный контракт соединения и `send_operation_t` (ADR-2/ADR-3).
Каталога `src/platform/` больше нет (2.7); новых каталогов-«слоёв» не
заводить — только фича-ветки по ADR-10. Публичный модуль — префикс `etsl.` и
строка `export import` в `src/etsl.cppm`; внутренние модули (`win.wsa`) и
партиции — без префикса. Вход пользователя: `import etsl;` (или точечно
`import etsl.net;` и т.п.).

Правило партиций (D15): **все интерфейсные партиции модуля обязаны быть прямо
или косвенно экспортированы первичным интерфейсом** ([module.unit]/3; нарушение
ill-formed, NDR — clang его не диагностирует). Реэкспорт партиции не делает её
содержимое публичным: наружу уходит только то, что помечено `export` внутри
самой партиции, поэтому внутренние состояния и константы держатся в обычном
`namespace etsl` без `export`.

Партицию, которой нечего экспортировать, **нельзя** «закрыть» переводом в
implementation-партицию (`module m:p;` без `export`): её определения перестают
быть достижимыми в instantiation context потребителя, и любой экспортированный
шаблон, чьё тело их трогает, перестаёт инстанцироваться. Проверено на
изолированном примере (12.09.2026): `:defs` как implementation-партиция,
`:iocp` с шаблоном, поле которого имеет тип из `:defs`, — сама `:iocp`
компилируется, а потребитель падает:

```
error: definition of 'state_e' must be imported from module 'm' before it is required
note: in instantiation of template class 'demo::C_Thing<int>' requested here
note: definition here is not reachable
```

Именно об этом предупреждает clang'овое
`-Wimport-implementation-partition-unit-in-interface-unit` («Names from m:defs
may not be reachable») — предупреждение про reachability, а не про стиль.
Вывод: все партиции остаются интерфейсными, а выбор есть только между
`export import` (буква стандарта) и `import` (работает за счёт interface
dependency, но формально ill-formed NDR).

---

## 5. Известные дефекты (обновлено 14.09.2026)

D1–D8 (проверено 2026-07-18) закрыты в Этапе 0; оставлены как история.
Подробности D12–D16 — `review/result.md`; план работ и размышления —
`review/next-steps.md`.

- **D9** *(закрыт 25.08.2026)*: deploy-хук уже использует ключ `~/.ssh/win-dev`
  — деплой Debug- и MinSizeRel-сборок на ВМ `win-dev` прошёл успешно.
  Закомментированная обёртка `#if(WIN32)` оставлена сознательно: хук нужен
  именно при кросс-сборке из Linux.

- **D10** *(закрыт 07.09.2026)*: gtest-набор падал на 3-м тесте
  `ConnectToListeningPeerInvokesOnConnectSuccess` — `connect(addr)` возвращал
  ошибку, после ASSERT-провала процесс завершался (код 3). Воспроизводился в
  Debug и MinSizeRel, идентично до и после реструктуризации 2.7. Причина
  подтвердилась в `net_ops` (как и предполагалось): `ParseIPV4`,
  `net_ops_win.cpp:25` — `auto sa = reinterpret_cast<os_sockaddr_in_t&>(
  storage)` (auto без `&`): вывод типа отбрасывал ссылку, `sa` был стековой
  копией — family/port/`inet_pton` писались в копию, `storage` вызывающего
  оставался нулевым, connect уходил на нулевой адрес. Фикс: `auto& sa = ...`.
  Прогон после фикса и рефакторинга контракта соединения: **35/35 PASSED**
  (07.09.2026).

- **D11** *(закрыт 11.09.2026)*: акцептор не ставил
  `SO_UPDATE_ACCEPT_CONTEXT` на принятый сокет. Теперь опция ставится в
  `C_TCPAcceptorIOCP::acceptIncoming` с дескриптором листенера до передачи
  сокета делегату; отказ — `onError`, соединение сбрасывается. На стороне
  connection `adopt()` опций контекста не ставит (с 07.09.2026).

- **D12** *(закрыт 11.09.2026, найден ревью)*: регрессия teardown акцептора —
  лямбда `fallback` в `onIncoming` проверяла `backlog_.empty()` **до**
  `destroy()` операции; после `dispose()` с взведёнными `AcceptEx` последняя
  отменённая операция не запускала `beginTeardown` → `onDisposed` не приходил
  никогда. Прогон: 10/40 (все тесты с акцептором — таймаут). Фикс — порядок
  «освободить → решить» (ADR-5); затем `onIncoming` переписан целиком.

- **D13** *(закрыт 11.09.2026, найден ревью)*: `GetExtensionFunction` с
  одним `static void* fn` на все GUID — первый запрошенный указатель
  (`AcceptEx`) возвращался и на `WSAID_CONNECTEX` → `connect()` падал, gtest
  аварийно завершался на 3-м тесте. Фикс — `template<GUID guid>` (кеш на
  специализацию, ошибка не кешируется); прогон 40/40 ×3.

- **D14** *(закрыт 12.09.2026 бампом ETL до 20.49.0)*: дефект ETL 20.48.1 —
  const-перегрузка `etl::expected<T,E>::operator*()`
  (`external/etl/include/etl/expected.h:749`) при отсутствии значения делала
  `return ETL_NULLPTR` через `const T&`; для `T = void*` — ссылка на временный
  объект (`-Wreturn-stack-address`), для прочих `T` перегрузка не
  инстанцируема. В апстриме исправлено («Fix expected::operator*() const& and
  && failing to compile»): в 20.49.0 обе перегрузки используют обычный
  `ETL_ASSERT`, `ETL_NULLPTR` остался только в `operator->`, где возврат
  указателя законен. Обход `.value()` в коде можно не снимать — он корректен и
  при исправленной версии.

- **D15** *(закрыт по существу 12.09.2026; остаток — сознательное отклонение)*:
  первичные интерфейсы не реэкспортировали свои интерфейсные партиции — по
  [module.unit]/3 ill-formed, NDR (clang молчит). Предупреждение
  `-Wimport-implementation-partition-unit-in-interface-unit` было снято раньше
  переводом `:delegate` в интерфейсные партиции, но суть оставалась. Ревизия
  всех модулей показала, что задеты **три**, а не два: `etsl.reactor`
  (`:trait`, `:iocp`, `:iocp_defs`), `etsl.tcp.connection` (`:iocp`,
  `:defs_iocp`) и `etsl.tcp.acceptor` (`:iocp`); чисты были только `etsl.net`
  и `etsl.timer`. Реэкспорт проставлен по цепочке primary → `:iocp` →
  `:iocp_defs`/`:defs_iocp`/`:delegate`/`:trait` везде, **кроме**
  `etsl.tcp.connection:defs_iocp`: там оставлен `import :defs_iocp;`
  сознательно — партиция не экспортирует ни одного имени, реэкспортировать в
  ней нечего. Практических последствий нет: партиция остаётся интерфейсной, у
  `:iocp` на неё interface dependency, поэтому определения достижимы у
  потребителя и `C_TCPConnectionIOCP` инстанцируется нормально (44/44 на ВМ).
  Остаётся формальное несоответствие [module.unit]/3 — принято осознанно.
  Альтернатива «перевести в implementation-партицию» **отвергнута проверкой**:
  она ломает инстанцирование у потребителя (см. §4, правило партиций).
  Внутренности наружу нигде не уехали: в `:iocp_defs`/`:defs_iocp` сущности
  объявлены в обычном `namespace etsl` без `export` (состояния, константы,
  `operation_iocp_s`). Единственное исключение — `accept_operation_s`,
  помеченный `export` сознательно: пользователь обязан завести под него
  `etl::pool`. *Состояние на 14.09.2026 (запись выше про `operation_iocp_s`
  устарела):* в `etsl.reactor:iocp_defs` с `export` теперь объявлены
  `operation_iocp_s` и `dispose_operation_iocp_s` (в HEAD `9b36efa` — без);
  `iocp_code_e` не экспортируется. `etsl.tcp.connection:defs_iocp`
  (состояния, `EXPLICIT_DISPOSE`/`INVALID_CACHE_VALUE`) — без `export`;
  `etsl.tcp.acceptor:defs_iocp` — по-прежнему только `accept_operation_s`.

- **D16** *(закрыт 12.09.2026)*: `static C_WSAInitializer wsa;` в
  `Initialize()` — function-local static с нетривиальным деструктором. На
  MinGW guard libstdc++ тянул EH/unwind/`__cxa_demangle`/winpthread: **+134 КБ**
  к любому бинарю (12 276 → 146 512 на синтетическом примере;
  `constexpr`-конструктор не спасал — guard нужен и для регистрации
  деструктора). Фикс — плоский `static bool` с константной инициализацией плюс
  сознательный отказ от `WSACleanup` (как в libuv; обоснование — в комментарии
  `win_initializer.cpp`). Контрольный замер того же TU теми же флагами: в
  версии до правки `__cxa_guard_acquire` ×1 и `WSACleanup` ×1, после — по нулю,
  `initialized` лежит в `.bss`. По всей `libetsl_core.a` счётчики
  `__cxa_guard_acquire`, `__cxa_atexit`, `__gxx_personality`, `_Unwind_Resume`,
  `__cxa_demangle`, `pthread_once` — нули. Цена решения: потокобезопасность
  первого вызова потеряна (в рамках ADR-7 v1 допустимо). Остаток — снять цифру
  в байтах: MinSizeRel-каталога на MinGW нет, см. 2.5.

- **D17** *(открыт 13.09.2026, найден анализом; тестом не подтверждён)*:
  `send()` (`tcp_connection_iocp.cppm:172–181`) пишет `transferred`, `content`
  и `callback` **до** проверки `inFlight` в `Assign`. Повторный `send()`
  операции, находящейся в полёте: (1) `transferred` обнулён, `content` подменён
  — `onSendOperation` досылает чужой буфер или преждевременно зовёт
  `onCommit`; (2) подменён `callback` — если второй `send()` пришёл через
  другое соединение, завершение первой подачи уходит не тому владельцу (чужой
  `pendingOps_`, UAF); (3) `WSAEALREADY` уходит в `beginTeardown` — ошибка
  пользователя рвёт живое соединение. Фикс: `if (operation.inFlight) return
  etl::unexpected(WSAEALREADY);` в `send()` до любых записей и без teardown —
  usage-отказ наравне с `WSAEINVAL`/`WSAENOTCONN` (ADR-2). Перевзвод частичной
  отправки (`onSendOperation` → `createSendOperation`) эти поля не пишет — не
  задет. Нужен тест: повторный `send()` той же операции → `WSAEALREADY`,
  соединение живо, `onCommit` первой подачи корректен.

- **D18** *(закрыт 14.09.2026 в тот же день; найден при фиксации 3.3, тестом
  не воспроизводился)*: `resumeReading()` (`tcp_connection_iocp.cppm:233–249`)
  возвращает `WSAEALREADY` при `readinessOperation_.inFlight`, **не снимая**
  `suspendReading_`. Сценарий: `suspendReading()` вне `onReadyRead`, пока проба
  в полёте (из `onCommit`, таймера) → `resumeReading()` до её завершения →
  `WSAEALREADY`, пауза остаётся → завершение пробы доставляет `onReadyRead`,
  `invokeReadyRead` видит паузу и пробу не перевзводит → чтение встало, а
  пользователь получил код «уже»; сокет молчит до повторного `resumeReading()`.
  Штатный путь (пауза из `onReadyRead` → resume после освобождения буфера) не
  задет: проба к этому моменту не в полёте. Фикс по схеме 13.09: снять
  `suspendReading_` первым; при `inFlight` вернуть успех (проба уже взведена);
  иначе взвести. Там же: (а) отказ `createReadProbeOperation()` в
  `resumeReading()` возвращается без `beginTeardown` — живое соединение
  остаётся без пробы и без уведомления (K2); (б) вне `CONNECTED` оба метода
  возвращают `WSAEISCONN`, в остальном API для этого случая — `WSAENOTCONN`.
  **Фикс (14.09.2026):** `resumeReading()` снимает `suspendReading_` до
  взвода; `WSAEALREADY` из `createReadProbeOperation()` (проба уже в полёте,
  отказ `Assign`) — успех, её завершение перевзведётся через `invokeReadyRead`;
  прочие отказы → `beginTeardown` + информационный sync-код; вне `CONNECTED` —
  `WSAENOTCONN`. Различение идёт по коду: `WSARecv` `WSAEALREADY` по
  документации не возвращает, так что сейчас это однозначно — явная проверка
  `readinessOperation_.inFlight` сделала бы связь самодокументируемой. Прогон
  44/44 (Debug, MinGW, ВМ); тестов на пути паузы по-прежнему нет (3.3).

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
  `initialize()` как function-local static. *(Вторая половина решения
  пересмотрена в D16: function-local static с нетривиальным деструктором стоил
  +134 КБ на MinGW; `C_WSAInitializer` удалён.)*

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
      `C_TCPConnectionIOCP`. **(19.07.2026; move 23.07.2026)**
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

### Этап 2 — tcp_connection (исходно tcp_socket) / tcp_server

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
      *Позже (13.08.2026) при реструктуризации переехал в
      `platform/net/net_address.cppm` (модуль `net:address`, umbrella `net`).*
- [x] **2.3** `tcp_socket`: generic IOCP completion dispatcher;
      `C_TCPConnectionIOCP` внутри `platform/net/iocp`; `ConnectEx` с wildcard
      bind; persistent read-probe; `read(span)` без `bytesAvailable`;
      caller-owned `C_TcpWriteRequest`, FIFO и proactive `WSASend` без timer/ring;
      states и безопасный `DISPOSED` после отмен. Smoke: три FIFO operations на
      двух reusable request slots, включая reentrant reuse из completion,
      loopback read и teardown; отдельный close-after-write smoke проверяет
      cancellation callbacks двух одновременно активных операций.
      MinSize executable с обоими smoke: 22 194 байта. **(23.07.2026)**
      *Дальнейшая эволюция (август 2026, фактическое состояние): делегатная
      архитектура `C_TCPSocketDriver<T>` + concept `TCPDriverDelegate` (09.08);
      оптимизации драйвера (10.08); переезд в `platform/net/tcp/` (13.08);
      переименование в `C_TCPConnection<T>`/`TCPConnectionDelegate`, модуль
      `net.tcp_connection`, тест `tcp_connection_iocp_test` (21.08) — класс
      стал полноценным соединением, имя «driver» потеряло исходный смысл.
      Generic `tcp_socket`-слой, event-enum `EV_*`, FIFO write-очередь и
      `C_TcpWriteRequest` упразднены: полу-проактивная модель I/O с
      множеством caller-owned `send_operation_t` и teardown через
      `closesocket`+`pendingOps_` (ADR-2/ADR-3/ADR-4/ADR-5).*
      *Реальный вес после эволюции (замер 20.08.2026, см. ADR-9): с динамическим
      CRT — **12 647 байт** (лучше прежних 22 194: smoke-тестов с `<iostream>`
      больше нет, `main.cpp` минимальный); со статическим CRT кросс-тулчейна —
      97 091 байт, из которых 91 366 — CRT-подложка пустого `main`, т.е. вклад
      кода etsl ≈ **5,7 КБ**. Рост библиотеки нет; разница с июлём — только
      способ линковки CRT.*
- [x] **2.4** `tcp_server`: `listen(backlog)`, пул posted `AcceptEx` (буфер
      адресов `(sizeof(sockaddr_storage)+16)*2` на accept), `onIncoming` с
      готовым соединением подключённого пира (делегатная модель, ADR-3), repost.
      `SO_EXCLUSIVEADDRUSE` вместо `SO_REUSEADDR`.
      *В работе (24.08.2026): контракт `net.tcp_acceptor` (defs + alias) и
      `C_TCPAcceptorIOCP` — пул posted `AcceptEx` на `etl::pool` (32),
      repost из completion; teardown-пути и делегатные колбэки (`onIncoming`)
      ещё не заведены (todo в коде); `main.cpp` упражняет acceptor вручную.*
      *Дополнено (28.08.2026): акцептор доведён до полной аналогии с connection
      (ADR-3/ADR-5) — делегатная модель `C_TCPAcceptorIOCP<delegate_t>` + концепт
      `TCPAcceptorDelegate` (`onIncoming(C_Socket)` — сокет по значению,
      `onDisposed()`), teardown через `pendingOps_` + `disposeOperation_`
      (`dispose()` → `beginTeardown()` → терминальный `onDisposed()` при любом
      пути сноса), `SO_EXCLUSIVEADDRUSE` перед bind, `SO_UPDATE_ACCEPT_CONTEXT`
      на принятом сокете, `FlushOperation` (memset `WSAOVERLAPPED`) перед
      взводом, очистка слота пула на путях отказа, буфер адресов переведён на
      `(sizeof(sockaddr_storage)+16)*2`; state-машина `tcp_acceptor_state_e`
      (NONE/LISTENING/DISPOSING) в партиции `:defs_iocp`. Кросс-сборка Debug
      зелёная, деплой на ВМ и короткий smoke-прогон (4 c, Debug-ассерты включены)
      без падений; `[x]` не ставится до прогона под нагрузкой (echo, 2.5) —
      gtest-покрытия акцептора пока нет.*
      *Дополнено (05.09.2026): teardown акцептора получил код причины по образцу
      ADR-5 — `beginTeardown(int32_t reason)` с кешем `cachedDisposeReason_`
      (первая причина побеждает, `EXPLICIT_DISPOSE` перебивает всегда; роль
      `pendingOps_` играет `backlog_.size()`), терминальный колбэк концепта стал
      `onDisposed(int32_t reason)`. Константы `EXPLICIT_DISPOSE`/
      `INVALID_CACHE_VALUE` продублированы в `etsl.tcp.acceptor:defs_iocp`, чтобы
      партиции connection и acceptor оставались независимыми. `listen()` теперь
      различает два отказа `armAcceptBacklog`: полный (пул пуст — синхронный
      `etl::unexpected(err)`, объект чист, колбэка нет) и частичный (часть
      `AcceptEx` в полёте — `listen()` возвращает успех, внутри стартует
      `beginTeardown(err)`, причина доезжает отложенно в `onDisposed`). Прежняя
      эвристика `state_ == NONE` в `onConnectionIncoming` удалена; заодно
      completion с ошибкой, отличной от `ERROR_OPERATION_ABORTED` (напр. RST пира
      до accept), больше не отдаёт делегату негодный сокет, а просто перевзводит
      слот.*
      *Дополнено (10.09.2026): gtest-покрытие акцептора появилось — набор
      `tcp_connection_iocp_test.cpp` переписан на связку C_TCPAcceptor +
      C_TCPConnection (все peer-стороны сценариев обслуживаются акцептором;
      +5 акцепторных тестов: `listen()` без/повторно → `WSAEINVAL`,
      повторный `initialize()` → отказ, посторонний пул backlog → отказ,
      dispose-контракт с причиной `EXPLICIT_DISPOSE` в `onDisposed`). Аргумент
      «gtest-покрытия акцептора пока нет» снят; `[x]` по-прежнему не ставится
      до прогона под нагрузкой (echo, 2.5). Замечание из прогонов: при
      `dispose()` клиента прямо в `onConnect` completion `AcceptEx` — гонка с
      каскадом сноса (может не диспатчиться вовсе), поэтому в тесте
      `ConnectToListeningPeerInvokesOnConnectSuccess` факты accept не
      проверяются — детерминированное покрытие поставки в `PeerData*`/
      `TwoDrivers`.*
      *Дополнено (11.09.2026): тесты обновлены под новый контракт акцептора —
      `onIncoming(C_Socket, const C_Address&)` (адрес пира; тесты проверяют, что
      удалённая сторона loopback = 127.0.0.1) и `onError(int32_t)`. Новое
      покрытие: дренирование backlog при teardown — каждый отменённый AcceptEx
      доходит до делегата как `onError(WSAEINVAL)` (при пуле на 4 слота — ровно
      4); `initialize()` повторный до `listen()` → `WSAEALREADY` (проверка
      state/gateway), после `listen()` → `WSAEINVAL` (раньше срабатывает guard
      непустого пула — задокументированный приоритет проверок), чужой пул →
      `WSAEINVAL`. `listen()` без initialize/повторный → `WSAEINVAL`. Прогон
      41/41 PASSED × 3 (Debug, MinGW-тулчейн).*
      *Итог дня (11.09.2026, после ревью `review/result.md`; изменения не
      закоммичены, база — `9bb1026`). Логика `onIncoming` переписана: слот
      освобождается до решения о teardown (D12), успешный перевзвод
      переиспользует операцию, отказ — `destroy`, единый хвост
      `armAcceptBacklog()` → `beginTeardown` (ADR-5). `onError` получает только
      некритичные ошибки при `LISTENING`; «не `LISTENING`» кодируется
      `WSA_OPERATION_ABORTED` и отфильтровывается — отмены при `dispose()` в
      `onError` больше **не** приходят (запись выше про `onError(WSAEINVAL)` ×4
      устарела). Адрес пира: `GetAcceptExSockaddrs` (через
      `GetExtensionFunction<WSAID_GETACCEPTEXSOCKADDRS>`) →
      `C_Address::initialize(const os_sockaddr&, uint32_t)` (размер 0 и больше
      `sockaddr_storage` отклоняются). `initialize()`: сначала состояние
      (`WSAEALREADY`), затем пул — ёмкость, размер элемента `==
      sizeof(accept_operation_t)`, **пул пуст** (иначе `WSAEINVAL`). Частичный
      отказ `armAcceptBacklog` в `listen()` больше не запускает teardown: пул
      работает деградировавшим и дозаполняется на следующих completion (запись
      05.09 в этой части устарела). Попутно: `CreateSocket` ставил
      `TCP_NODELAY = 0` (Nagle не отключался) — исправлено на 1; `ConnectEx`
      берётся через `GetExtensionFunction<WSAID_CONNECTEX>` (D13).
      Прогон на ВМ: **39/41 ×3** — падают `AcceptorListenTwiceReturnsInvalid`
      (ожидает 4 × `onError(WSAEINVAL)`, стало 0) и
      `AcceptorInitializeAfterListenFails` (ожидает `WSAEINVAL`, стало
      `WSAEALREADY`): ожидания тестов отражают прежнее поведение, требуют
      обновления. Открыто по акцептору: ~~двойной отчёт `onError(err)` +
      `onDisposed(err)`~~ (штатно, §8); ~~неинициализированные `local`/
      `remote` в `GetRemotePeerAddr`~~ (исправлено 11.09); тестов нет на `dispose()` из
      `onIncoming`, пул на 1 слот, реюз объекта после `onDisposed` (добавлены 11.09, 44/44);
      вопросы §8. `[x]` — после echo под нагрузкой (2.5).*
      *Обновлено (11.09.2026, там же): оба отмеченных теста поправлены —
      `AcceptorListenTwiceReturnsInvalid` теперь ожидает тихое дренирование
      (`acceptorErrorCount_ == 0`), `AcceptorInitializeAfterListenReturnsAlready`
      — `WSAEALREADY`. Добавлен `AcceptorDisposeFromOnIncomingDrainsBacklogAndKeepsSession`
      (закрывает пункт «тестов нет на dispose() из onIncoming»): реентерабельный
      снос из колбэка поставки — перевзвод слота тихо падает (destroy без
      `onError`), ошибка `armAcceptBacklog` из хвоста completion не перетирает
      причину `EXPLICIT_DISPOSE`, принятое до сноса соединение живёт
      независимо от акцептора (доставка пейлоада после закрытия gateway).
      Прогон на ВМ: **42/42 ×3** (Debug, MinGW-тулчейн).*
      *Дополнено (11.09.2026, тем же): закрыты последние два открытых пункта
      акцепторного покрытия. `AcceptorSingleSlotBacklogTeardownThroughLastOperation`
      — акцептор с `etl::pool<..., 1>` (отдельный делегат `SingleSlotDelegate`,
      не зависящий от фикстурного акцептора): ровно одна взведённая операция
      после `listen()` (`backlog.size() == 1`), accept и перевзвод через тот
      же единственный элемент, teardown дренирует пул до `size() == 0`,
      `onDisposed(-1)`, `onError` не вызывается. `AcceptorReuseAfterDisposedReinitializesCleanly`
      — реентерабельные `initialize()`/`listen()` прямо из `onDisposed` на
      свежем порту: сами guard'ы служат проверкой чистого NONE (gateway закрыт,
      пул пуст, кеш причины сброшен — иначе WSAEALREADY/WSAEINVAL); возрождённый
      акцептор обслуживает полную сессию (accept + пейлоад), финальный снос —
      вторая пара `onDisposed(EXPLICIT_DISPOSE)` + пустой пул. Прогон на ВМ:
      **44/44 ×3** (Debug, MinGW-тулчейн). Открытых пунктов в акцепторном
      gtest-покрытии не осталось.*
      **Закрыто 12.09.2026:** условием `[x]` был прогон под нагрузкой — он
      выполнен эхо-парой 2.5 (8×64 МиБ ×3 и 512 МиБ одним соединением, сессии
      закрываются без ошибок, см. 2.5). Оставшееся по акцептору не блокирует
      пункт: m3 и косметика (`review/next-steps.md` §1.4); D15 закрыт 12.09.2026.
- [ ] **2.5** `examples/echo_client.cpp` + `examples/echo_server.cpp`; гонка
      ≥ 64 МБ без потерь/рассинхрона; зафиксировать размер sample:
      `echo_server: ___ КБ` → установить бюджет.
      *В работе (12.09.2026): примеры написаны и прогнаны, бюджет не зафиксирован.
      Оба файла платформенно-нейтральны (только `import etsl` + ETL, ноль
      заголовков ОС — заодно проверка DoD Этапа 4). `echo_server [port] [sessions]`:
      акцептор с пулом на 16 `AcceptEx`, до 16 сессий через `adopt()`, в сессии
      4 send-слота по 16 КБ (чтение — сразу в буфер слота, эхо без копирования),
      печатает адрес пира, после `sessions` сессий штатно сносится.
      `echo_client [host] [port] [mib] [connections]`: до 8 соединений, окно
      4×16 КБ, детерминированный поток (хеш смещения) сверяется байт-в-байт,
      сторожевой таймер реактора рвёт прогон после 10 с без прогресса.
      **Прогоны на ВМ (Debug, MinGW):** 1×64 МиБ — PASS, 349 мс, 183 МиБ/с;
      8×64 МиБ — PASS ×3, ~2,9 с, ~176 МиБ/с; 1×512 МиБ — PASS, 180 МиБ/с;
      сессии сервера закрываются без ошибок. Негативные проверки: сверка с
      чужим seed → `mismatch at offset 1`; без сервера → `connect failed 10061`,
      код возврата 1.
      **Размер (MinSizeRel, флаги ADR-9, MinGW, `dec`):** `echo_server` 155 064,
      `echo_client` 154 264, `etsl.exe` 152 704 при подложке пустого `main`
      12 276 (после бампа ETL до 20.49.0 — 155 032 и 154 264, то есть −32 байта).
      Из них **+134 КБ — не код etsl**: `static C_WSAInitializer` внутри
      `Initialize()` требовал guard libstdc++, а тот тянет EH/unwind/
      `__cxa_demangle`/winpthread (замеры и варианты — `review/next-steps.md` §1.1).
      Без guard `echo_server` = 48 532, из них ~28 КБ — `printf` (mingw_pformat),
      вклад etsl + логики примера ≈ 8 КБ.
      **Цифры выше сняты до фикса D16 (12.09.2026) и более не актуальны:**
      guard устранён, что подтверждено символьно (`__cxa_guard_acquire` в
      `libetsl_core.a` — 0), но байты не перемеряны.
      **Не сделано:** примеры не подключены к `CMakeLists.txt` (проверялись
      внешним проектом через `add_subdirectory`), нет MinSizeRel-каталога на
      MinGW — без него бюджет не зафиксировать; это единственное, что осталось
      для закрытия 2.5.*
- [x] **2.6** *(добавлено постфактум)* gtest-набор `test/tcp_connection_iocp_test.cpp`
      (GoogleTest v1.17.0 FetchContent, опция `ETSL_BUILD_TESTS`; кросс-сборка,
      прогон на Windows-ВМ): 30 тестов — teardown/dispose-контракт, connect и
      реконнекты (включая реентерабельный `connect()` из `onDisconnect`),
      read-probe (в т.ч. данные пира, отправленные до завершения коннекта),
      проактивная запись (последовательная, параллельные операции с
      сохранением TCP-порядка, отмена pending send), два соединения на одном
      реакторе. Известное ограничение loopback: «RST при pending send»
      недетерминирован (WinSock буферизует отправку без flow control) — путь
      покрыт по частям dispose-отменой и abortive-close тестами. **(20.08.2026)**
      *Замер размера (ADR-9, 20.08.2026): MinSizeRel-сборка тестового exe —
      `dec` 673 367 байт (text 666 190 + data 7 177; gtest и CRT статические).
      MinSizeRel-бинарь проверен прогоном на ВМ: 30/30 PASSED (assert'ы
      отключены NDEBUG).*
- [x] **2.7** *(добавлено и выполнено 25.08.2026)* Реструктуризация по ADR-10 и
      его уточнению: `src/platform/**` ликвидирован (`net` → `src/net`,
      `tcp` → `src/tcp`, `wsa_initializer.*` → `src/reactor/iocp/`,
      `etl_chrono.cpp` → `src/util/`); `socket/` слит в `net`
      (`net_socket.cppm` = `etsl.net:socket`, `net_socket_factory.cppm` =
      `etsl.net:factory`, umbrella `socket.cppm` удалён; платформенное
      ветвление фабрики сознательно не трогалось — за владельцем); публичные
      модули переименованы в `etsl.*`, добавлен корневой umbrella
      `src/etsl.cppm`; `main.cpp`/тест — `import etsl;`. Имена партиций не
      менялись. Проверено: кросс-сборка Debug/MinSizeRel + деплой (D9
      закрыт); gtest падает на 3-м тесте в **обоих** состояниях дерева
      (до/после реструктуризации) — предсуществующий D10, не регрессия 2.7;
      размер dec 96 371 против baseline 97 091 (−720). Грабли на будущее:
      при обновлении `FILE_SET` легко выронить файл — CMake молча исключает
      его из module-графа (симптом: `module 'X' not found` на impl-юните);
      после правки пересчитывать список (сейчас 23 модуля + 6 private TU).

**DoD:** echo-пара работает под нагрузкой; контрактные модули соединения
не содержат вызовов ОС и `#ifdef` (кроме alias-umbrella); бюджет размера
установлен.

### Этап 3 — Доводка

- [ ] **3.1** Connect timeout: таймер пользователя (ADR-2) + `dispose()` —
      closesocket отменяет ConnectEx; терминальный колбэк `onConnect(error)`.
      Таймерная эмуляция readiness не используется.
- [ ] **3.2** Graceful close: `shutdown(SD_SEND)` → у пира EOF (`read()==0` →
      `onDisconnect(0)`); полузакрытие корректно обрабатывается.
- [ ] **3.3** Backpressure: high-watermark tx-буфера, пауза взвода чтения,
      пока tx не проседает (защита от медленного пира).
      *Уточнено 12.09.2026 по замерам echo (см. ADR-2 и `review/next-steps.md`
      §2.1): нужен платформенно-нейтральный `pauseRead()`/`resumeRead()` — на
      IOCP пауза = не перевзводить пробу после `onReadyRead`, на epoll — снять
      и вернуть `EPOLLIN`. Без него «подождать освобождения буфера» = холостой
      цикл на 100% ядра; вектор DoS: пир шлёт и не читает. Аналоги:
      `uv_read_stop`, `QAbstractSocket::setReadBufferSize`; в asio проблемы нет
      — следующий read инициирует пользователь.*
      *Дополнено 14.09.2026: интегрировано в `C_TCPConnectionIOCP` под именами
      `suspendReading()`/`resumeReading()` — флаг `suspendReading_` (сброс в
      `onDisposeOperation`, поэтому не переживает teardown), `invokeReadyRead`
      не перевзводит пробу при паузе или уже взведённой пробе (ADR-11). Регресса
      нет: 44/44 (Debug, MinGW, ВМ); D18 закрыт тем же днём. Открыто: тесты — пауза из
      `onReadyRead` и resume после освобождения буфера, resume из `onReadyRead`
      (ровно одна проба, без удвоения `onReadyRead`), resume при пробе в полёте;
      повтор замера ADR-2 с неотвечающим пиром (холостого цикла быть не должно);
      контракт `onReadyRead` при паузе (§8); epoll-аналог — снять/вернуть
      `EPOLLIN` (4.2).*
      *Дополнено 14.09.2026 (вечер): закрыты тесты из списка открытых —
      `SuspendResumeReadingBeforeConnectReturnsNotConn` (оба метода →
      `WSAENOTCONN`), `SuspendResumeReadingGuardCodesWhileConnected` (двойной
      suspend → `WSAEALREADY`; resume без паузы → `WSAEALREADY`; resume при
      пробе в полёте → успех без повторного взвода), `SuspendReadingFromReadyReadSuppressesDeliveryUntilResume`
      (пауза из `onReadyRead`: хвост не перевзводит пробу, данные пира молчат
      до `resumeReading()`, после — доставка), `SuspendReadingOutsideCallbackSuppressesDeliveryUntilResume`
      (suspend из таймера после доставки+перевзвода: вторая порция
      подавляется, ручной `read()` при паузе легален → `WSAEWOULDBLOCK`,
      resume доставляет). Осталось открытым: resume из `onReadyRead`.*
      *Проверено 14.09.2026 (ночь): отмеченный ранее «кандидат в дефекты»
      («suspend при пробе в полёте на тихом сокете → данные пира → доставки нет,
      после resume проба не перевзводится, stale-`inFlight`») **не
      подтвердился**. Тест `SuspendedDataDrainedByReadThenResumeRearmsProbe`:
      пауза при взведённой пробе → порция B (завершение пробы подавлено
      фильтром `invokeReadyRead`, `run()` снял `inFlight` до колбэка) →
      синхронный `read()` забирает `BBBB` → `resumeReading()` на пустом сокете
      (доставки нет — корректно) → порция C доходит через перевзведённую пробу.
      Симптом «соединение молчит» давал сам синхронный `read()`: он опустошал
      сокет, и новой пробе было не о чем сигналить (level-triggered: данные,
      прочитанные вручную на паузе, после resume не уведомляются); падение
      assert деструктора — от таймаута фикстуры с висящей пробой без
      `dispose()`, а не от библиотеки. Прогон **49/49 ×3** (Debug, MinGW, ВМ).*
      *Состояние на 14.09.2026 (итог; перекрывает списки «Открыто» выше):*
      *`invokeReadyRead` на паузе подавляет доставку целиком — не зовёт
      `onReadyRead` и не перевзводит пробу; вопрос §8 «доставлять или глушить»
      решён в пользу «глушить», контракт — ADR-2. Покрыто тестами: guard-коды,
      пауза из `onReadyRead`, suspend вне колбэка, resume при пробе в полёте,
      данные на паузе, прочитанные вручную. **Открыто:** тест resume изнутри
      `onReadyRead` (ровно одна проба, без удвоения `onReadyRead`); повтор
      замера ADR-2 с неотвечающим пиром — примеры `echo_server`/`echo_client`
      паузу пока не используют; high-watermark/политика паузы в примерах;
      epoll-аналог — снять/вернуть `EPOLLIN` (4.2).*
- [ ] **3.4** `to_portable()` для частых кодов: `WSAEWOULDBLOCK`,
      `WSAECONNREFUSED`, `WSAECONNRESET`, `WSAECONNABORTED`, `WSAETIMEDOUT`,
      `WSAEHOSTUNREACH`.
- [ ] **3.5** DNS: решение — v1: блокирующий `getaddrinfo` до вызова connect;
      позже `GetAddrInfoEx` overlapped. Зафиксировать выбор здесь.
- [ ] **3.6** *(добавлено 14.09.2026)* Операция IOCP — тип с инвариантом
      (ADR-11). `operation_iocp_s` перерос «просто структуру»: у него есть
      состояние и порядок переходов, которые сейчас никто не охраняет.
      Целевой вид: приватная база `WSAOVERLAPPED` (подача в обход `Assign` —
      ошибка компиляции); `inFlight_` приватный + `inFlight() const`;
      приватные `flush()` и `complete(bytes, error)` (снять флаг → колбэк —
      порядок живёт в одном месте); `friend class C_ReactorIOCP`
      (предварительное объявление в `:iocp_defs`; дружба между партициями
      одного модуля с clang-модулями не проверена). `Assign` отдаёт в `post`
      операцию и `WSAOVERLAPPED&` — единственный источник указателя для
      `ConnectEx`/`AcceptEx`/`WSARecv`/`WSASend`; в `run()` — `static_cast` +
      `complete()` вместо `reinterpret_cast`; `post(task)` — через `Assign`.
      Раскладка: флаг последним — **сделано** (14.09.2026); осталось
      `transferred` перед `content` (сейчас `send_operation_s` 80 байт) и
      `static_assert(sizeof(send_operation_s) == 72)`. `callback` пока
      публичный (выставляют владельцы). Решить: суффикс `_s` при приватных
      членах (соглашение §4: классы — `C_`) — переименование задевает
      публичный `send_operation_s`. В той же задаче — фикс D17 и тесты
      повторной подачи.

**DoD:** тесты на timeout/RST/backpressure/graceful close зелёные.

### Этап 4 — Linux (epoll)

**Цель:** те же examples собираются и работают на Linux **без изменений** —
проверка унитарности интерфейсов.

- [ ] **4.1** `reactor/epoll/reactor_epoll.*`: `epoll_create1`, LT-режим
      и `eventfd` для `post()/shutdown()`; reactor остаётся generic
      demultiplexer без знания TCP операций.
- [ ] **4.2** `tcp/connection/epoll/*`: `EPOLLIN` →
      `onReadyRead` (синхронный `read()` в колбэке); active write продолжать
      по `EPOLLOUT`, отключая interest после терминального `onCommit`;
      тот же `TCPConnectionDelegate`-контракт, что у IOCP-реализации.
- [ ] **4.3** Linux-тела ос-шин: `src/init/linux/` (Initialize → true) и
      `src/net/linux/` (`CloseSocket` → `::close`, berkeley-тела `ParseIPV4`/
      `CreateSocket`, ошибки — `errno`); включение — else-веткой `if(WIN32)`-блока
      CMakeLists (паттерн «контракт + impl-юнит», см. §4).
- [ ] **4.4** CI/пресет Linux-сборки; прогон gtest-набора + echo на обеих ОС.

**DoD:** user-код examples идентичен на обеих платформах; отличия — только
внутри `<feature>/<os>/`-партиций (ADR-10).

---

## 7. Правила разработки (для человека и ИИ)

1. **Никаких новых зависимостей.** Только WinAPI/Winsock2 и ETL.
2. **Запрещено в коде библиотеки:** исключения, RTTI, `new/delete` напрямую,
   `std::function`, `std::error_code`, `iostream`, `printf`-логирование.
3. `noexcept` по умолчанию на всех публичных функциях; ошибки — только через
   `etl::expected` (ADR-6).
4. ОС-специфичный код — только в `<feature>/<os>/` (`reactor/iocp/`,
   `tcp/connection/iocp/`, `init/win/`, `net/win/`, …). Бекенды с типами —
   партиции с alias-umbrella по `_WIN32`/`__linux__`; ос-шины свободных функций —
   паттерн «контракт + impl-юнит» (декларации в `.cppm`, тела в `<os>/*.cpp`,
   выбор файла — `if(WIN32)`-блоком CMake; исключение — `etsl.net:defs`,
   ветвление в global module fragment). Контрактные модули (defs,
   delegate-концепты) не содержат вызовов ОС. Пользовательский делегат
   платформенно-нейтрален (ADR-3, ADR-10).
5. Read-probe — ровно один pending OVERLAPPED на сокет; send-операций может
   быть несколько, каждая со своим caller-owned OVERLAPPED (ADR-2/ADR-4);
   лайфтайм по ADR-5. Подача любой IOCP-операции — только через
   `C_Reactor::Assign` (ADR-11).
6. Каждый новый модуль — в `FILE_SET CXX_MODULES` в `CMakeLists.txt`; имя файла
   == имя модуля. Публичный — префикс `etsl.` и строка в `src/etsl.cppm`;
   внутренние модули и партиции — без префикса.
7. После каждого этапа — прогон `size-report`, сравнение с baseline/budget.
8. Минимальные изменения: задача не должна тащить рефакторинг соседнего кода.
9. Язык проекта: RU/ENG — комментарии и документация допустимы на обоих языках.
10. После изменений reactor/tcp connection — кросс-сборка и прогон gtest-набора на
    Windows-ВМ (см. §2); изменение публичного контракта соединения сопровождать
    правкой тестов. Код тестов править можно, код библиотеки — только по задаче.

---

## 8. Открытые вопросы (отложенные решения)

- **EV_WRITE наружу:** вопрос снят (20.08.2026) — запись полностью проактивная
  (`send` + терминальный `onCommit`, см. ADR-2), трансляция writability наружу
  не нужна; эмуляция readiness таймерами отвергнута.
- **DNS:** блокирующий `getaddrinfo` vs overlapped `GetAddrInfoEx` — решение
  фиксируется в задаче 3.5.
- ~~**`onReadyRead` после `suspendReading()`**~~ — *решено 14.09.2026:*
  глушить. Завершение пробы, взведённой до паузы, подавляется в
  `invokeReadyRead` (делегат не зовётся, проба не перевзводится);
  `resumeReading()` взводит пробу заново, и данные, пришедшие на паузе,
  доставляются сразу. `CancelIoEx` не используется (см. ADR-2). Для Этапа 4:
  событие, уже полученное из `epoll_wait` до `EPOLL_CTL_MOD`, epoll-бекенд
  обязан фильтровать так же — контракт одинаков на обеих ОС (ADR-10).
- **UDP:** вне v1; `socket_factory` не должен её блокировать (см. D3).
- **TLS:** out of scope (отдельный проект/слой поверх).
- **Мультипоточный `run()`** (несколько потоков на один IOCP): конструкция
  зарезервирована (ADR-7), не реализуется в v1. Кеши extension-функций
  (`static` в `GetExtensionFunction<GUID>`) не синхронизированы — при
  мультипоточном `run()` пересмотреть.
- ~~**Хранилище backlog акцептора и epoll**~~ — *решено 11.09.2026:* пул
  остаётся внешним — `C_TCPAcceptor(reactor, etl::ipool& backlog, delegate)`.
  Библиотека не диктует источник памяти: `etl::ipool` покрывает и
  статический `etl::pool<T, N>`, и `etl::pool_ext`/`generic_pool_ext` над
  буфером из кучи (ADR-4). **Отвергнуто:** параметр шаблона
  `C_TCPAcceptor<D, N>` со встроенным пулом — принуждает к памяти внутри
  объекта. Следствие для Этапа 4: epoll-бекенд обязан принимать ту же
  сигнатуру конструктора (ADR-10 — код пользователя одинаков); что epoll
  делает с пулом (игнорирует / берёт `capacity()` как лимит `accept4` за
  пробуждение) — решить в 4.x.
- **Ошибки accept на epoll:** по accept(2) сетевые ошибки нового соединения
  (`ECONNABORTED`, `EPROTO`, `ENETDOWN`, `EHOSTUNREACH`…) лечатся повтором как
  `EAGAIN`; `EMFILE`/`ENFILE`/`ENOBUFS` при LT-epoll дают горячий цикл —
  нужна защита (резервный fd / снятие `EPOLLIN`). На IOCP аналог — устойчивая
  ошибка листенера при перевзводе: сейчас сигнал уходит в `onError`, решение
  за делегатом; защиты на уровне библиотеки нет.
- **Владение сокетом в `TCPAcceptorDelegate`:** концепт пропускает
  `onIncoming(const C_Socket&, …)` — сокет тогда молча закрывается при
  перевзводе. Отрицательное требование (запрет lvalue-вызова) рассмотрено и
  пока не принято.
- ~~**Один канал отчёта при опустевшем пуле**~~ — *решено 11.09.2026:*
  `onError(err)` + затем `onDisposed(err)` — штатно, это два разных события:
  некритичный отказ перевзвода слота, а следом — критичное опустение пула и
  снос (см. ADR-3).
