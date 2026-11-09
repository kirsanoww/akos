# АКОС · ИДЗ №3 · Вариант 34

Бакшинский Павел Петрович, БПИ252, 2 курс

Сортировочный центр посылок на C11. Три реализации для требований до 7–8 баллов:

| Программа | Запуск | Синхронизация и данные |
|---|---|---|
| `center_named` | родитель создаёт участников через `fork` | именованные POSIX-семафоры, POSIX shared memory |
| `center_unnamed` | родитель создаёт участников через `fork` | `sem_init(..., 1, ...)` в POSIX shared memory |
| `center_session` + `center_worker` | отдельные запуски из терминалов или скрипта | именованные POSIX-семафоры, POSIX shared memory |

Участники: каждая посылка, источник, сканер, сортировщик, конвейер и разгрузчик.
По умолчанию работают 33 участника и контроллер. Рабочих потоков pthread нет.
Общая логика находится в `src/model.c`, IPC — в `src/ipc.c`, запуск — в `src/controller.c`.

## Запуск на Mac через Docker

Docker Desktop должен быть запущен. Из этой папки:

```sh
docker build -t akos-idz3 .
docker run --rm --init akos-idz3
```

Команда по умолчанию собирает тестовые программы и выполняет тесты всех трёх реализаций.
Для отдельного запуска:

```sh
docker run --rm --init -it akos-idz3 ./build/center_named --delay-ms 100 --check
docker run --rm --init -it akos-idz3 ./build/center_unnamed --parcels 17 --error-percent 100 --retries 3 --check
docker run --rm --init -it akos-idz3 python3 scripts/run_independent.py --delay-ms 100 --check
```

Ctrl+C прерывает моделирование. Для просмотра контейнера из второго терминала используйте `docker ps`.
Журналы внутри контейнера удаляются вместе с ним; чтобы сохранить их на Mac:

```sh
mkdir -p logs
docker run --rm --init --mount "type=bind,source=$(pwd)/logs,target=/project/logs" akos-idz3 sh tests/demo.sh
```

Нативная сборка под macOS не предусмотрена: целевая ОС задания — Linux.

## Сборка и запуск в Linux

Нужны GCC или Clang, Make и Python 3 для сценариев и тестов. Все программы модели написаны на C.

```sh
make
./build/center_named --check
./build/center_unnamed --parcels 40 --error-percent 0 --check
python3 scripts/run_independent.py --check
./build/center_named --help
```

Параметры из ИДЗ2 сохранены: размеры партии и очередей, число устройств, времена,
вероятность ошибки, повторы, `--seed`, `--delay-ms`, `--check`, `--log`.
Партия — 0..512 посылок, устройства каждого вида — 1..64; ОС может ограничить число процессов.
Seed фиксирует направления и результаты чтения каждой посылки, но не порядок событий.
Повторы считаются после первого чтения: `--retries 3` допускает четыре попытки.

## Независимые процессы вручную

Сначала в первом Linux-терминале запустите контроллер:

```sh
./build/center_session --session demo --parcels 1 --directions 1 --lines 1 --scanners 1 --sorters 1 --startup-timeout 120 --check --log demo.txt
```

Во втором терминале той же Linux-системы, в папке проекта:

```sh
./build/center_worker demo plan
./build/center_worker demo parcel 1 &
./build/center_worker demo source 1 &
./build/center_worker demo device 1 &
./build/center_worker demo device 2 &
./build/center_worker demo device 3 &
./build/center_worker demo unload 1 &
./build/center_worker demo unload 2 &
wait
```

В этом примере `device 1` — сканер, `device 2` — сортировщик, `device 3` — конвейер;
`unload 1` освобождает выход, `unload 2` — ручной накопитель.
В Docker для двух терминалов используйте один контейнер и `docker exec -it <имя> sh`.
Два отдельных `docker run` по умолчанию имеют разные пространства IPC.

Контроллер не создаёт рабочих процессов. Каждый `center_worker` сам открывает объекты по имени сеанса,
отображает общую память и занимает роль. Скрипт `run_independent.py` автоматизирует эти же отдельные запуски.
Обработка начинается после регистрации всех ролей. По умолчанию ожидание ограничено 30 секундами;
`--startup-timeout` меняет предел. Повторное имя сеанса и повторная роль отклоняются.
Одновременно можно запускать несколько сеансов с разными именами.

## Завершение и ресурсы

При доставке или отказе всех посылок — код 0; SIGINT/SIGTERM — 130/143;
ошибка выполнения — 1, неверные аргументы — 2.
Ожидания семафоров проверяют остановку с интервалом до 100 мс, задержки — до 20 мс.
Родитель собирает дочерних через `waitpid`. Независимые участники отмечают завершение в общей памяти.
Контроллер закрывает и удаляет именованные семафоры и shared memory;
неименованные семафоры уничтожает после завершения дочерних процессов.

Имена IPC имеют префикс `/ak3_<uid>_<session>_`. Доступ — только владельцу, права `0600`.
SIGKILL, выключение системы и принудительное уничтожение контроллера не дают выполнить обработчик очистки;
обычная остановка — Ctrl+C или SIGTERM. Тесты проверяют отсутствие объектов в `/dev/shm` после завершения.

## Проверка и отчёт

```sh
make test
make sanitize
sh tests/demo.sh
```

Санитайзеры — AddressSanitizer и UndefinedBehaviorSanitizer. ThreadSanitizer не используется как доказательство
корректности межпроцессной синхронизации. Проверки событий и PID выполняются отдельным Python-тестом.
Подробные результаты и исправления: [DEBUGGING.md](docs/DEBUGGING.md).

[Краткий отчёт](docs/main.pdf) · [LaTeX](docs/main.tex) · [Запросы к ИИ](docs/PROMPTS.md)
