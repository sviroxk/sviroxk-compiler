# Журнал циклов разработки SC

## Цикл 1 — audit/build hygiene baseline
- Базовая ревизия: `0963c1b`; исходная рабочая копия была чистой.
- `./build.sh`: успешно.
- `./test.sh`: успешно; исходный suite компилировал и запускал все зарегистрированные примеры и проверял отрицательные случаи.
- Инвентарь: C++ frontend/backend (`lexer.cpp`, `parser.cpp`, `semantic.cpp`, `codegen.cpp`), LLVM IR generation, source imports by relative path, один source stdlib-модуль `math`; нет CI, системного memory model или полной diagnostics infrastructure.
- Подтверждённые предупреждения `-Wall -Wextra`: precedence в `semantic.cpp::compatible` и unused `fcloseType` в `codegen.cpp`. Исправлены в этом цикле.
- ASan/UBSan с LeakSanitizer нашёл утечки AST `Node*`, создаваемых через `new` в parser. Добавлен arena ownership на время parse/compile и regression `examples/array_write.svx`.
- Сборка `-Wall -Wextra -Werror`: прошла без предупреждений.
- Полный `./test.sh` под `-fsanitize=address,undefined`, `ASAN_OPTIONS=detect_leaks=1`: прошёл.
- Следующее: завершить cleanup git-generated artifacts, затем CLI version/help, спецификацию реализованного синтаксиса и CI. Продолжить аудит UB/краевых случаев.

## Раздел 1 — зрелость и стабильность (локальная проверка завершена)
- Добавлены `sc --version`/`sc --help`, версия `0.3.0`, EBNF-описание реально поддержанного подмножества, совместимость экспериментальной версии и conformance fixtures.
- Добавлен `.github/workflows/ci.yml`: LLVM install, `-Wall -Wextra -Werror`, suite, ASan/UBSan+LeakSanitizer. YAML parsed локально; GitHub Actions run пока не наблюдался.
- `./build.sh && ./test.sh`: прошли; CLI, conformance positive/negative fixtures и общий suite зелёные.
- `CXXFLAGS='-Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer' ASAN_OPTIONS=detect_leaks=1 ./test.sh`: прошёл.
- Известное ограничение: диагностика пока без source excerpt/caret, parser останавливается на первой syntax error; warning deprecation policy описана, но механизм ещё отсутствует.

## Цикл 2 — memory ownership и scalar bounds
- Baseline: `a0abc61`; обычные build/test прошли.
- LeakSanitizer воспроизвёл утечку `Node*` в parser, включая binary-expression copies. Перевёл parser allocations на arena, владеющую узлами весь parse/compile lifetime.
- Добавлен `examples/array_write.svx` для ранее непроверенного array element assignment/read.
- `./test.sh`: все positive/negative cases прошли.
- `CXXFLAGS='-Wall -Wextra -Werror' ./build.sh`: прошёл без предупреждений.
- `CXXFLAGS='-Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer' ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 ./test.sh`: прошёл без ASan/UBSan/LeakSanitizer ошибок.
- Исправлено: unused `fcloseType`, warning grouping, AST leaks. Ограничение: arena пока глобальная и parser state global; глубоко-copying Node с raw children остаётся архитектурным долгом.
- Добавлена семантическая валидация numeric literals в выражениях; integer/floating overflow и suffix garbage теперь получают обычную semantic diagnostic до LLVM codegen. Тесты `print(9223372036854775808)`, `print(1e9999)`, `print(123abc)` проходят как ожидаемые ошибки.
- После добавления range tests полный обычный suite прошёл; требуется повторить санитайзеры на финальном состоянии этого исправления.
- Санитайзеры повторены после range validation: обычный suite и ASan/UBSan + LeakSanitizer прошли.
- Исправлено: `math_abs(INT_MIN)` ранее вычислял `0 - value` в i32 и вызывал signed overflow. Теперь вход `int` расширяется до `long` до отрицания, результат API — `long`.
- Сигнатуры `math_min`, `math_max`, `math_is_even` приведены к `long`, чтобы принимать результат `math_abs` без сужения.
- Добавлен `examples/stdlib_abs_min.svx` с ожидаемым результатом `2147483648` и негативный тест на аргумент за пределами `int`.
- Обычный `./test.sh`: прошёл; полный `-Wall -Wextra -Werror -fsanitize=address,undefined`, LeakSanitizer `detect_leaks=1`: прошёл.
- Следующий приоритет: улучшить parser recovery / устранить глобальное состояние, либо сделать целочисленные касты checked runtime operation.

## Цикл 3 — безопасная замена строк
- Воспроизведён дефект legacy `rep`: фиксированный 512-byte stack buffer переполнялся на длинной строке; при совпадении результат был исходной строкой с replacement, дописанным в конец, а не заменой фрагмента.
- Backend переписан: вычисляет длины, ищет первое совпадение, checked-add размера, heap alloc точного объёма, memcpy prefix/replacement/tail; отсутствие совпадения возвращает отдельную heap copy. Результат имеет тип `ptr`; caller должен `del`.
- Семантика проверяет все три аргумента `rep` как `str`; отрицательный test на integer argument.
- Добавлен integration example с заменой в середине строки, отсутствующим needle и строкой больше прежнего лимита; обе возвращённые строки освобождаются.
- Обычный `./test.sh`: прошёл. ASan/UBSan + LeakSanitizer `detect_leaks=1`: прошёл.
- OOM и размерный overflow в текущем generated code завершаются `abort`, не Result/diagnostic; semantics заменяет первое совпадение. Это документировано как остаточное ограничение.
- Следующее: проверить и закоммитить текущую задачу, затем продолжить P0 аудит.

## Цикл 4 — short-circuit логика
- Baseline: `2c9fe7a`; до правки `&&`/`||` строились как eager `and/or`, то есть RHS с эффектами выполнялся всегда.
- Добавлен `toBool` для integer/float truthy conversion и control-flow lowering через условную ветку и PHI: `false && rhs` пропускает RHS, `true || rhs` пропускает RHS.
- Добавлен `examples/short_circuit.svx`, где RHS печатает маркер; тест ожидает только итоговую строку и тем самым подтверждает отсутствие обоих побочных эффектов.
- Обычный `./test.sh`: прошёл. ASan/UBSan с warnings-as-errors и LeakSanitizer: прошёл.
- Случайный сбой проверки оказался дублирующей строкой print в fixture, а не ошибкой CFG; дубликат удалён, stdout сравнивается точно.
- Остаток: результат этой цепочки проверен; общий стресс-тест deeply nested logical CFG остаётся задачей conformance suite.

## Цикл 5 — статические bounds checks
- Подтверждено, что статические array reads/writes ранее не проверяли индекс до inbounds GEP.
- Добавлена проверка отрицательного и `index >= length` для статических массивов с line/column runtime diagnostic и ненулевым exit.
- Позитивный array-write и негативные `-1`/`length` примеры прошли обычный и ASan/UBSan suite.
- Dynamic ptr arrays не хранят runtime length; их границы остаются непроверяемыми и задокументированы как ограничение.
- Коммит: `9d9eb8a`.

## Цикл 7 — строгая конверсия float literals в integer
- Запрещено неявное преобразование decimal/exponent float literals в integer, включая инициализацию `int` значениями `3.0` и `1e3`; семантическая ошибка возникает до LLVM lowering.
- Явный cast `(int) 3.0` сохраняет поддержку и проверяется положительным примером `examples/numeric_literals.svx`.
- Добавлены отрицательные проверки для decimal и exponent литералов; обычный и строгий sanitizer suite выполнены в рамках этого цикла.

## Цикл 6 — текущая работа
- Проверено состояние repo и backlog. Остались незакоммиченные `codegen.cpp`, `semantic.cpp`, `test.sh`, `examples/string_replace.svx` после предыдущего rep fix.
- При очередной сборке выяснилось, что `build.sh` создаёт `sc` без executable permission; sanitizer runner спотыкался на status 126. Исправлено добавлением chmod на бинарник.
- Подтверждены обычная и ASan/UBSan/LeakSanitizer сборки и тесты после исправления тестовой длины expected string.
- Дальше: зафиксировать исправленный rep и build script; затем продолжить P0 audit, не теряя пользовательские изменения.

## Цикл 8 — control-flow анализ: definite assignment и shadowing
- В `semantic.cpp::declare()` добавлен флаг `isInitialized` (по умолчанию false).
- `checkFunction()` помечает параметры `isInitialized = true` — параметры всегда инициализированы при входе.
- `NODE_FOR_EACH` и `NODE_FOR_RANGE` помечают итерационные переменные как инициализированные.
- `NODE_ARRAY_DECL` помечает массив как инициализированный (элементы заданы при создании).
- Удалена ошибка на затенение — перенесена в policy: inner scope может затенять outer scope без ошибки (только дубликат в том же scope остаётся ошибкой).
- `checkInitialized()` вызывается в `NODE_IDENTIFIER` при чтении переменной.
- `markInitialized()` вызывается при успешном присваивании.
- `./test.sh`: все 68 примеров + CLI + 28 негативных fixture + импорт/циклы — зелёно.
- Коммит `9302a23`, ветка `fix/definite-assignment` (main защищён, push идёт в ветку).

## Цикл 9 — динамические массивы: length/capacity и runtime bounds check
- Добавлен в `Symbol` учёт `isDynArray`, `dynArrayElementType`, `dynArrayLength`, `dynArrayCapacity`.
- `NODE_DYN_ARRAY` (`nw T[n]`) в семантике возвращает `ptr`; дескриптор `{ptr data; i64 length; i64 capacity}` выделяется через `malloc` в codegen.
- `emitArrayBoundsCheck` переписан на динамическую длину (i64), статические массивы передают `getInt64(n)`.
- Добавлены runtime bounds checks для динамического array access/assign (negative и `>= length` → diagnostic + exit).
- OOM при `malloc` ведёт к `abort()` (допуск: не Result/diagnostic).
- `del` освобождает data pointer через `free`.
- `./test.sh`: все 68 примеров + CLI + 28 негативных fixture + импорт/циклы — зелёно. `examples/b2_dynamic_array.svx` → `15`, `test_dyn_array.svx` → `10 20 30`.
- Коммит в ветку `fix/dynamic-array-semantics`.

## Stage 2 — единая типовая система `sc::Type` (ветка `feat/nested-struct-access`)
- Заменены строковые сравнения типов в `semantic.cpp` на единую типовую структуру `sc::Type` (TypeKind enum: Error/Integer/Float/Bool/Char/Str/Ptr/Void/File/Struct/Array/DynArray).
- `compatible()` в Checker теперь делегирует напрямую в `sc::Type::compatible()` — исторические исключения `char <-> u8`, `ptr <-> array/dynarray` перенесены в сам тип.
- `fitsNumericLiteral()` переписан через `sc::Type::fitsLiteral()` + `fitsArithmeticLiteral()` (отвергает узкие типы narrower 32 бит и float-литералы для целевых типов).
- `binary()` арифметического promotion делегирует в `sc::Type::commonType()` — устранена дублирующая логика смешанной signedness; несовместимые пары теперь дают контролируемую semantic-ошибку вместо silent `i32`.
- `sc::Type::from()` нормализует все синонимы (`int/i32/rox`, `uint/u32`, `byte/u8`, `array<...>` → Array, `dyn<...>` → DynArray, неизвестное → Struct).
- `Type::toString()` даёт каноническое представление; `isConvertibleTo()`, `promote()`, `isScalar()`/`isNumeric()` — вспомогательные предикаты для semantic/codegen.
- `./test.sh`: все 68 примеров + CLI + 28 негативных fixture + импорт/циклы — зелёно.
- `CXXFLAGS='-Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer' ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 ./test.sh`: прошёл.
- Коммит `a77556f feat: nested struct field access and recursive struct literal initialization` + Stage 2 типовой миграции.
