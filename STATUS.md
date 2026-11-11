# SC — статус разработки

SC находится на ранней стадии разработки. Наличие работающего прототипа компилятора не означает, что язык, стандартная библиотека или инструменты готовы для крупных проектов. Разработка игр и игрового движка не входит в цели проекта.

## Сделано

- Версия компилятора `0.3.0` (экспериментальная); `sc --version`, `sc --help` и отказ на неизвестный флаг покрыты тестом.
- Добавлена `docs/language.md` с EBNF-фрагментом только реализованного подмножества, политикой совместимости и conformance fixtures; полная спецификация ещё не завершена.
- Добавлен GitHub Actions workflow: warning-clean build, conformance/regression suite, ASan/UBSan. Локально обе конфигурации проходят; удалённый Actions run после push ещё требует проверки.
- Deprecated warnings пока не поддерживаются; legacy syntax остаётся рабочим и не помечен deprecated.

- Generated ARM64 binaries, object files, LLVM/assembly output and empty build log removed from Git tracking; `.gitignore` excludes local build products. `test.svx` retained as an actual source regression fixture; stale `test` executable and unreferenced `test.txt` removed.
- Parser state is per invocation; `ParseResult` owns its error status. Copied AST nodes retain expression subtrees through shared ownership, without a global arena; repeated parsing and subtree lifetime have an ASan/UBSan regression.
- Legacy `rep` now replaces the first occurrence into a dynamically sized heap buffer; callers release it with `del`. Long input, match/no-match and sanitizer cases pass; OOM/size overflow currently abort.
- `build.sh` accepts `CXXFLAGS`; `-Wall -Wextra -Werror` and ASan/UBSan+LeakSanitizer verification passed.
- Semantic analysis rejects out-of-range integer/float literals and trailing parse garbage in every expression context before LLVM generation; implicit decimal/exponent float-literal-to-integer conversion is rejected, while explicit numeric casts remain supported.

| Область | Статус |
|---------|--------|
| Лексер + номера строк в ошибках | ✅ |
| Операторы `+ - * /` с обычным приоритетом | ✅ |
| Переменные, массивы, условия, циклы, функции | ✅ |
| Указатели, файлы, импорт, ввод/вывод | ✅ |
| Структуры + `p.field` | [~] отдельные scalar fields заменены на LLVM aggregate storage с проверенным layout/read/write; nested structs и struct call ABI ещё не завершены |
| Инициализация: `svi tip Player lor p = { health = 80; };` | ✅ |
| `fucn dor имя(...)` — возврат `dor`/`cos`/и др. | ✅ |
| Типы параметров в `fucn` | ✅ |
| `rep()`, `test.sh` | ✅ |

- ASan generated-program suite включает 10 отдельных memory-safety cases для heap/del, dynamic arrays и файлов; 3 heap lifecycle cases проходят с LeakSanitizer. Unsafe detector self-tests подтверждают инструментирование, но не означают поддержку безопасных указателей. Контракт и исключения: `docs/memory-safety-tests.md`.

## Следующие направления

- [~] Скалярные типы и алиасы `int/i32`, `uint/u32`, `short/i16`, `ushort/u16`, `long/i64`, `ulong/u64`, `byte/u8`, `float/f32`, `double/f64`; прямые литералы проверяются по диапазону, явные скалярные casts проходят runtime-тесты. `i8/i16` продвигаются до i32, `u8/u16` до u32; mixed signedness от 32 бит отклоняется. Decimal/exponent float literals поддержаны и покрыты runtime-тестами; их неявное преобразование в integer запрещено, явный cast покрыт положительным и отрицательными compile tests. Точные массивы/поля структур отклоняются; pointer/heap array path остаётся i32; runtime overflow checks ещё нет.
- [~] Операторы `<=`, `>=`, `&&`, `||`, унарный `!` и скобочная группировка реализованы; `&&`/`||` используют short-circuit CFG и проверяются побочными эффектами правого операнда. Числовые logical values приводятся к truthy/falsy; язык пока не имеет отдельного требования bool-only для условий.
- [~] Runtime bounds checks реализованы для статических массивов: отрицательный и верхний индекс дают диагностику и ненулевой exit. Динамические ptr-массивы не содержат длину и пока не проверяются.
- [ ] Устойчивое дерево синтаксиса и диагностические сообщения
- [~] Первый семантический проход проверяет имена, повторные объявления, типы переменных/аргументов/возвратов, арифметику, массивы, структуры, управление циклом и ветки if/else; parser восстанавливается после нескольких syntax errors с source caret. Полный CFG analysis, definite assignment, nested struct layout и полная система типов ещё не готовы. Неаннотированные функции возвращают int; процедуры объявляются как fn void.
- [x] Разрешение вложенных относительных импортов, дедупликация и выявление циклов; пространства имён и изоляция модулей пока не реализованы
- [~] Начальный source module `math` в `stdlib/math/math.svx`: `math_abs(int)->long` безопасно обрабатывает INT_MIN; `math_min/max(long,long)->long`, `math_is_even(long)->bool` проверены импортом/runtime-тестами и ASan/UBSan. `use math`/qualified calls пока не поддержаны; большая часть стандартной библиотеки и модель управления памятью отсутствуют
- [ ] Самохостинг компилятора на SC
- [ ] Инструменты редактора, включая VS Code, после стабилизации языка

## Проверка в Termux

```bash
git pull
chmod +x build.sh run.sh test.sh
./test.sh          # все примеры
./run.sh hello.svx # сборка и запуск примера SC
```

Синтаксис короткой формы проверяется в `examples/modern.svx`. Старые `.svx` примеры остаются для совместимости.
