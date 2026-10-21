# Bloodborne: первый эксперимент нативного запуска

## Частота кадров, звук, диагностика (2026-09-25)

- **FPS**: `BB_FPS=uncap` (по умолчанию) применяет патч «Uncap FPS++»
  (Lance McDonald, Kyo) из `patches/Bloodborne.xml`: интервал флипа 1 и
  шаги симуляции от реального дельтатайма кадра. Vblank при этом следует за
  частотой монитора (`BB_VBLANK_HZ=0`). `BB_FPS=60`/`90` — патчи с
  фиксированным шагом, `BB_FPS=30` — без патчей. Дополнительные патчи из XML:
  `BB_PATCHES="Skip Intro;Disable Motion Blur (perf increase)"`.
  `patches.py` компилирует их в `out/patches.bin`; адреса в XML — это
  виртуальные адреса PS4 (база eboot 0x400000), загрузчик пишет их по
  смещению `адрес-0x400000` после релокаций (`--patches`).
- **Звук**: `sceAudioOutOutput` теперь возвращает управление строго раз в
  период буфера (как железо). Раньше вызовы шли пачками раз в квант PipeWire
  (21 мс), поток вывода FMOD обгонял микшер и проигрывал незаписанные половины
  512-кадровых блоков — щелчок каждые 10.7 мс.
- **Производительность потока GPU** (`shadPS4:GpuCommandProcessor` — узкое
  место: ~97% одного ядра). Замер в одной сцене: 26.5 → 47 FPS.
  - `runtime_memory.c`: таблица регионов под rwlock вместо рекурсивного
    мьютекса (18% времени поток GPU ждал его), плюс потоковый кеш регионов,
    проверяемый по счётчику поколений таблицы, — запросы видеоядра на каждый
    draw идут без блокировки.
  - `vk_scheduler.cpp`: `PopPendingOperations` на каждом draw делал ioctl
    опроса timeline-семафора; теперь только при непустой очереди и не чаще
    раза в 250 мкс.
  - `fetch_shader.cpp`: разбор fetch-шейдера кешируется по адресу кода с
    проверкой копии кода.
  - `region_manager.h`: без изменённых страниц в диапазоне защита не
    пересчитывается.
  - Запись команд Vulkan в отдельном потоке `bb:VkRecorder`
    (`Scheduler::Record`, команды и их данные в блоках по 128 КиБ; draw,
    dispatch, дескрипторы, барьеры, копирования, dynamic state, детайлинг).
    `CommandBuffer()` для непереведённого кода синхронизируется и включает
    прямую запись до конца draw. `BB_VK_RECORD_THREAD=0` — старое поведение.
  - Кеш описаний текстур (`ImageDesc`) по T# и флагам ресурса, кеш
    `FindImage`, проверка изменённых страниц без блокировки для привязок
    только на чтение, загрузка гостевых данных буферов в потоке записи.
  - Замер в одной сцене: все оптимизации 64 FPS, без них 40 FPS (×1.6).
  - `BB_TOGGLE_FILE=<файл>`: маска выключенных оптимизаций, меняется во время
    игры (1 кеш регионов, 2 кеш fetch-шейдеров, 4 ранний выход трекинга
    страниц, 8 лимит опроса драйвера, 16 поток записи, 32 кеш текстур,
    64 проверка страниц без блокировки, 128 кеш FindImage, 256 загрузка
    буферов в потоке записи, 512 мемо трекера барьеров) — для поиска
    регрессий без перезапуска.
- **Звук**: при старте для Bloodborne выставляется флаг `0x204E` в
  `SPRJ0005/userdata0010` (хак rainvmaker из форка Diegolix29): с нулевым
  флагом часть звуков, например оружие игрока, не проигрывается.
  `BB_SOUND_HACK=0` отключает.
  Профилирование без perf: загрузчик по SIGUSR2 печатает RIP, `rdi` (адрес
  мьютекса при ожидании futex) и цепочку кадров потока; GPU-библиотека
  собирается с `-fno-omit-frame-pointer`.
- **Диагностика**: `BB_FRAME_STATS=1` — FPS, худший кадр и время компиляции
  шейдеров/пайплайнов раз в 5 с; `BB_AUDIO_STATS=1` — ритм и заполнение
  очереди звука; `BB_AUDIO_DUMP=путь` — сырой PCM каждого порта.

## Текущее состояние (2026-09-25)

Оригинальный x86-64 код `eboot.bin` выполняется напрямую на Linux и проходит
глобальные конструкторы, инициализацию памяти и запуск собственной системы
потоков игры: создано 13 гостевых потоков (`FD4JobWorker:0`, `EzWorkPool_*` ×9,
`MOMainThread`, логгер). Игра выделяет ~4.8 ГиБ direct- и ~337 МиБ
flexible-памяти. Текущая остановка — `sceAjmInitialize` (аудиокодек ATRAC9),
caller `0xd18a6a`.

Изменения этого этапа:

- `runtime_memory.c` переписан: единый sparse memfd на весь direct-пул
  (5056 МиБ, как `sceKernelGetDirectMemorySize` на PS4), таблица регионов
  с разбиением, flexible memory (448 МиБ), `ReserveVirtualRange`, `Mprotect`,
  `QueryMemoryProtection`, `VirtualQuery`, `GetDirectMemoryType`, `BatchMap(2)`,
  частичные unmap/release, `MAP_FIXED`/`NO_OVERWRITE`, GPU-биты защиты.
- **Все видимые гостю адреса размещаются ниже 1 ТиБ**: образ, стеки потоков,
  главный стек, трамплины (`runtime_low_map`), гостевая память
  (`0x10_0000_0000..0xfc_0000_0000`), кучи host-объектов (non-PIE + `mallopt`).
  Игра упаковывает указатели в 40 бит: например, lock по адресу `0x20368c0`
  хранит владельца как `scePthreadSelf() & 0xffffffffff` и сравнивает с полным
  значением. С хэндлами `0x7f…` это давало вечную взаимоблокировку.
- Watchdog (`--timeout`) печатает RIP и цепочку кадров всех потоков
  (смещения относительно образа и `bb-probe` для `addr2line`).
  `BB_TRACE_SEMA=1` печатает блокирующие ожидания семафоров.
  `got_import.py <адрес GOT>` называет импорт за PLT-заглушкой.
- Сеть в режиме «кабель отключён»: HTTP-шаблоны/соединения/запросы создаются,
  отправка возвращает сетевую ошибку; NpWebApi/Matching2/Signaling/Score —
  «не в PSN»; голосовой чат — порты без звука.

Следующие подсистемы по порядку появления: Ajm (ATRAC9 через ffmpeg),
Fios2, SaveData, Rtc, Pad, AudioOut, AvPlayer, VideoOut и GnmDriver.

## Предыдущее описание


**Оригинальный x86-64 код `eboot.bin` выполняется непосредственно процессором
Linux и проходит начальный участок конструкторов.** Теперь он возвращается из
`_init_env`, регистрирует три обработчика `atexit` и 279 C++-деструкторов,
проходит 1416 guard инициализации, создаёт 1422 mutex, один rwlock и 17 семафоров,
выделяет/отображает 116 МиБ для игрового аллокатора.
Оригинальная `libc.prx` загружается вместе с eboot, её инициализатор возвращает 0,
конструктор `std::ios_base::Init` пройден. Следующая контролируемая остановка —
`sceSysmoduleLoadModule(0xb4)` (`g8cM39EUZ6o#M#N`, caller `0x2026149`).
Отдельный тест Vulkan выполняет команду заполнения буфера и проверяет
возвращённые 4096 байт. shadPS4 в этом эксперименте не запускается.

**Это ещё не рекомпиляция всей игры и не играбельный порт.** Нет окна игры,
меню, отрисовки игровых ресурсов или работающего игрового цикла. Vulkan пока
не соединён с графическими командами игры. Контролируемая остановка на очередном
неподдерживаемом импорте — измеримая граница прототипа; запуск игры не завершён.

## Запуск в текущем окружении

Из `game_files`:

```bash
bash native_probe/run.sh --software
```

Скрипт находит локальные Python, GCC и Vulkan SDK, подготавливает образ,
собирает загрузчик и запускает его. На этой машине инструменты найдены
в `/nix/store`; ничего скачивать или устанавливать не понадобилось.
`--software` выбирает Lavapipe и отключает неявные Vulkan layers для теста.
В проверенном окружении `/dev/dri` отсутствует, поэтому реальный GPU не проверен.

Ожидаемый результат:

```text
Vulkan: llvmpipe (LLVM 21.1.8, 256 bits); command submission + 4096-byte readback PASS
Mapped 91973152 bytes, 4 segments; applied 237083 relocations
Starting native libc.prx at image offset 0x56e0000; fallback exports=202
Runtime: application heap API registered
Native libc module initializer returned 0
Entering original x86-64 code at guest offset 0xa0
Runtime: _init_env returned (verified libc implementation: RET)
Runtime: direct allocation offset=0x0, size=121634816 bytes
Runtime: mapped direct memory, size=121634816, alignment=0x200000
Runtime: semaphore id=1, attr=2, initial=0, max=1
Runtime: semaphore id=2, attr=2, initial=0, max=1
Runtime: semaphore id=3, attr=2, initial=0, max=1
Runtime: semaphore id=4, attr=2, initial=0, max=1
Runtime: semaphore id=5, attr=2, initial=0, max=1
Runtime: semaphore id=6, attr=2, initial=0, max=1
Runtime: semaphore id=7, attr=2, initial=0, max=1
Runtime: semaphore id=8, attr=2, initial=0, max=1
Runtime: semaphore id=9, attr=2, initial=0, max=1
Runtime: semaphore id=10, attr=2, initial=0, max=1
Runtime: semaphore id=11, attr=2, initial=0, max=1
Runtime: semaphore id=12, attr=2, initial=0, max=1
Runtime: semaphore id=13, attr=2, initial=0, max=1
Runtime: semaphore id=14, attr=2, initial=0, max=1
Runtime: semaphore id=15, attr=2, initial=0, max=1
Runtime: semaphore id=16, attr=2, initial=0, max=1
Runtime: semaphore id=17, attr=2, initial=0, max=1
STOP: first unsupported PS4 import: g8cM39EUZ6o#M#N (index 249)
API: sceSysmoduleLoadModule
Caller return offset: 0x2026149; first argument: 0xb4
Runtime: _init_env=1, atexit=3, __cxa_atexit=279, registered handlers=282
Runtime: static guards acquired=1416, released=1416
Runtime: mutexes created=1422, locks=3208, unlocks=3208
Runtime: rwlocks created=1, reads=21, writes=1, unlocks=22
Runtime: semaphores created=17, deleted=0, acquired=0, signals=0, timeouts=0
Runtime: direct memory allocations=1, maps=1, live=121634816, budget=536870912
Runtime: memory operations=9884
Original guest entry instructions executed; game initialization is incomplete.
```

**Код завершения 20 означает ожидаемую остановку на отсутствующей реализации
PS4 API.** Он намеренно отличается от 0. Ошибки загрузки возвращают 1;
явно неподдерживаемые режимы runtime — 21, срабатывание stack protector — 22.
Linux-сигналы — 128 + номер сигнала. На выполнение гостевого кода в Linux
установлен таймер 10 секунд.

На системе с доступным GPU можно попробовать `bash native_probe/run.sh`.
Это использует обычный выбор устройства Vulkan; аппаратный результат здесь
не проверен. Для повторного запуска уже собранных файлов:

```bash
native_probe/out/bb-probe native_probe/out/boot-libc.bin --cpu-only
native_probe/out/bb-probe --vulkan-only
```

Базовый образ без нативной libc сохраняется в `out/boot.bin` для сравнения
с прежней остановкой на `std::ios_base::Init`.

Из каталога `native_probe` команда остаётся прежней: `bash run.sh --software`.
Для воспроизведения самой ранней остановки на `_init_env` используйте
`out/bb-probe out/boot.bin --cpu-only --strict-imports` из этого каталога.

Последняя команда использует обычные настройки Vulkan. Для программного
драйвера задайте `VK_DRIVER_FILES` на его JSON и
`VK_LOADER_LAYERS_DISABLE='~implicit~'`, как делает `run.sh --software`.

## Что найдено в файлах

| Параметр | Результат |
|---|---|
| Игра | Bloodborne, CUSA03173 |
| Версия из `param.sfo` | APP_VER 01.09, VERSION 01.00 |
| Контейнер | SELF, 8 записей сегментов |
| CPU | x86-64, ELF64 little-endian, PS4/FreeBSD ABI |
| Тип ELF | ET_SCE_DYNEXEC (`0xfe10`) |
| Точка входа | `0xa0` относительно базы загрузки |
| Загружаемые сегменты | 2: код/данные только для чтения и изменяемые данные/BSS |
| Образ в памяти | 91 042 036 байт |
| Импорты | 686 уникальных импортированных символов |
| Зависимости DT_NEEDED | 42 модуля |
| Перемещения адресов | 231 478 RELATIVE, 2 838 ABS64, 23 GLOB_DAT, 661 JUMP_SLOT |
| Ресурсы `dvdroot_ps4` | 31 322 968 005 байт |

Среди зависимостей: libc, libkernel, GnmDriver, VideoOut, Pad, Fios2,
AudioOut, SaveData, Sysmodule, сетевые и диалоговые API. В `sce_module`
имеются только семь файлов; наличие файлов модулей не заменяет поддержку их ABI.

Используемые сегменты SELF доступны открытым текстом. Извлечение не выполняет
расшифровку. Неподдерживаемые сжатые/зашифрованные сегменты отклоняются.
Один незагружаемый сегмент метаданных (`program header 9`, тип `0x6fffff01`)
не представлен в доступных SELF-сегментах. В диагностическом `eboot.elf` его
диапазон заполнен нулями: **это не побайтово полное восстановление ELF**.
Все сегменты, нужные этому эксперименту, проверены на наличие.

Подробности, SHA-256 исходного eboot, список зависимостей, NID, сегментов
и количество ресурсов по каталогам находятся в `out/analysis.json`.
Имена нескольких импортов проверены вычислением NID из предполагаемых имён;
результаты записаны в `import_name_hints`. Для первого вызова `_init_env`
получен NID `bzQExy189ZI`, следующий вызов соответствует `atexit`.

## Как устроен прототип

`prepare.py` читает SELF и SFO, проверяет границы, восстанавливает доступные
сегменты, разбирает PS4 dynamic/symbol/RELA-таблицы и формирует `out/boot.bin`.
Исходные файлы открываются только для чтения. Все результаты — в `out/`.

Дополнительно проверяется фактический экспорт `_init_env` в `sce_module/libc.prx`:
NID `bzQExy189ZI`, адрес `0x5fef0`, размер 1, байт `c3` (RET).
SHA-256 libc и доказательство записываются в `analysis.json/libc_evidence`.
Первоначальное предположение о сложной инициализации внутри `_init_env`
для этого дампа не подтвердилось. Возврат соответствует реальному коду функции.
Формат `BBPROBE2` содержит флаг этой проверки; без флага runtime отключён.
Старые образы `BBPROBE1` читаются с отключённым runtime.

`link_libc.py` создаёт отдельный `out/boot-libc.bin` формата `BBPROBE3`:
добавляет два сегмента оригинальной libc, её перемещения адресов и 202
резервные привязки экспортов. Сопоставляет NID вместе с именами и версиями
библиотеки/модуля: локальные суффиксы libc `#C#A` и eboot `#q#q` различаются.
Реализованные host-контракты имеют приоритет. Остальные совпавшие импорты
ссылаются на настоящий код/данные libc, включая объекты и таблицы C++.
Исходные файлы игры не изменяются; сведения о привязках и SHA-256 libc
записываются в `out/libc-link.json`.

Перед входом в eboot выполняется `DT_INIT` libc. Поддержан TLS этой библиотеки:
модуль 2, шаблон 544 байта, блок 1184 байта на host-поток, обнулённый остаток;
14 перемещений `DTPMOD64` записывают номер модуля, а не указатель на образ.
`__tls_get_addr` проверяет модуль и границы. Полный TLS всех модулей PS4,
его освобождение при завершении гостевых потоков и прямой доступ через FS
не реализованы. Для текущего одного гостевого потока блок живёт до выхода.
`sceKernelGetProcParam` возвращает исходный перемещённый procparam eboot;
`_sceKernelRtldSetApplicationHeapAPI` сохраняет таблицу callbacks аллокатора.

`runtime_thread.c` реализует self и запросы init/get/getaffinity/destroy
атрибутов текущего гостевого потока. Для этого прототипа существует один
виртуальный CPU (маска 1); это не маска host CPU и не настройка его affinity.
Создание гостевых потоков и изменение атрибутов остаются неподдержанными.

`probe.c` выделяет память, копирует сегменты и применяет перемещения адресов.
Неизвестные функции направляются в исполняемые перехватчики, печатающие NID
и завершающие процесс. Неподдерживаемые импортированные данные направляются в недоступные
страницы: их использование вызывает ошибку, а не возвращает фиктивные данные.
После загрузки устанавливаются права страниц по ELF, код получает права RX.
Затем выполняется прямой вызов оригинальной точки входа по SysV ABI.
Подготовки аргументов достаточно для наблюдаемого участка; полноценный стек
запуска ядра PS4, исключения и общий механизм загрузки зависимых модулей
не реализованы; пока специально подключена только libc.

`runtime.c` реализует проверенный возврат из `_init_env`, реестр `atexit`/
`__cxa_atexit` с обратным порядком и фильтрацией DSO, `__cxa_finalize`,
`exit`, guard acquire/release/abort, случайный process-local stack canary,
аварийный обработчик повреждения стека, четыре функции работы с памятью и `strlen`.
Сопоставление выполняется по полному NID с library/module suffix именно
этого eboot, без автоматического «вернуть 0» для неизвестных импортов.
Реестр завершения и guards рассчитаны на текущий один гостевой поток;
рекурсивная/конкурентная инициализация guard останавливает выполнение с кодом 21.

`runtime_mutex.c` связывает PS4 mutex/атрибуты с Linux pthread, преобразует
типы и коды ошибок, поддерживает рекурсивные и статически созданные mutex.
Приоритетные протоколы, PS4-планировщик и гостевые потоки не реализованы.
Opaque handles предназначены для реализованных API; двоичный layout
внутренних PS4 mutex не воспроизводится. Наблюдаемый участок libc работает
через эти API; прямой доступ к внутренностям mutex потребует отдельной реализации.

POSIX-импорты mutex из `libScePosix` используют тот же backend через
отдельные wrappers: возвращают положительные PS4 errno, а не отрицательные
коды `scePthread`. Двухаргументный `pthread_mutex_init` не читает отсутствующий
третий аргумент. Linux errno не передаётся гостю напрямую.

`runtime_sema.c` реализует kernel create/delete, wait/poll, signal/cancel.
Handles — уникальные 32-битные ID; удалённые ID не переиспользуются.
Проверены счётчики и переполнение, таймаут в микросекундах с монотонными
часами, резервирование токенов до пробуждения, отмена и удаление с ожидающими
host-потоками. Объект освобождается после возврата всех ожидающих вызовов.
В режиме FIFO обслуживаются подходящие запросы в порядке очереди; запрос
меньшего размера может пройти ожидающий запрос, для которого токенов ещё нет.
Это соответствует использованной справочной модели shadPS4; сопоставление
на реальной PS4 пока не выполнено.
Атрибуты 0/2 допускают создание и неблокирующие операции, но блокирующее
ожидание явно останавливает прототип (21): приоритетный PS4-планировщик ещё
отсутствует. Ненулевые option parameters также не реализованы. В текущем
запуске игра создала 17 семафоров с attr=2, initial=0, max=1, но пока не
выполняла wait/signal; эти операции проверены отдельными C-тестами.

`runtime_time.c` поддерживает POSIX `gettimeofday`: время берётся у host,
поля копируются в явные PS4 timeval/timezone. Проверены границы полей и
диапазон микросекунд. Неожиданная ошибка host API останавливает прототип:
полный POSIX errno/TLS bridge ещё не реализован.

`runtime_rwlock.c` поддерживает init/destroy, read/write, try-read/try-write,
timed-read/timed-write и unlock поверх Linux pthread. Реестр синхронизирует
инициализацию статических handles, отслеживает владельцев, читателей и
ожидающие вызовы: уничтожение занятой блокировки возвращает EBUSY,
чужой unlock — EPERM, таймаут — Orbis ETIMEDOUT. Ожидание не удерживает
mutex реестра. Проверена одновременная работа нескольких host-потоков,
но создание гостевых потоков пока отсутствует. Поддержаны только атрибуты
по умолчанию; нестандартные атрибуты останавливают прототип с кодом 21.

`runtime_memory.c` даёт **ограниченный CPU-пул 512 МиБ для исследования**.
Это выбранный бюджет прототипа, не объём RAM PS4. Выделение возвращает
виртуальный физический offset; Linux memfd обеспечивает реальное общее
хранилище для нескольких отображений и выравнивание 16 КиБ/2 МиБ.
Реализованы allocate/map, полное unmap и полное release. Поддержаны типы
памяти 0/3 для CPU, permissions read/write, flags=0, автоматический адрес;
иной режим останавливается с диагностикой. Частичные release/unmap пока
возвращают EINVAL. GPU coherency, резервирование фиксированных адресов,
полное адресное пространство PS4 и подключение этого пула к Vulkan отсутствуют.

`vulkan_smoke.c` создаёт instance/device/queue, отправляет `vkCmdFillBuffer`,
ставит барьер для чтения CPU, ожидает завершения и сравнивает все слова буфера.
Это проверка будущего графического backend, не переводчик GNM.

## Сборка на других системах

Linux x86-64: Python 3, GCC/Clang, Vulkan headers и loader. Обычно достаточно:

```bash
python3 native_probe/prepare.py CUSA03173
python3 native_probe/link_libc.py CUSA03173
bash native_probe/build.sh
```

Скрипт поддерживает `CC`, `VULKAN_INCLUDE`, `VULKAN_LIB`;
при наличии `pkg-config vulkan` использует его настройки.

Для Windows в исходнике есть ветка `VirtualAlloc`/`VirtualProtect` и явный
SysV ABI. Предполагается **MinGW-w64 GCC/Clang x86-64**, не MSVC.
Эта ветка **не собрана и не проверена** в текущем Linux-окружении.
Пример ручной сборки в окружении MinGW с установленным Vulkan SDK:

```text
python native_probe/prepare.py CUSA03173
gcc -std=c11 -O2 -Wall -Wextra native_probe/probe.c native_probe/runtime.c native_probe/runtime_mutex.c native_probe/runtime_thread.c native_probe/runtime_rwlock.c native_probe/runtime_sema.c native_probe/runtime_time.c native_probe/runtime_memory.c native_probe/vulkan_smoke.c -I<SDK>/Include -L<SDK>/Lib -lvulkan-1 -o native_probe/out/bb-probe.exe
native_probe/out/bb-probe.exe native_probe/out/boot.bin
```

Пути SDK заменяются фактическими; бинарник Linux непереносим на Windows.
Диагностика сигналов и таймер в текущем исходнике реализованы только для Linux.
Новые mutex/rwlock/semaphore/time/direct-memory backends также реализованы только для Linux;
Windows дойдёт до более раннего неподдерживаемого импорта.

## Проверки

```bash
bash native_probe/build.sh --test
python3 -m unittest discover -s native_probe -v
```

Пройдено 35 Python-тестов, включая запуск набора C-проверок runtime:
границы SELF/образа, проверка инструкции `_init_env`, прямое исполнение
x86-64, перемещения адресов, режим strict, отключение неподтверждённого runtime,
диагностика сбоев. C-проверки охватывают обратный порядок деструкторов,
DSO и повторную финализацию, регистрацию при финализации и рост реестра,
guard acquire/abort/release, ошибки и рекурсивность mutex, выравнивание,
непересечение физических выделений, shared aliasing и повторное выделение
очищенной памяти. Stack protector и рекурсивный guard проверены в subprocess.
Дополнительно проверены жизненный цикл rwlock, повторные read-захваты,
чужой unlock, занятое уничтожение, несколько одновременных читателей,
ожидающий writer, преобразование таймаутов и `strlen` с NUL внутри строки.
Добавлены проверки нативного инициализатора/экспорта, приоритета host API,
отключения libc в strict-режиме, прав исполнения и границ новых metadata,
совпадения идентичности при разных локальных ID, отказа при другой версии
библиотеки и обработки TLS-релокаций. C-тест проверяет отдельные TLS-блоки
в host-потоках, шаблон/BSS, запросы атрибутов и регистрацию heap API.
Отдельный `sema-test` проверяет 32-битную запись handle без порчи соседнего
поля, устаревшие ID, переполнение, таймаут, очередь из нескольких host-потоков,
резервирование токенов, отмену/удаление с ожидающими и явный отказ от
приоритетного блокирующего ожидания. Проверены положительные ошибки POSIX
mutex, рекурсивный захват и выходные структуры `gettimeofday`.
Linux-сборка выполнена с `-Wall -Wextra -Werror`.
Актуальный интеграционный запуск настоящего eboot записан в `out/run.log`;
результат старого режима — в `out/strict-run.log`.

## Следующие этапы настоящего порта

1. Реализовать загрузку/реестр системных модулей и необходимые контракты
   AppContent. Игра вызвала `sceSysmoduleLoadModule(0xb4)` из `0x2026149`;
   по справочной таблице этот ID означает `libSceAppContent`. Такого файла
   среди семи локальных `sce_module/*.prx` нет. Потребуется реализация
   используемых API и состояния модуля; простой возврат успеха не добавлен.
2. Продолжить конструкторы, дополнить память/синхронизацию, затем реализовать
   TLS остальных модулей, запуск потоков и загрузку необходимых модулей.
3. Дойти до `main`, добавить файловую систему (`/app0` → каталог ресурсов)
   и фиксировать каждый следующий неподдерживаемый контракт.
4. Перевести GNM/PM4-команды, форматы ресурсов и AMD GCN-шейдеры в Vulkan/SPIR-V;
   затем связать VideoOut с окном и swapchain. Наличие Vulkan само по себе
   не делает графический код PS4 совместимым.
5. Для статической рекомпиляции восстанавливать функции и контрольный поток,
   таблицы виртуальных вызовов, исключения и ABI; проверять поведение сравнением
   с исходным машинным кодом. На x86-64 прямое исполнение с совместимым runtime
   уже позволяет исследовать запуск без предварительного перевода всего CPU-кода.

Приоритет следующего изменения — Sysmodule/AppContent и следующие конструкторы.

## Справочные источники

Формат SELF и правила соотнесения сегментов сверены с
[заголовками загрузчика shadPS4](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/loader/elf.h)
и [загрузкой сегментов](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/loader/elf.cpp).
PS4 dynamic-таблицы и вычисление NID сверены с
[module.cpp](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/module.cpp),
контекст запуска — с [linker.cpp](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/linker.cpp).
Сигнатуры mutex и значения типов сверены с
[mutex.cpp](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/libraries/kernel/threads/mutex.cpp)
и [pthread.h](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/libraries/kernel/threads/pthread.h),
rwlock — с [rwlock.cpp](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/libraries/kernel/threads/rwlock.cpp),
сигнатуры direct memory — с
[memory.cpp](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/libraries/kernel/memory.cpp).
Семафоры сверены с
[semaphore.cpp](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/libraries/kernel/threads/semaphore.cpp),
структуры времени — с
[time.h](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/libraries/kernel/time.h).
Идентификатор AppContent — с
[sysmodule_table.h](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/libraries/sysmodule/sysmodule_table.h).
Новые контракты регистрации heap API и procparam сверены с
[linker.h](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/linker.h)
и [process.cpp](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/libraries/kernel/process.cpp),
атрибуты потоков — с
[pthread_attr.cpp](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/libraries/kernel/threads/pthread_attr.cpp).
Тело `_init_env` и устройство guards проверены дизассемблированием локальной libc.
Реализация прототипа написана отдельно; зависимости на исполняемый shadPS4 нет.

Каталог `out/` содержит производные данные игры, имеет `.gitignore` и не нужен
для распространения исходников инструмента.
