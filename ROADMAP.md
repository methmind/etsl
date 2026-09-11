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
- ETL 20.48.1 (`external/etl`, submodule; обновлён с 20.47.1 и застейджен
  28.08.2026 — бамп уменьшил MinSizeRel-dec на 1 472 байта, см. ADR-9).
  Исключения внутри ETL выключены по умолчанию (`ETL_THROW_EXCEPTIONS`
  не определён, `external/etl/include/etl/platform.h:287`)
  — дополнительных макросов для ETL не нужно; нужно лишь компилировать сам проект
  без исключений.
- CMake ≥ 4.2, `CMAKE_CXX_SCAN_FOR_MODULES ON`. Модули перечислены в
  `FILE_SET CXX_MODULES` в `CMakeLists.txt` — **каждый новый модуль добавлять туда**.
- **Тесты:** GoogleTest v1.17.0 (FetchContent), опция `ETSL_BUILD_TESTS`; набор
  `test/tcp_connection_iocp_test.cpp` — 41 тест, Windows/IOCP-only (на
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
  — формальное закрытие фазы №3, см. 2.4. 11.09.2026: 41 тест (+покрытие
  адреса пира); после финальной правки акцептора и обновления ожиданий двух
  акцепторных тестов — **41/41 ×3**, см. 2.4.)*
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
  семантика, пейсинг через async completion, busy loop невозможен.
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
// Реактор не знает тип операции.
struct operation_iocp_s : WSAOVERLAPPED {
    etl::delegate<void(operation_iocp_s& operation, uint32_t bytes, int32_t error)> callback;
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
  `onReadyRead` либо `onDisconnect`, в т.ч. `onDisconnect(err)` при неудаче
  взвода read-пробы внутри `adopt` (07.09.2026);
- `onReadyRead()` — данные доступны; пользователь зовёт `read(span)`;
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
  причина (пул опустел).

Валидный callback одновременно обозначает in-flight operation; reactor очищает
его перед вызовом, отдельного `pending` flag нет. Виртуальных иерархий и heap
type-erasure нет.

### ADR-4. Владение памятью — библиотека не аллоцирует

- Event loop (reactor) — value type на стеке: `C_Reactor loop; loop.initialize(); loop.run();`
  **Отвергнуто:** `etl::pool` + `createLoop()` + `unique_ptr` (машинерия без
  выигрыша — цикл создаётся один раз).
- IOCP operation (содержит `WSAOVERLAPPED`) — член platform connection; reactor
  хранит указатель только до completion.
- Write payload и `send_operation_t` принадлежат пользователю и живут до
  единственного терминального `onCommit`; переиспользование или разрушение
  контекста до завершения запрещено (соединение пишет в его `WSAOVERLAPPED`).
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

---

## 4. Структура модулей (фактическая после 2.7, обновлено 11.09.2026)

```
src/
├── etsl.cppm                         # module etsl — umbrella: export import
│                                     # всех публичных модулей (import etsl;)
├── init/                             # инициализация подсистемы (etsl.init)
│   ├── initializer.cppm              # контракт: декларация Initialize(), без ОС
│   └── win/                          # impl-юнит: WSAStartup/WSACleanup
│                                     # (C_WSAInitializer — module-local)
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
│       ├── tcp_acceptor_defs.cppm              # :defs — пока пуст
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

test/tcp_connection_iocp_test.cpp  # gtest-набор connection + acceptor (41 тест)
review/result.md                   # результаты ревью (не код)
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

---

## 5. Известные дефекты (обновлено 11.09.2026)

D1–D8 (проверено 2026-07-18) закрыты в Этапе 0; оставлены как история.
Подробности D12–D15 — `review/result.md`.

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

- **D14** *(обойдён, 11.09.2026)*: дефект ETL 20.48.1 — const-перегрузка
  `etl::expected<T,E>::operator*()` (`external/etl/include/etl/expected.h:749`)
  при отсутствии значения делает `return ETL_NULLPTR` через `const T&`; для
  `T = void*` — ссылка на временный объект (`-Wreturn-stack-address`), для
  прочих `T` перегрузка не инстанцируема. Обход: не разыменовывать
  `const expected` через `*` — брать `.value()` (у `value() const&` такой ветки
  нет) или не объявлять результат `const`. Кандидат в апстрим.

- **D15** *(открыт, 11.09.2026)*: модульная структура tcp-фич (и connection, и
  acceptor): `:delegate` — implementation-партиция, импортируемая в
  интерфейсную `:iocp` (clang:
  `-Wimport-implementation-partition-unit-in-interface-unit` на
  `tcp_acceptor_iocp.cppm:18`); интерфейсные партиции `:iocp`/`:defs_iocp`
  primary interface не реэкспортирует (`import :iocp;` без `export`) — по
  [module.unit]/3 ill-formed, NDR. Clang пока собирает; чинить в обоих модулях
  разом.

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
- [ ] **2.4** `tcp_server`: `listen(backlog)`, пул posted `AcceptEx` (буфер
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
      обновления. Открыто по акцептору: двойной отчёт `onError(err)` +
      `onDisposed(err)` при опустевшем пуле; неинициализированные `local`/
      `remote` в `GetRemotePeerAddr`; тестов нет на `dispose()` из
      `onIncoming`, пул на 1 слот, реюз объекта после `onDisposed`;
      D15; вопросы §8. `[x]` — после echo под нагрузкой (2.5).*
      *Обновлено (11.09.2026, там же): оба отмеченных теста поправлены —
      `AcceptorListenTwiceReturnsInvalid` теперь ожидает тихое дренирование
      (`acceptorErrorCount_ == 0`), `AcceptorInitializeAfterListenReturnsAlready`
      — `WSAEALREADY`. Прогон на ВМ: **41/41 ×3** (Debug, MinGW-тулчейн).*
- [ ] **2.5** `examples/echo_client.cpp` + `examples/echo_server.cpp`; гонка
      ≥ 64 МБ без потерь/рассинхрона; зафиксировать размер sample:
      `echo_server: ___ КБ` → установить бюджет.
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
- [ ] **3.4** `to_portable()` для частых кодов: `WSAEWOULDBLOCK`,
      `WSAECONNREFUSED`, `WSAECONNRESET`, `WSAECONNABORTED`, `WSAETIMEDOUT`,
      `WSAEHOSTUNREACH`.
- [ ] **3.5** DNS: решение — v1: блокирующий `getaddrinfo` до вызова connect;
      позже `GetAddrInfoEx` overlapped. Зафиксировать выбор здесь.

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
   лайфтайм по ADR-5.
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
- **UDP:** вне v1; `socket_factory` не должен её блокировать (см. D3).
- **TLS:** out of scope (отдельный проект/слой поверх).
- **Мультипоточный `run()`** (несколько потоков на один IOCP): конструкция
  зарезервирована (ADR-7), не реализуется в v1. Кеши extension-функций
  (`static` в `GetExtensionFunction<GUID>`) не синхронизированы — при
  мультипоточном `run()` пересмотреть.
- **Хранилище backlog акцептора и epoll (11.09.2026):** конструктор
  `C_TCPAcceptorIOCP(reactor, etl::ipool& backlog, delegate)` — IOCP-деталь:
  на epoll операций нет (`EPOLLIN` на листенере + `accept4` в цикле до
  `EAGAIN`), пул не нужен, пользовательский код разойдётся (против ADR-10).
  Вариант: параметр шаблона `C_TCPAcceptor<D, N>` — на IOCP встроенный
  `etl::pool<accept_operation_s, N>` (без кучи, ADR-4), на epoll — лимит
  `accept4` за пробуждение. Решить до Этапа 4.
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
- **Один канал отчёта при опустевшем пуле:** сейчас `onError(err)` и затем
  `onDisposed(err)`; альтернатива — сообщать ошибку перевзвода после
  `armAcceptBacklog()` и только при `LISTENING`.
