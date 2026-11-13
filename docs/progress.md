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
- Воспроизведён дефект legacy `replace`: фиксированный 512-byte stack buffer переполнялся на длинной строке; при совпадении результат был исходной строкой с replacement, дописанным в конец, а не заменой фрагмента.
- Backend переписан: вычисляет длины, ищет первое совпадение, checked-add размера, heap alloc точного объёма, memcpy prefix/replacement/tail; отсутствие совпадения возвращает отдельную heap copy. Результат имеет тип `ptr`; caller должен `del`.
- Семантика проверяет все три аргумента `replace` как `str`; отрицательный test на integer argument.
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

## Цикл 10 — семантика структур: рекурсия, mandatory fields, copy ownership
- Исправлен segfault при объявления структур с полями без значения по умолчанию: `codegen.cpp::NODE_STRUCT_DEF` делал `sf.defaultVal = *field.left` при `field.left == nullptr`. Добавлен нейтральный нулевой литерал для полей без дефолта.
- Исправлена опечатка в сообщении об ошибке `expects` → `awaited` в `semantic.cpp::checkStruct`.
- Добавлено обнаружение рекурсивных определений структур (`checkStructRecursion`) — циклы по значению отклоняются с диагнозом "бесконечный размер".
- Добавлена проверка mandatory fields: поле без значения по умолчанию должно быть указано в struct literal.
- Добавлена защита от неявного копирования структур с владеющими полями (`ptr`, динамический массив) — требует явного клонирования/перемещения.
- Добавлены examples: `struct_copy_value.svx`, `p0_struct_mandatory_field.svx`, `p0_struct_copy_ownership.svx`, `p0_struct_recursive.svx`, `struct_recursive.svx`.
- Добавлены conformance-fixtures: `recursive-struct-definition`, `struct-copy-ownership`, `struct-ptr-field-access`.
- `./test.sh`: все positive/negative cases + CLI + импорт/циклы — зелёно.
- `CXXFLAGS='-fsanitize=address,undefined -g' ./build.sh && ./test.sh`: прошёл без ASAN/UBSAN сбоев.
- Покрытие кода (gcov, 6 source files): lines 71.6% / branches 46.0% / calls 69.1%.

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
- `NODE_DYN_ARRAY` (`new T[n]`) в семантике возвращает `ptr`; дескриптор `{ptr data; i64 length; i64 capacity}` выделяется через `malloc` в codegen.
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
- `sc::Type::from()` нормализует все синонимы (`int/i32`, `uint/u32`, `byte/u8`, `array<...>` → Array, `dyn<...>` → DynArray, неизвестное → Struct).
- `Type::toString()` даёт каноническое представление; `isConvertibleTo()`, `promote()`, `isScalar()`/`isNumeric()` — вспомогательные предикаты для semantic/codegen.
- `./test.sh`: все 68 примеров + CLI + 28 негативных fixture + импорт/циклы — зелёно.
- `CXXFLAGS='-Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer' ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 ./test.sh`: прошёл.
- Коммит `a77556f feat: nested struct field access and recursive struct literal initialization` + Stage 2 типовой миграции.
104	
105	## Stage 2 fix — тестовые фреймворк и Werror cleanup
106	- Исправлен `test.sh`: `expect_compile_error` / `expect_runtime_error` теперь принимают опциональный третий аргумент `expected` (обратная совместимость).
107	- Добавлен отсутствующий `expected` для теста `missing-expression`.
108	- Исправлено предупреждение `-Wlogical-op-parentheses` в `semantic.cpp::knownType()` — добавлены скобки вокруг `&&`.
109	- `./test.sh`: все тесты проходят; `CXXFLAGS='-Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer' ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 ./test.sh`: прошёл (exit=0).
110	- Stage 2 (единая типовая система) подтверждена как завершённая: 100%.
## Stage 3 — definite assignment для полей структур и элементов массивов
- `semantic.cpp::NODE_STRUCT_FIELD` и `NODE_STRUCT_FIELD_ASSIGN`: добавлен `checkInitialized()` — доступ/присвоение к полю структуры требует, чтобы сам объект был инициализирован.
- `semantic.cpp::NODE_ARRAY_ACCESS` и `NODE_ARRAY_ASSIGN`: добавлен `checkInitialized()` для массива + compile-time bounds check для статических массивов с константным целочисленным индексом (`NODE_NUMBER` без varType) — отрицательный и `>= length` → semantic diagnostic.
- `NODE_ARRAY_DECL`: исправлено — тип символа теперь `sc::Type::arrayTy(element, length)` с реальной длиной (раньше длина не сохранялась, `normalizeName` возвращал length=0).
- `NODE_STRUCT_INSTANCE`: экземпляры структуры помечаются `isInitialized=true` (поля имеют значения по умолчанию из определения).
- `test.sh`: runtime-проверки `array-negative-index`/`array-upper-index` заменены на `expect_compile_error` с ожидаемым сообщением "индекс массива выходит за границы"; `missing-expression` — добавлен expected.
- `./test.sh`: все 68 примеров + CLI + 28 негативных fixture + импорт/циклы — зелёно.
- `CXXFLAGS='-Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer' ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 ./test.sh`: прошёл.
- Коммит `171c14d`. Stage 3 (переменные, константы, выражения, definite assignment) — 100%.

## Stage 4 — динамические массивы: push/pop/resize/len/cap
- `new T[n]` в codegen выставлял `length = 0`, `capacity = n`. **Изменено на шаге 3:** `new T[n]` создаёт n обнулённых элементов (`length = capacity = n`, как `new T[n]()` в C++ и `make([]T, n)` в Go); пустой массив — `new T[0]`, затем `push`.
- Добавлен `NODE_ARRAY_POP` в `genNode`: уменьшает length на 1 при ненулевой длине, без side-effect в противном случае.
- `NODE_ARRAY_POP` также поддерживается в `genExpr` (возвращает последний элемент).
- `NODE_ARRAY_PUSH` / `NODE_ARRAY_RESIZE` в `genNode` используют `growDynArray` с умножением capacity (минимум 1).
- `NODE_ARRAY_LEN` / `NODE_ARRAY_CAP` в `genExpr` читают поля дескриптора.
- `parseOne()` распознаёт array method calls как statements: `arr.push(x)`, `arr.pop()`, `arr.resize(n)`, `arr.len()`, `arr.cap()`.
- `parsePrimary()` для `push`/`resize` теперь кладёт аргумент в `node.left` вместо `node.args`; `semantic.cpp::NODE_ARRAY_PUSH` читает `node.left`.
- `examples/b2_dynamic_array.svx` обновлён: использует `push` вместо прямой индексной записи.
- Добавлен `examples/dyn_array_methods.svx` с ожидаемым выводом `3\n10\n20\n30\n2\n20\n1\n5`.
- `./test.sh`: все 68 примеров + CLI + 28 негативных fixture + импорт/циклы — зелёно.
- `CXXFLAGS='-Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer' ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 ./test.sh`: прошёл.
- Stage 4 (динамические массивы) — 100%.


## P0 — изолированный parser и владение копиями AST
- Baseline `179f4da`: копия выражения `1 + 2 * 3` после уничтожения исходного дерева и следующего `parse` давала ASan `heap-use-after-free` (exit 1).
- Состояние разбора перенесено в локальный `Parser`; `ParseResult` возвращает дерево и собственный статус ошибок. Глобальная arena удалена, `left/right/extra` используют shared ownership. Копирование сохраняет время жизни поддерева, но не является независимым deep clone.
- `tests/parser-state.cpp` проверяет переживание исходного дерева/повторного разбора, восстановление после ошибочного разбора, независимый статус и пустой input. Тест включён в `test.sh`, включая ASan/UBSan CI-конфигурацию.
- LLVM 18.1.8: warning-clean build, `./test.sh`, отдельный `./test-conformance.sh` (3/3) и полный `./test.sh` под ASan/UBSan + LeakSanitizer прошли. Каждый полный прогон включает 93 программы под ASan и новую API regression. Синтаксис SC не менялся; потоки и performance benchmarks не проверялись.


## P0 — memory-safety sanitizer cases для pointers/arrays/files
- Baseline `ad3096d`: compiler sanitizers и ASan 93 программ уже проходили, но отдельной проверки детектора UAF/double-free/raw OOB и leak-clean heap lifecycle cases в CI не было.
- Добавлен `tests/memory-safety.sh`, вызываемый из `test.sh` после ASan probe: 4 корректных случая (3 с LeakSanitizer), 3 контролируемые runtime ошибки, 3 намеренно небезопасных self-tests детектора. Вывод, статус и точный класс sanitizer-ошибки проверяются раздельно; timeout не считается ожидаемой ошибкой.
- При расширении покрытия выявлен старый verifier-баг `print(v.pop())`: PHI predecessors mismatch, exit 3. Вне задачи, не исправлен; воспроизведение добавлено в backlog. Statement `v.pop();` покрыт положительно.
- Runtime metadata/ownership не вводились. Ограничения file/string leaks, закрытых handles, alias lifetime и UBSan для generated IR описаны в `docs/memory-safety-tests.md`.
- LLVM 18.1.8: strict build, полный `test.sh` (прежние 93 программы под ASan + 10 новых cases), отдельный conformance (3/3), полный ASan/UBSan/LeakSanitizer compiler suite и `bash -n` прошли. Mutation check: временная копия suite без `sanitize_address` корректно падает на UAF detector (exit 1, unsafe program exit 0), подтверждая чувствительность теста.


## Исправление pop() expression — PHI predecessors
- Baseline `1ec686b`: сначала добавлены 4 regression cases в `test.sh`; первый (`new int[0]`, `push(1)`, `print(v.pop())`) провалился с compiler exit 3, `PHI node entries do not match predecessors`.
- В `genExpr(NODE_ARRAY_POP)` сохранён блок перед условным переходом. Именно из него идёт empty-array ветка в `pop_done`; раньше PHI неверно ссылался на сам `pop_done`. Statement lowering и контракт empty pop = typed zero не менялись.
- Regression coverage: pop после роста, повторный pop пустого массива, LIFO/длина и два pop в одном выражении, pop внутри if, f64 значение/нулевой результат. Все 4 cases проходят и попадают в ASan-прогон.
- LLVM 18.1.8: strict build, полный `test.sh` (97 программ под ASan + 10 memory-safety cases), отдельный conformance 3/3, полный ASan/UBSan/LeakSanitizer suite прошли. Termux/ARM64 и allocator/OOM paths не проверялись.


## Шаг 4б — структуры по значению из закрытого PR22
- Сопоставлены PR22 `edcaacf`/stage5 `5cfce47` и main `d578daa`. Существующие nested literals/read, aggregate assignment и структуры внутри функций (#25) сохранены без переписывания; недостижимая дополнительная materialization не перенесена.
- До правки добавлены 4 output regressions и 6 expected-errors с текстом. На отдельной сборке чистой базы все 10 новых ожиданий проваливаются (parser exit 1); полный test.sh также воспроизвёл отказ на первом новом случае.
- Добавлены только `struct Name` в параметрах/вложенных полях, preregistration struct types перед LLVM function signatures и field lookup для параметров-структур. Положительные cases проверяют копию, nested aggregate replacement/read, forward declaration/mixed arguments, u8/f64 layout.
- LLVM 18.1.8: strict build, полный test.sh (101 программа под ASan + 10 memory-safety cases), conformance 3/3 и полный ASan/UBSan/LeakSanitizer прошли. Ошибки проверяют неизвестные типы/поля, неверный аргумент, несовпадение forward signature и рекурсию.
- `defer`/`drop`, struct returns, anonymous struct arguments, ownership/clone ABI и checked casts не добавлялись. Termux/ARM64 и внешний ABI не проверялись. Следующий шаг плана — `defer`.


## Шаг 4в — defer из PR22
- Источник: `fa7e9ce` из закрытого PR22; main `8157599`. До кода добавлены 8 output + 8 text-diagnostic cases: все 16 ожиданий провалились на baseline. Отдельная рабочая ветка теперь постоянная `dev` по новому правилу владельца.
- Перенесены syntax/lexical scope/LIFO, позднее вычисление и cleanup до return expression. Генерация по путям CFG не теряет cleanup sibling-веток; break/continue очищают только покидаемые loop scopes. Bindings сохраняются при регистрации, значения читаются при выполнении; shadow case усилен ранним void return.
- По согласованному временному ограничению допустимы только обычные операции/вызовы. Объявления, return/break/continue, вложенные defer и control-flow внутри defer дают semantic-ошибку (D-006); `drop` и `.github` не изменены.
- LLVM 18.1.8: warning-clean build в полном test.sh, обычный и полный compiler ASan/UBSan/LeakSanitizer suite прошли (109 ASan programs + 10 memory-safety cases, conformance 3/3 внутри каждого прогона). Проверены все формы циклов, return из ветки/цикла, LIFO, shadow и delayed assignment/call. Termux/ARM64, OOM и аварийное завершение не проверялись; cleanup при abort/exit не гарантирован.
- Далее по запросу владельца отдельный housekeeping PR (README, LICENSE, fixtures), затем языковой шаг `drop`.


## Техническая уборка после defer
- Main `f48770d`: `.vscode` удалена и добавлена в `.gitignore`; hello/math сохранены в `examples/hello.svx` и `examples/legacy_square.svx`, пять root regression fixtures — в `tests/fixtures/`. Содержимое всех семи .svx сохранено, source list/output checks перенастроены без удаления тестов. Старые имена в прошлых записях журнала — исторические.
- `run.sh` оставлен как рабочий native-run helper: обновлён default path и compiler timeout по AGENTS. `.sc-version` оставлен как существующий маркер версии (скрипты его не читают); спорные файлы не удалялись. Правила AGENTS приведены к явно заданной владельцем схеме main/dev, squash без удаления и force-with-lease.
- LICENSE заменён точными байтами официального `https://www.apache.org/licenses/LICENSE-2.0.txt` (11358 байт, включая Appendix). README переписан под текущее подмножество: таблицы возможностей/типов, quick start, 4 запускаемых примера с ожидаемым выводом, авторы SVIROXK/Claude/Viktor; старое имя убрано. История авторов коммитов просмотрена, чисел вклада/новых заслуг не заявляется.
- Каждый из 4 точных SC-фрагментов README сначала скомпилирован с timeout и выполнен через lli: 42; 17/3; 2/20; 0/2/3. Документ записан после сверки вывода. Default run.sh с native link/run выводит приветствие и 42; relative links, canonical license, bash syntax и diff проверены.
- LLVM 18.1.8: warning-clean build и полные normal + ASan/UBSan/LeakSanitizer test.sh прошли; в каждом conformance 3/3, 109 ASan programs + прежние 10 memory-safety cases. Код компилятора, синтаксис и `.github` не менялись. Termux/ARM64 и визуальный рендер GitHub не проверялись. Далее — отдельный языковой шаг `drop`.


## Сайт документации из спецификации
- `sviroxk/SC-DOC` PR #2: `index.html` собирает `gen.py` из `docs/spec.md` (коммит `1f4c99e`), устаревший ручной текст удалён. Все 23 примера, извлечённые из готовой страницы, прогнаны LLVM 18.1.8: 16 программ дали указанный вывод, 7 `sc-error` — указанную ошибку. Pages-деплой main успешен.
- spec §17: строка «нет `==` для строк» противоречила §11 — исправлено на «нет `+`, `<` и `>`». Пункт 1 очереди закрыт.


## LLVM: эталон 23.1.5 и популярные версии; без требования Termux
- Решения владельца 2026-11-12: эталон LLVM 23.1.5, компилятор обязан собираться и на популярных версиях; сборка на телефоне (Termux) больше не требуется — упоминания удалены из AGENTS/README/handoff, способ сборки (`./build.sh` + `llvm-config`) не менялся.
- До правки: 18.1.8 и 19.1.7 — сборка с `-Werror` и все тесты проходят; 20.1.8–22.1.8 — сборка падает (`Intrinsic::getDeclaration` устарела, `-Wdeprecated-declarations`); 23.1.5 из официального архива — падает на предупреждениях в заголовках LLVM: `build.sh` делал `-isystem` только для путей со строчным «llvm», а архив лежит в `/opt/LLVM-23.1.5-Linux-X64`.
- Исправлено: `getOrInsertDeclaration` при `LLVM_VERSION_MAJOR >= 20`; все `-I` из `llvm-config` идут как `-isystem`. Новый `tests/build-flags.sh` (из `test.sh`) собирает с путём заголовков без «llvm» в имени; до правки падал на 18 и 23.
- Матрица `ci.yml` для 18–23 подготовлена для владельца (workflows агент менять не может).


## Рефакторинг ядра: позиция у каждого узла AST
- Новый `tests/ast-positions.cpp` (из `test.sh`) обходит AST примеров, fixtures, stdlib и всех `sc`-программ spec. До правки (с `line/column = 0` по умолчанию) без позиции были узлы двух десятков видов: BINOP из `*`/`+`, IF, break/continue, поля структур, инициализаторы полей (у них ещё и не был задан `type`), литерал структуры, вспомогательный 0 у `!`/унарного минуса и др.; у многих не было имени файла.
- Исправлено в `parser.cpp`: `setPos`, `parseMul`/`parseAdd` через `makeBinary`, инициализатор поля — `NODE_ASSIGN` с позицией. После: 1875 узлов, все с позицией.
- Найденный баг: ошибка типа в инициализаторе поля (`P p = { a = "x"; };`) печаталась в `1:1`, теперь в позиции поля (`3:9`); тест `struct-field-init-position`.
- Парсер без глобального состояния уже сделан в PR #28 (класс `Parser`); пункт снят с очереди.


## Уточнение к записи о версиях LLVM
- Пересборка коммита до правки (`7ed2790`) показала точнее: на 20.1.8 и 21.1.8 — `'getDeclaration' is deprecated [-Werror,-Wdeprecated-declarations]`, на 22.1.8 функции уже нет (`no member named 'getDeclaration' in namespace 'llvm::Intrinsic'`, ошибка и без `-Werror`); на 23.1.5 — 19 × `[-Werror,-Wunused-parameter]` в заголовках LLVM. `llvm-config --cxxflags` во всех версиях 18–23 даёт `-std=c++17`; ни одна ошибка со стандартом C++ не связана.


## Переход на C++20
- Решение владельца 2026-11-12: код компилятора — C++20. `build.sh` добавляет `-std=c++20` после флагов `llvm-config` (те дают `-std=c++17` на всех версиях 18–23); в `test.sh` отдельные C++-тесты тоже собираются с `-std=c++20`. `main.cpp` проверяет `__cplusplus >= 202002L`; `tests/build-flags.sh` убеждается, что сборка как C++17 отклоняется с понятным сообщением.
- Изменений кода под C++20 не понадобилось: заголовки LLVM 18–23 под C++20 дают ноль предупреждений с `-Wall -Wextra -Werror`.
- Проверено: clang 18.1.8, 19.1.7, 20.1.8, 21.1.8, 22.1.8, 23.1.5 (с LLVM своей версии) — сборка, 269 PASS, conformance 3/3; ASan/UBSan на 23.1.5 и 18.1.8; g++ 12.2 с LLVM 18. Более старые clang/g++ не проверялись.


## Аудит, класс 4: импорт не-файла и исключения
- Воспроизведено на main `1e54e03` (LLVM 23.1.5): `import "";` и `import ".";` — `terminate … std::__ios_failure … Is a directory`, abort (код 134); `import "/dev/zero";` — зависание (timeout).
- Исправлено: существующий путь, который не является обычным файлом, даёт `Ошибка: не удалось открыть модуль: <путь> (это не обычный файл)`, код 1. `main` ловит любые исключения и печатает «внутренняя ошибка компилятора», код 3 (spec §1).
- Тесты: `empty-path`, `directory`, `device` в `test.sh`. Путь «исключение → код 3» сейчас воспроизвести нечем (после правки ни один известный вход исключение не бросает) — отдельного теста нет.
- CI-матрица LLVM 18–23 вставлена владельцем (PR #49): AGENTS/README/handoff обновлены, пункт «Матрица LLVM в CI» снят с очереди.


## Аудит, класс 2: тернарный оператор с ветвями разной ширины
- Воспроизведено на main (LLVM 23.1.5): `short s = 1; print(true ? 5 : s);` и вариант `5.0`/`f32` — «внутренняя ошибка компилятора — сгенерирован некорректный LLVM IR», код 3.
- Причина: приведение к общему типу создавалось после `br` обеих ветвей. Теперь `castTo` вставляется перед переходом в конце своей ветви.
- Тесты `ternary-mixed-int-width` (i16/i32, i64/i32) и `ternary-f32-f64` с проверкой вывода.


## Аудит, класс 5: литерал массива вне диапазона
- Воспроизведено на main `ac4b9c5` (LLVM 23.1.5): `int[] a = [2147483648];` печатал `-2147483648`, `u8[] b = [300];` — `44`, без ошибки.
- Исправлено: целый литерал в объявлении массива проверяется `literalFits` по типу элемента (как уже было для `a[i] = c`, `push`, полей и аргументов). spec §8: правило и пример `sc-error`.
- Тесты: 3 отрицательных (i32, u8, отрицательное в u8) и положительный на границы (0/255, INT32_MIN/MAX).
- Очередь: задача «Библиотеки» (extern C, имена модулей) записана в handoff и backlog по решению владельца 2026-11-12.

## Аудит, класс 6: литерал больше i32 без контекста
- Воспроизведено на main (LLVM 23.1.5): `print(-9223372036854775807 - 1);` и даже `i64 m = -9223372036854775807 - 1;` — ложная ошибка «литерал … не помещается в тип i64 (long)» (текст неверный: в i64 он помещается).
- Исправлено: литерал без контекста — `i32`, а если не помещается — `i64` (codegen уже так генерировал); ошибка остаётся только для значений вне i64. Переполнение константы в `i32` (`2147483647 + 1`, spec §4) по-прежнему ошибка компиляции. spec §2 и §4 обновлены, новый пример.
- Тесты: `i64-min-expression` (вывод), `literal-beyond-i64`, `i64-constant-overflow`.


## Аудит, класс 3: `%` для дробных
- Воспроизведено на main (LLVM 23.1.5): `f64 x = 5.5; print(x % 2.0);` — assertion `cast<IntegerType>` в codegen, abort (код 134); так же `x %= 2.0`.
- Решение владельца 2026-11-12 (D-007): разрешить, остаток как `fmod` в C. Codegen: `frem` для дробных. spec §4: правило и проверяемый пример.
- Тесты: `float-remainder` (f64, отрицательное делимое, f32, смешанный с целым, `%=`), `float-integer-division` (`//` для дробных по-прежнему ошибка).

## Аудит 07d/07e: ввод строкой целиком
- Воспроизведено на main (LLVM 23.1.5): `input_str`/`read` обрезали ввод до 255 байт и читали одно слово; `input_int` на мусоре, переполнении и конце ввода молча давал неверное число.
- Исправлено (D-007): внутренняя функция `sc.read_line` (fgetc/getchar + realloc) читает строку любой длины без `\n`/`\r`; `input_int` разбирает строку через `strtoll` и выдаёт `SC runtime error` (invalid integer input / integer input out of range / unexpected end of input). Несколько чисел в одной строке больше не читаются — каждое число на своей строке (spec §12).
- Служебные слоты — в начале функции (`entryAlloca`), чтобы ввод в цикле не растил стек.
- Тесты: `input-line-whole`, `input-line-crlf`, `input-line-long` (100000 символов), `input-str-eof-empty`, `input-int-garbage`/`-trailing`/`-overflow`/`-eof`, `file-read-lines`; `memory-safety file-roundtrip-eof` обновлён (`read` больше не возвращает `\n`).
- AGENTS.md: правило про кредиты (порог 1000, письмо — `docs/handoff.md` целиком).

## `SC runtime error` — в stderr (D-007)
- Было: диагностика выполнения печаталась в stdout (`puts`/`printf`) и смешивалась с выводом программы.
- Стало: одна точка `emitRuntimeFailureIf` печатает через `dprintf(2, …)`; проверка границ массива больше не дублирует её своим `puts`.
- Тесты: `expect_runtime_error` и `tests/memory-safety.sh` (режим runtime) теперь требуют сообщение в stderr и его отсутствие в stdout; новый `runtime-error-stderr` (stdout ровно `before`). `file-read-lines` пишет временный файл в `$TMP_DIR`, а не в корень репозитория.

## Аудит 07a/07b: размер `new T[n]` и `resize(n)`
- Воспроизведено на main (LLVM 23.1.5): `new int[-1]` — abort (134), `v.resize(-1)` — segfault (139), `new int[2^62]` — abort, `resize(2^62)` — молча длина 2^62, `resize(INT64_MAX)` — segfault, `u64` максимум в `new` — abort; `resize` вверх после уменьшения читал мусор (`21868`).
- Исправлено: `emitArraySize` (знак/беззнаковость, отрицательный → `negative array size`, u64 > INT64_MAX → `array size too large`); `growDynArray` проверяет переполнение `n * размер` и `malloc == NULL` → `out of memory`; `new` больше не зовёт `abort`; `resize` вверх обнуляет новые элементы (spec §7).
- Тесты: `new-negative-size`, `resize-negative`, `new-huge-size`, `resize-huge`, `new-u64-max`, `resize-zero-fill`, `new-zero-then-push`.

## Аудит 07c: использование файла после `close(f)` (D-007)
- Воспроизведено на main (LLVM 23.1.5): `close(f); write(f, "x");` компилировалось (неопределённое поведение: запись в закрытый `FILE*`).
- Исправлено в `semantic.cpp`: `close(f)` помечает файл освобождённым, как `drop f` (те же правила: ветки, повтор, файл снаружи цикла/функции). Текст ошибки называет, чем освобождено: «используется после close (строка N)».
- Тесты: `close-then-write`, `close-then-read`, `close-twice`, `drop-after-close`, `close-after-branch`, `close-outer-in-loop`, положительный `close-in-loop-own-file`.

## Аудит 07f: `return &локальная` (D-007)
- Воспроизведено на main (LLVM 23.1.5): `fn ptr g() { int x = 5; return &x; }` компилировалось без ошибки (висячий указатель: -O0 печатал 5, -O2 — мусор).
- Исправлено в `semantic.cpp`: `return &x`, где `x` — локальная или параметр текущей функции, — ошибка компиляции; так же `ptr p = &x; return p;` (флаг `pointsToLocal` при объявлении). Адрес глобальной и возврат переданного указателя разрешены.
- Не ловится (записано как ограничение): адрес, присвоенный позже (`p = &x;`), сохранённый в поле структуры или массиве.
- Тесты: `return-addr-local`, `return-addr-param`, `return-ptr-to-local`; положительные `return-addr-global`, `return-ptr-param`.

## Аудит, класс 1: глубокая вложенность
- Воспроизведено на main (LLVM 23.1.5): 200 вложенных `if`, сумма из 400 слагаемых — segfault (139); 300 скобок — то же.
- Исправлено: в парсере `NestingGuard` (блоки, скобки, унарные операторы) — больше 256 уровней → `error[syntax]: слишком глубокая вложенность: больше 256 уровней`, разбор останавливается. Компиляция (`main.cpp`) идёт в потоке со стеком 512 МБ (`pthread_attr_setstacksize`; если поток не создан — обычный стек), как `runWithSufficientStackSpace` у clang; `build.sh` добавляет `-pthread`.
- Измерено после правки (23.1.5): 256 `if` и 256 скобок компилируются; 257 и 100000 — ошибка; сумма 2000/5000/10000 слагаемых — работает, но 5000 ≈ 7 с, 10000 ≈ 29 с, 20000 > 60 с: где-то повторный обход поддерева (квадратичное время) — следующая задача.
- Тесты: `nesting-256-if`, `nesting-257-if`, `nesting-256-parens`, `nesting-257-parens`, `nesting-100000-parens`, `nesting-300-not`, `sum-2000-terms`, `else-if-chain-1000`.

## Аудит, класс 1: длинные цепочки `a + b + …`
- Было (main после #61, 23.1.5): время квадратичное — 5000 слагаемых ≈ 7 с, 10000 ≈ 29 с, 20000 > 60 с; 50000 — segfault даже на стеке 512 МБ.
- Причина (замер `-fprofile-instr-generate` + `llvm-profdata`, 3000 слагаемых): `CodeGen::isUnsignedExpr` и `CodeGen::integerWidth` — по 9 млн вызовов, у каждого `+` заново обходили левое поддерево; так же `Checker::constantInteger` в семантике.
- Исправлено: кэш по узлу AST (`left`/`right` в куче): в семантике — на всю проверку, в codegen — на одну инструкцию (`genNode` сбрасывает: объявления меняют таблицы переменных). После: 5000 ≈ 0,23 с, 10000 ≈ 0,47 с, 20000 ≈ 0,93 с. В парсере лимит 10000 бинарных операторов в инструкции → `слишком длинное выражение` (дерево цепочки обходится рекурсивно).
- Тесты: `sum-10000-operators` (вывод и время < 20 с), `sum-10001-operators`, `sum-two-long-statements`.

## Аудит §2: без `std::exit` и `catch (...)`
- Было (main, 23.1.5): лексер, импорт и codegen завершали процесс `std::exit(1)` посреди кода (21 место) с текстом «Ошибка 1:9: …» без имени файла; в `semantic.cpp` пять `catch (...) {}` глотали всё подряд, а `stoll("0x5")` давал 0 — константный индекс `a[0x5]` и сдвиг `1 << 0x40` не проверялись при компиляции.
- Стало: `sc::failAt`/`sc::failFile` (`diagnostics.h`) печатают диагностику в общем формате («файл:строка:столбец: error[lexer|import]» с выдержкой и `^`) и бросают `sc::CompileFailure` — её ловит только `main` и возвращает код (spec §1). Импорт показывает позицию строки `import`; не открывшийся исходный файл — «файл: error[import]». В семантике константы разбирает `constantInteger` (0x/0b/0o, `_`), без исключений; в `type.cpp` — `catch (const std::logic_error&)` вместо `catch (...)`.
- Тесты: `lexer-*` (5), `missing-module-position`, `import-without-path`, файл не найден, `hex-index-out-of-bounds`, `binary-index-store-out-of-bounds`, `hex-shift-too-wide`, `hex-index-in-bounds`.

## Аудит §3: фаззер в `test.sh`
- `tests/fuzz.py` — сокращённый фаззер аудита (мутации токенов и байтов примеров и spec, случайные байты, глубокие/длинные конструкции, случайные программы по грамматике). `./test.sh` гоняет 400 входов с seed 20251112; провал — код 3, сигнал, зависание > 10 с, ошибка без текста, отчёт ASan/UBSan. В CI ASan-задачи гоняют его на ASan-сборке компилятора. Без `python3` — SKIP. Правка `.github/workflows` не нужна.
- Первый же прогон (300 входов) нашёл segfault: цепочка `1 ? 1 : 2 ? 1 : …` из 100000 тернарных операторов — рекурсия парсера без лимита. Исправлено: каждый `?` — уровень вложенности (`NestingGuard`), глубже 256 — синтаксическая ошибка.
- Тесты: `ternary-chain-256`, `ternary-chain-100000`, сам прогон фаззера.
- CI PR #64, первый прогон: «LLVM 23 + ASan/UBSan» упал — `fuzz: 400 входов, чисто 395, проблем 5`, все 5 — «зависание дольше 10 с» (#36, #101, #220, #262, #316). Это один и тот же вход: литерал массива из 100000 элементов; локально он компилируется за 1,5–1,8 с (время линейное: 12500 — 0,18 с, 100000 — 1,5 с), но на раннере CI под ASan с 4 процессами превысил 10 с; во втором прогоне того же коммита прошёл. Недетерминизма во входах нет (seed фиксирован). Исправлено в фаззере: порог зависания 60 с, процессов — по числу ядер (не больше 4).


## Предел длины выражения: 10000 → 250000 (решение владельца «столько сколько можно»)
- Кадры стека: `genExpr` разбит на диспетчер + `genBinary`/`genOtherExpr`/`genCallExpr` (`[[gnu::noinline]]`), в семантике `expression` → `binaryExpression`/`otherExpression`; левый операнд считается в маленьком кадре. Поведение не меняется. Было ~21 КБ кадра `genExpr` на уровень — потолок ~24000 (release), ~11500 (ASan).
- Измерено с отключённым пределом, стек 512 МБ: release 23 — 1 000 000 проходит (57 с); ASan/UBSan 18 — 250000 (111 с, 1,8 ГБ), 500000 (224 с, 3,2 ГБ) проходят, 1 000 000 — переполнение стека в семантике; ASan/UBSan 23 — 500000 проходит (212 с, 3,1 ГБ).
- Предел 250000 — половина самого низкого проходящего потолка под ASan. Выше — ошибка «слишком длинное выражение». Снять совсем — задача «обход цепочек без рекурсии».
- Тесты: `sum-250000-operators` (только компиляция, таймаут 600 с), `sum-250001-operators` (тот же таймаут).
- Первый CI PR #65: под ASan `sum-250001-operators` — «hung (timeout 10s)», хотя ошибка напечаталась. Причины: `reportDiagnostic` писал отступ под `^` в небуферизованный `std::cerr` по символу (строка в 1 МБ — миллион `write`); теперь диагностика собирается в строку и пишется одним вызовом. Плюс разбор 250001 операторов — та же работа, что у 250000, под ASan на раннере дольше 10 с.

## 2026-11-12 — письмо следующему ИИ

- `docs/handoff.md` переписан целиком (правило кредитов): состояние после #65, решения владельца, очередь с первым шагом (новый полный аудит первым).
- C++20 по файлам, шаг 1: `map.count(x)` в условиях → `contains(x)` (codegen, parser, semantic, main), `rfind("array<", 0) == 0` → `starts_with` (type.cpp); лишняя проверка `count` перед `erase` убрана. Поведение не меняется.
- C++20 по файлам, шаг 2: `Type::from`, `normalizeName`, `Type::structTy` принимают `std::string_view` (разбор `array<...>` без копии подстроки); мёртвое `Type::isConvertibleTo` (нигде не вызывалось) удалено. Поведение не меняется.
- C++20 по файлам, шаг 3: в lexer.cpp цепочка `if (word == "...")` (101 сравнение) заменена `constexpr` таблицей `kKeywords` + `std::ranges::find`; мёртвая ветка `str → STR` (перекрыта `str → IDENTIFIER`) не перенесена. Тест `tests/lexer-keywords.cpp` (104 слова) проходит и на прежнем лексере, и на новом. Поведение не меняется.
- C++20 по файлам, шаг 4: `tokenize` принимает исходный текст как `std::string_view` (вызов из тестов со строковым литералом или `string_view` без временной строки); 8 лишних обёрток `std::string("...")` в сообщениях лексера убраны. Поведение не меняется.
- C++20 по файлам, шаг 5: `sc::failAt`, `sc::failFile`, `sc::reportDiagnostic` (diagnostics.h) принимают `std::string_view`; таблица исходников `sourceFiles()` ищет по `string_view` (`std::less<>`); дублирующее объявление `checkSemantics` в parser.h удалено (оно в semantic.h). Поведение и текст диагностик не меняются.
- C++20 по файлам, шаг 6: `CodeGen::runtimeFunction(std::string_view)` — сигнатуры 29 функций C в `constexpr` таблице `kRuntimeFunctions` (коды типов) + `std::ranges::find`; прежде `std::map` с векторами строился заново при каждом вызове. Набор функций и сигнатуры те же, поведение не меняется.
- C++20 по файлам, шаг 7 (codegen): `fieldIsUnsigned` обходит путь `a.b.c` по `std::string_view` и указателям на имена типов (без копии сегментов), `emitRuntimeFailureIf` принимает `std::string_view`, `getStringPtr` — `llvm::StringRef`. Текст ошибок и IR не меняются.
- Решение владельца 12.11: `replace` должен возвращать строку; строки как в C++ и Rust (отдельный тип, адрес явно, память освобождается сама). Код не менялся (`replace` пока `ptr`): переход ломает `ptr r = replace(...)` + `del r`, нужна модель памяти строк — задача 0б в handoff.md.
- Решение владельца 12.11: Python только для тестов (`tests/fuzz.py`); правило в AGENTS.md, D-008 в decisions.md, строка в README и docstring fuzz.py. Код не менялся.
- Документация сверена с main (12.11): README — убраны «legacy-формы/циклы» (удалены, spec §16), `drop` перенесён в сделанное, строка о текущем `replace → ptr` и плане «как в C++ и Rust», фаззер в `./test.sh` нужен `python3`. Остальное (LLVM 23.1.5/18–23, C++20, решения аудита, лимиты 256/250000) уже совпадало. Сайт SC-DOC устарел на один пункт — задача в handoff.

## Исправления по аудиту 12.11 (PR #75–#104)
- Аудит 12.11: 25 находок (лексер, парсер, семантика, кодогенерация, рантайм, тесты, spec); все исправлены отдельными PR #75–#89, #91–#98 с тестом, падавшим до правки.
- Решения владельца по вопросам аудита 1–6 слиты: `//` только комментарий, деление нацело — `div` (#99, D-010); режим `open` обязателен (#100, D-011); функция без типа — `void` (#101, D-012); `sqrt`/`floor`/`ceil`/`round` от целого — `f64`, `pow` двух целых — целое с проверкой переполнения (#84); свои функции с именами встроенных (#102, D-013); `del` статического массива — ошибка (#103); после `del` имя недоступно, повторный `del` — ошибка, как у `drop` (#104).
- `./test.sh`: 341 → 414 PASS (LLVM 20, 23, 23 + ASan/UBSan), `./test-conformance.sh` 3/3; фаззер 8000 входов, seed 20251112: до 100 %, после 100 % чистых (8000/8000).
- Непроверенная догадка осталась задачей в handoff: `ptr p = &x; del p;`.

## 2026-11-13 — строки, шаг 1 (задача 0б, D-014): replace → string, data/as_ptr
- `replace` возвращает только `string` (было `ptr` + обязательный `del`, D-002 заменено D-014): `string s = replace(…)` и `let t = replace(…)` работают, результат передаётся в `fn(string)`.
- Старая запись `ptr r = replace(…)` — ошибка «string неявно в ptr не превращается…» с подсказкой; та же подсказка при `ptr p = s`, `p = s`, передаче строки в `fn(ptr)`; `del` строки — ошибка «del для строки не пишется».
- `data(s)` и `as_ptr(s)` — явный адрес символов (`ptr` на `char`); `&s` — ошибка с подсказкой; `del` такого указателя — «освобождать нечего».
- Переписаны: spec §9, §11, §15, `examples/string_replace.svx`, `new-words-builtins`, `tests/memory-safety.sh` (`heap-release` → `replace-string`, ASan-самопроверки `use-after-delete`/`double-delete`/`raw-pointer-overrun` — через `data(s)` и освобождение параметром функции).
- Письмо: решение владельца 13.11 о сайте, 0а закрыта, «Рабочие факты» — сайт не пересобирать.

## 2026-11-13 — строки, шаг 2 (0б, D-014): память строки освобождается сама
- Каждая string-переменная и string-параметр владеют своей копией в куче (`strdup`; свежий результат `str()`/`replace`/ввода/`read`/функции забирается без копии) и освобождают её в конце блока — через тот же список, что `defer` (`return`, `break`, `continue`, ветки, все виды циклов). `s = …` освобождает старое значение после вычисления нового.
- `string b = a` — копия (запись через `data(b)` не меняет `a`); функция получает копию аргумента, `return` отдаёт копию. Запись строки-переменной в поле структуры/элемент массива — копия (иначе висячий адрес после конца блока), она пока не освобождается (spec §17).
- `drop` строки — ошибка «drop для строки не пишется».
- Тесты: `tests/memory-safety.sh` — 5 новых случаев с LeakSanitizer (`string-copy-assign-release`, `string-function-release`, `string-loop-release`, `string-input-release`, `string-field-outlives-variable`), `replace-string` и `file-roundtrip-eof` теперь с проверкой утечек; `test.sh` — `string-copy-independent`, `drop-string`. На сборке до правки `string-copy-assign-release` и `string-loop-release` падали (LeakSanitizer, код 86).

## 2026-11-13 — строки, шаг 3 (0б, D-014): временные строки освобождаются в конце оператора
- Результат `str()`, `replace`, `input_str`, `read`, функции с результатом `string`, не положенный в переменную/поле/`return` (`print(str(1))`, `f(str(1))`, `s == str(5)`, `f();`), освобождается в конце оператора: слот в начале функции (NULL) + запись в `deferred_`; `return`/`break`/`continue` освобождают его вместе с переменными; условие `while` — после каждой проверки; `defer` с временной строкой — тоже.
- `data(str(1))` / `as_ptr(…)` от временной строки — ошибка компиляции (висячий адрес); аргумент — переменная, параметр или поле.
- Тесты: `tests/memory-safety.sh` `string-temporaries-release` (LeakSanitizer; на сборке до правки — утечка, код 86), `test.sh` `string-data-of-temporary`.

## 2026-11-13 — строки, шаг 4 (0б, D-015): перенос move(имя)
- `move(a)` — встроенная функция (решение владельца 13.11: только со скобками): отдаёт строку без копии, в слоте `a` — NULL. После переноса `a` — ошибка «используется после move» (как после `drop`, с объединением веток); `a = …` (и `a = move(a)`) снова делает имя пригодным. Аргумент — имя string-переменной/параметра; `move` внешней переменной в цикле/функции — ошибка; `move a` — синтаксическая ошибка «ожидалась ';'».
- Тесты: `tests/memory-safety.sh` `string-move-release` (LeakSanitizer), `test.sh` `string-move`, `string-use-after-move`, `string-move-after-branch`, `string-move-without-parens`, `string-move-in-loop`, `string-move-not-name`. До правки `move` — «неизвестная функция 'move'».
- Задача 0б закрыта (D-014, D-015).

## 2026-11-13 — структуры, шаг 1 (1.2): возврат структуры
- `fn Point make(…)` возвращает структуру, если в ней и во вложенных структурах только скалярные поля; `Point q = make(…);`, `q = make(…);`, `sum(make(…))`, `shift(shift(a, 1), 1)`. `Point w = q;` — копия (раньше разбор ждал только `= {`). Конец функции без `return` даёт нулевое значение любого типа (`Constant::getNullValue`).
- Структура с полем `string`/`ptr`/динамический массив, в том числе во вложенной структуре: возврат — ошибка «пока нельзя вернуть из функции», `P w = q;` и `w = q;` — «неявное копирование … запрещено» (раньше вложенное поле не проверялось). Правила владеющих полей — 5.2.
- Тесты: `test.sh` `struct-return`, `struct-return-owning-field`, `struct-return-wrong-type`, `struct-init-wrong-type`, `struct-result-into-int`, `struct-copy-nested-owning`, `struct-init-copy-owning`; пример и ошибка в spec §7. До правки `fn Point make` — «неизвестный тип результата 'Point'», `Point w = q;` — «ожидалась ';'».
- В письмо записаны баг `ptr q = v` (из PR #110) и решение 13.11 о `data(временная строка)`.

## 2026-11-13 — структуры, шаг 2 (1.2, D-016): строки в полях
- Решение владельца 13.11: структуры «с любыми полями, как в C++ и Rust» раньше массива структур; копия — полная, перенос — `move(q)` (D-016).
- Строка в поле принадлежит структуре: литерал и переменная копируются, результат `str()`/`replace`/вызова забирается без копии, `p.name = …` и `p.tag.label = …` освобождают прежнюю строку; поле `string` без значения — пустая строка. Строки полей, в том числе во вложенных структурах, освобождаются в конце блока переменной, параметра (своя копия) и глобальной структуры — на `return`, `break`, `continue` и в ветках.
- `P w = q;`, `w = q;` (и `c = c`), `c.tag = a.tag;`, `{ tag = t; }`, передача в функцию по значению — полная копия: изменение копии не меняет исходник, двойного освобождения нет. ptr-поле копирует адрес, как ptr-переменная (раньше копия такой структуры была ошибкой «неявное копирование … запрещено»).
- Вне задачи, найдено и исправлено попутно (нужно для теста неверного типа поля): `s.name = 5;` для string-поля проходил проверку типов (литерал получал тип поля) и падал при выполнении (segfault на main); теперь «поле ожидает тип 'str', получен 'int/i32'».
- Возврат структуры со строкой из функции пока ошибка (следующий шаг).
- Тесты: `tests/memory-safety.sh` `struct-string-fields-release` (LeakSanitizer + ASan: копии, вложенные поля, параметр, цикл с `break`/`continue`, ранний `return`, глобальные); `test.sh` `struct-string-field-copy`, `struct-ptr-field-copy`, `struct-copy-ptr-field` (бывший отрицательный пример `p0_struct_copy_ownership.svx`), `struct-string-field-wrong-type`; удалены отрицательные `struct-copy-nested-owning`, `struct-init-copy-owning`, `struct-copy-ownership` (правило изменено владельцем).

## 2026-11-13 — структуры, шаг 3 (1.2, D-016): возврат и move структуры
- `fn Person make(…)` возвращает структуру со строками (в том числе во вложенных структурах): `return p;` для локальной переменной отдаёт её без копии и без освобождения, глобальная структура и поле (`return p.tag;`) — копией. Раньше — ошибка «пока нельзя вернуть из функции».
- `Person b = move(a);`, `move(a)` в аргументе и в `return` — перенос структуры целиком; после него `a` нельзя использовать до нового присваивания, `move` внутри цикла переменной снаружи — ошибка (те же проверки, что для строк, D-015). `Point q = move(p);` для скалярной структуры тоже разрешён.
- Результат функции-структуры, не положенный в переменную (`size(make(5))`, `make(3);`, `c.tag = tagOf(a);`), освобождается в конце оператора.
- Тесты: `tests/memory-safety.sh` `struct-return-move-release` (LeakSanitizer + ASan: возврат из ветки, параметр, глобальная, временные, цикл с `continue`, move); `test.sh` `struct-return-string-field`, `struct-use-after-move`, `struct-move-in-loop`; удалён отрицательный `struct-return-owning-field` (правило снято).

## 2026-11-13 — структуры, шаг 4 (1.2, D-017): массив в поле
- Поле `int[] scores;` (элементы — числа, `bool`, `char`) — растущий массив внутри структуры, сначала пустой: `p.scores.push(x)`, `pop()`, `resize(n)`, `len()`, `cap()`, `p.scores[i]` чтение и запись (`+=`), в том числе во вложенной структуре (`t.s.scores.push(1)`). Раньше `int[]` в поле — синтаксическая ошибка.
- Массив освобождается вместе со структурой на всех выходах; `P w = q;`, `w = q;`, передача по значению — своя копия буфера; `move(q)`, `return t` — перенос; временный результат `total(make(3))` освобождается.
- Два слова (решение владельца, D-017): `p.scores.clear();` (как в C++) — поле пустое, `push` работает, чтение элемента — ошибка выполнения, повтор безопасен; `clear()` есть и у массивов-переменных. `drop p.scores;` (как в Rust) — до `p.scores = [..]` поле и копия/передача/возврат/move всей структуры — ошибка компиляции, повторный `drop` — ошибка.
- Ошибки: `p.scores = 5` («присваивается только список»), использование после `drop`, повторный `drop`, копия структуры со сброшенным полем, `drop` поля в цикле, `ptr q = p.scores` («целиком не берётся»), `string[] a;` в поле, неверный тип в `push`, значение по умолчанию у поля-массива.
- Не сделано: `for x in p.scores`, массив строк или структур в поле.
- Тесты: `tests/memory-safety.sh` `struct-array-field-release` (LeakSanitizer + ASan), `struct-array-field-cleared-read` (ошибка выполнения); `test.sh` `struct-array-field`, `struct-array-field-assign`, `-use-after-drop`, `-double-drop`, `-copy-dropped`, `-drop-in-loop`, `struct-array-field-whole`, `struct-array-field-element-type`, `struct-array-field-push-type`.

## 2026-11-13 — структуры, шаг 5 (1.2): массив структур
- Статический массив структур: `P[] a = [{ … }, make(5), q];` (литерал, результат функции, копия переменной), `a[i].x` и `a[i].x.y` чтение, `a[i].x = v` / `+=` (индекс вычисляется один раз), `a[i] = make(7);`, `P r = a[i];`, `for p in a` (копия элемента), `a.len()`, проверка индекса во время выполнения.
- Элементы со строками и массивами в полях (D-016/D-017): освобождаются вместе с массивом (локальным и глобальным), `a[i] = q` освобождает старый элемент и копирует `q`, `a[i].name = "x"` освобождает старую строку, `for p in a` — своя копия на каждом шаге (в т.ч. при `continue`/`break`).
- Ошибки: неизвестное поле, неверный тип поля, элемент другого типа, поле у элемента `int[]`, константный индекс за границей, `move(a[i])`.
- Не сделано: `a[i].v.push(x)` (методы поля-массива у элемента), динамический массив структур.
- Тесты: `test.sh` `struct-array`, `struct-array-owning`, `struct-array-field-oob`, `struct-array-move-element` и др.; `tests/memory-safety.sh` `struct-array-owning-release` (LeakSanitizer + ASan).

## 2026-11-13 — структуры, шаг 6 (1.2): задача 11 «набор людей», задача 1.2 закрыта
- Пример spec §7 «Задача 11»: набор людей одним массивом структур (`string name`, вложенная `Address home`, `int[] scores`), без параллельных массивов: функция возвращает запись, элемент передаётся в функцию (копия) и заменяется её результатом, поиск по имени, изменение вложенного поля, полная копия элемента, перебор `for p in people`.
- Ошибка: неверный тип вложенного поля элемента (`people[0].home.zip = "Tver"`).
- Тесты: пример spec (через `tests/spec-examples.sh`), `tests/memory-safety.sh` `task11-people-release` (LeakSanitizer + ASan).
- Не вошло в 1.2: `a[i].v.push(x)`, динамический массив структур, `for x in p.scores`, строки/структуры в элементах поля-массива, `make(1).x`.

## 2026-11-13 — хвосты диагностики (1.7), шаг 1: позиция пропущенной `;`
- Лексер запоминает конец каждого токена (`Token::endLine/endColumn`); если `;` пропущена, а следующий токен на другой строке или это конец файла, ошибка ставится сразу после последнего токена: `int x = 0` → `1:10: ожидалась ';' после '0'` (было `2:1: … получено 'print'`). На той же строке — как раньше.
- Тесты `test.sh` `missing-semicolon-*` (следующая строка, комментарий и пустая строка между, конец файла, та же строка) сверяют позицию и текст и что ошибка ровно одна. Spec §1.

## 2026-11-13 — хвосты диагностики (1.7), шаг 2: цепочка сравнений; задача 1.7 закрыта
- `1 < 2 < 3`, `0 < x < 10`, `1 == 2 == 3` остаются ошибкой (как в Rust), но текст понятный: «сравнения нельзя соединять в цепочку (a < b < c): результат сравнения — bool, а не число; пишите a < b && b < c» у второго оператора (`semantic.cpp`, было «оператор сравнения принимает только числа»).
- Без ложной второй ошибки: переменная с инициализатором считается инициализированной, даже если в инициализаторе ошибка (было: `bool b = 1 < 2 < 3; print(b);` давало ещё «'b' используется до инициализации»).
- Тесты `test.sh` `comparison-chain-*` сверяют строку/столбец, текст и что ошибка ровно одна. Spec §4.

## 2026-11-13 — растущий массив (1.3), шаг 1: массив-переменная освобождается сам
- Решение владельца D-018: копия при передаче, `move(a)` — перенос, автоосвобождение в конце блока, возврат локального без копии.
- `ptr v = new T[n]` освобождается в конце блока на всех выходах (`codegen.cpp`: описатель в `deferred_`); `drop v`/`del v` раньше обнуляют буфер (`del v` был без обнуления).
- Тесты: `tests/memory-safety.sh` `dynamic-auto-release` (LeakSanitizer; до правки — утечка), `test.sh` `dynamic-array-auto-release`, `dynamic-array-del-then-drop`. Spec §8, §10.
- Исправлено (без него `del v` + `v = new int[1]` ломалось): `v = new T[n]` теперь даёт новый буфер с нулями и `len`/`cap` = n, старый освобождается (было: сырой malloc без нулей, старая длина, утечка). Тесты `dynamic-array-new-reassign`, `dynamic-new-reassign`.
- В письмо: баг `int u;` («ожидалась '='», из PR #121).

## 2026-11-13 — растущий массив (1.3), шаг 2: массив в функцию и из функции (D-018)
- Параметр `int[] v` — копия (как `vector` в C++), обычный массив — тоже копией; `move(a)` — перенос, `a` нельзя до присваивания (как в Rust); `fn int[] f()` — локальный/параметр возвращается без копии, глобальный — копией; `int[] b = a`/`b = a` — копия, `int[] b = move(a)`, `ptr b = f()`, `int[] v = new int[n]`; результат, не положенный в переменную, освобождается в конце оператора.
- Ошибки: «ожидался массив 'int[]', получен массив 'byte[]'», «return: ожидался массив …», параметр `ptr` — с подсказкой `int[] v`; `fn ptr` с `return v` — подсказка `fn int[]`.
- Тесты: `test.sh` `array-function-copy-move`, `array-use-after-move`, `array-argument-element`, `array-return-type`, `array-param-type`; `memory-safety.sh` `array-function-release` (LeakSanitizer). Spec §8, §17.

## 2026-11-13 — растущий массив (1.3), шаг 3: растущий массив структур
- `P[] a = new P[n]` — элементы со значениями полей по умолчанию; `push(q)` (копия) / `push(f())` (без копии), `a[i].x`, `a[i] = q`, `P r = a[i]`, `resize`/`clear`, передача/возврат/`move`/копия массива — со строками и массивами в полях, без утечек (LeakSanitizer).
- Размер элемента теперь из DataLayout (`elementSize`), а не `getPrimitiveSizeInBits/8` (для структуры давал 0).
- `pop()` у массива структур — ошибка с подсказкой (пока нет). Тесты `struct-dynamic-array`, `-pop`, `-push-type`, `memory-safety.sh` `struct-dynamic-array-release`. Spec §7, §17.

## 2026-11-13 — растущий массив (1.3), шаг 4: задачи 11, 13, 14, 20 через функции
- Программы `examples/task11_people_grow.svx`, `task13_stack_queue.svx`, `task14_matrix.svx`, `task20_grow.svx`: массивы передаются в функции и возвращаются (`x = f(move(x))`), копия до изменения остаётся прежней; вывод проверяет `test.sh`, отсутствие утечек — `tests/memory-safety.sh` (LeakSanitizer). Ошибки: чужой тип массива в аргументе, имя после move.
- `docs/tasks.md` — список 30 типовых задач из исходного запроса владельца; ссылка из `docs/handoff.md`.
- Найдено (не сделано, в письме и spec §17): `a[i].v[j]`, литерал-массив в аргументе, `x = f(move(x))` в цикле.
