# SC

[![SC compiler CI](https://img.shields.io/github/actions/workflow/status/sviroxk/sviroxk-compiler/ci.yml?branch=main&style=for-the-badge&logo=github&label=SC%20compiler%20CI)](https://github.com/sviroxk/sviroxk-compiler/actions/workflows/ci.yml) [![Claude](https://img.shields.io/badge/built%20with-Claude-D97757?style=for-the-badge&logo=claude&logoColor=white)](https://claude.ai) [![Viktor](https://img.shields.io/badge/built%20with-Viktor-5B4BDB?style=for-the-badge)](https://viktor.com) [![Cursor](https://img.shields.io/badge/built%20with-Cursor-000000?style=for-the-badge&logo=cursor&logoColor=white)](https://cursor.com)

**Экспериментальный язык · компилятор 0.3.0 · C++17 + LLVM · исходники `.svx`**

SC — самостоятельный универсальный язык в разработке. Компилятор проверяет
исходник и генерирует LLVM IR; его можно выполнить через `lli` или собрать в
машинный код инструментами LLVM. У каждого действия одно слово: старые синонимы (`fucn`, `slov`, `rox` …) удалены, компилятор подсказывает новое слово.

Это ранний прототип, не готовая платформа для больших проектов. Совместимость
синтаксиса и ABI до 1.0 не гарантируется. Игры и игровой движок не входят в цели.

## Быстрый старт

Нужны Bash, C++17-компилятор (`clang++`), LLVM **18** (`llvm-config`, `lli`,
`llvm-as`, `llc`), `clang` и `timeout`. Сборка — без CMake и новых зависимостей.
Если LLVM установлен с суффиксом версии, добавьте каталог его `bin` в `PATH`.
Проект разрабатывается в том числе для Termux; CI проверяет Linux, не телефон.

```bash
./build.sh
./sc --version                       # SC compiler 0.3.0
timeout 10 ./sc examples/hello.svx > hello.ll
lli hello.ll
```

Вывод: `Привет, мир!`, затем `42`.
Альтернатива с повторной сборкой и нативным запуском: `./run.sh examples/hello.svx`.

## Что работает сейчас

| Область | Реализовано | Что ещё впереди |
| --- | --- | --- |
| Синтаксис | `let`, `fn`, `if/else`, legacy-формы; диагностика с файлом и позицией | Полная единая спецификация, LSP и форматтер |
| Числа | Fixed-width scalar types, проверка прямых литералов, явные casts | Checked casts вычисляемых значений и runtime overflow checks |
| Выражения | Арифметика, сравнения, `&&`/`||` с short-circuit | Полностью специфицированные преобразования во всех контекстах |
| Управление | Области видимости, затенение, while/for/loop и legacy-циклы, break/continue | Более полный control-flow анализ |
| Функции | Параметры, результат/void, рекурсия и объявления без тела с проверкой сигнатуры | Раздельная компиляция и стабильный FFI ABI |
| Структуры | Aggregate storage, вложенные поля, структуры внутри функций, параметры по значению | Возврат структур и модель владения |
| Массивы | Статические массивы; динамические `nw`, push/pop/resize/len/cap; проверки границ | Типизированные безопасные указатели и aliases |
| Ресурсы | `ptr`, `del`, файловые операции, ограниченный `defer` | `drop` и автоматическое управление ресурсами |
| Модули | Относительные file imports, защита от циклов, начальный source-модуль math | Namespaces, полноценная стандартная библиотека |

Raw `ptr` без дескриптора не получает безопасной проверки границ. Передача
структуры копирует поля по значению, но pointer-поля — только адреса, не память.
Это не гарантии отсутствия утечек, use-after-free или double-free.

## Типы

Таблица чисел относится прежде всего к проверенному скалярному подмножеству;
она не обещает полную модель типов для любой коллекции или внешнего ABI.

| Тип SC | Алиас / форма | Значение |
| --- | --- | --- |
| `i8`, `i16`, `i32`, `i64` | `short = i16`, `int = i32`, `long = i64` | Знаковые целые указанной ширины |
| `u8`, `u16`, `u32`, `u64` | `byte = u8`, `ushort = u16`, `uint = u32`, `ulong = u64` | Беззнаковые целые |
| `f32`, `f64` | `float = f32`, `double = f64` | Дробные числа |
| `bool` | — | `true` / `false` |
| `str` | `string` | Строка без законченной модели владения/Unicode |
| `ptr` | `nw int[n]`, `&value` | Указатель; динамический массив использует дескриптор |
| `file` | `atk`, `zap`, `pro`, `zak` | Файловый handle |
| `tip Name` | Параметр структуры; экземпляр — `svi tip Name lor value` | Пользовательский aggregate |
| `void` | Результат процедуры | Нет возвращаемого значения |

Прямые литералы проверяются по
диапазону, но это не проверка переполнения вычислений. Неявное смешение signed
и unsigned от 32 бит ограничено; точные правила — в [спецификации](docs/spec.md).

## Примеры

Каждый фрагмент — самостоятельная программа. Сохраните её в `.svx` и запустите
через `timeout 10 ./sc example.svx > example.ll && lli example.ll`.

### Функция с предварительным объявлением

```sc
fn int add(int left, int right);
let answer: int = add(20, 22);
print(answer);
fn int add(int left, int right) { return left + right; }
```

Вывод:
```text
42
```

### Структура по значению: исходник не меняется

```sc
tip Pair { int x = 0; int y = 0; }
fn int change(tip Pair value) {
    value.x = value.x + 10;
    return value.x + value.y;
}
svi tip Pair lor point = { x = 3; y = 4; };
print(change(point));
print(point.x);
```

Вывод:
```text
17
3
```

### Динамический массив

```sc
ptr values = nw int[0];
values.push(10);
values.push(20);
print(values.len());
print(values.pop());
del values;
```

Вывод:
```text
2
20
```

### `defer`: обратный порядок при выходе из области

```sc
int value = 1;
defer print(value);
defer print(2);
value = 3;
print(0);
```

Вывод:
```text
0
2
3
```

`defer` работает при обычном выходе, return и выходе из тела цикла. Значения
читаются при выполнении; как в PR22, cleanup происходит **до вычисления выражения
return**. Пока разрешены обычные операции и вызовы, но не объявления,
return/break/continue, вложенный defer, if/циклы или отдельный блок внутри defer.
Расширение возможно по решению владельца. При abort/exit cleanup не гарантирован.

## Библиотека и тесты

Начальный модуль: `stdlib/math/math.svx`. Подключение — file import, например
`imp "../stdlib/math/math.svx";` из файла в `examples/`. Есть `math_abs(int) -> long`,
`math_min/max(long, long) -> long`, `math_is_even(long) -> bool`.
`use math` и namespace-вызовы `math.abs(...)` пока не поддерживаются.

```bash
CXX=clang++ CXXFLAGS='-Wall -Wextra -Werror' ./build.sh
./test.sh
./test-conformance.sh
```

CI выполняет строгую сборку и полный suite в обычной и ASan/UBSan-конфигурациях.
Sanitizer-проверки показывают проверенные сценарии, а не полную безопасность языка.
Примеры лежат в `examples/`, regression fixtures — в `tests/fixtures/`.

## Документация

- [Спецификация языка](docs/spec.md) — единственное описание языка, примеры в ней проверяются тестами
- [Очередь работ](docs/backlog.md) · [план владельца](docs/core-audit.md)
- [Технические решения](docs/decisions.md) · [изменения](CHANGELOG.md) · [журнал](docs/progress.md)

Generics, enum, Option/Result, безопасное владение, полноценная stdlib и self-hosting
остаются планами, не реализованными возможностями. Исторические документы могут
отставать от кода; источник подтверждённого поведения — тесты.

Лицензия: **Apache License 2.0** — полный текст в [LICENSE](LICENSE).
