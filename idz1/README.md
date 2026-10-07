# АКОС · ИЗ №1 · Вариант 34

Сортировочный центр посылок: последовательная модель на C11/POSIX.
Нужны компилятор C и Make; для тестов - Python 3.

## Сборка и запуск

```sh
make
./build/sorting_center
./build/sorting_center --help
```

Остановка: Ctrl+C. Для показа с задержкой: `make run`.

## Проверка

```sh
make test
sh tests/demo.sh
```

Демонстрационные журналы сохраняются в `logs/`.
Целевая ОС - Linux; В Linux: `sh tests/linux.sh`.

## Отчёт

[Отчёт в PDF](docs/main.pdf) · [Исходник LaTeX](docs/main.tex)
