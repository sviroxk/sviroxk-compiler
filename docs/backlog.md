# Очередь работ SC

Порядок — по приоритету и зависимостям. Не отмечать выполненным без тестов.

## План переноса PR22 (по указанию владельца)
- [x] 4а — объявления функций без тела (#27).
- [x] 4б — структуры по значению: `struct Name` параметры, `Inner field`, preregistration LLVM types, lookup полей параметра. 4 output cases + 6 semantic errors проверены; `examples/struct_by_value.svx`.
- [x] 4в — `defer`: LIFO, ранний return, if/else, все формы циклов, break/continue и lexical bindings. 8 output + 8 diagnostic cases; временно только обычные операции/вызовы (D-006).
- [x] Техническая уборка между 4в/4г: README с проверенными примерами и авторами, каноническая Apache 2.0, fixtures в tests/examples, локальная .vscode исключена.
- [ ] 4г — `drop`: при переносе исправить double-drop и чтение после drop из PR22.
- Checked casts отложены владельцем; поведение при выходе за диапазон пока не выбиралось.

## P0 — корректность/безопасность
- [x] Состояние parser принадлежит одному разбору; `ParseResult` хранит дерево и статус ошибок. Копии `Node` удерживают expression subtrees через `shared_ptr` без глобальной arena; lifetime и повторные разборы покрыты API regression под ASan/UBSan.
- [~] ASan/UBSan/LeakSanitizer проверяет компилятор и копии AST; ASan сгенерированных программ дополнен 10 memory-safety cases (heap/del, рост и границы dynamic arrays, files/EOF, detector self-tests UAF/double-free/raw OOB). Для 3 корректных heap lifecycle cases включён LeakSanitizer. Контролируемая диагностика alias/UAF/double-free/closed-file и автоматическое освобождение строк требуют runtime metadata/ownership contract и остаются открытыми. См. `docs/memory-safety-tests.md`.
- [x] Исправить compiler warnings под `-Wall -Wextra -Werror`; строгая сборка проходит локально и в CI.
- [x] Проверять LLVM module verifier до печати IR; некорректный IR возвращает internal diagnostic и exit code 3.
- [~] Прямые numeric literals во всех expression contexts проверяются до codegen. Остаются checked casts для вычисляемых значений и точное signed/unsigned conversion semantics.
- [~] Bounds checks статических массивов и dynamic arrays с дескриптором проверены, включая read/write под ASan. Raw ptr и aliases без length metadata пока не имеют безопасной проверки границ.

## P1 — зрелость compiler
- [x] Short-circuit evaluation для `&&`/`||` реализована через LLVM CFG; side-effect regression покрывает пропуск правого операнда.
- [~] Shared diagnostics показывают source excerpt/caret; parser собирает несколько syntax errors с синхронизацией по `;`/`}`; lexer recovery и richer diagnostic codes ещё отсутствуют.
- [ ] Проверка definite assignment и return analysis для вложенных control-flow.
- [ ] Корректность LLVM IR при ранних return, вложенных циклах, break/continue.
  - [x] Исправлен PHI в выражении `pop()`: `ptr v = new int[0]; v.push(1); print(v.pop());` теперь печатает `1` вместо compiler exit 3. Проверены рост, пустой массив, LIFO/длина, несколько pop в выражении, if и floating-point zero; 4 regression cases входят в обычный и ASan-прогон.
- [ ] Namespace modules, visibility, stable symbol resolution (имена модулей для своих библиотек; см. «Библиотеки» ниже и D-001).

## Библиотеки (решение владельца 2026-11-12; после аудита, C++20 и рефакторинга ядра)
- [ ] Вызов внешнего кода: объявление внешней функции C (набросок `extern fn int puts(ptr s);`) и способ указать, с чем линковать. Даёт libc, SDL, raylib, sqlite, curl; библиотеки C++ — через тонкую обёртку `extern "C"`. Классы и шаблоны C++ напрямую не поддерживаются.
- [ ] Свои библиотеки как механизм: имена модулей вместо склейки токенов (набросок `import "mylib/vec.svx" as vec; vec.length(v)`), видимость, поиск по папке.
- Синтаксис решает владелец: сначала дизайн-документ с 2–3 вариантами, реализация — после ответа. Новые библиотеки (содержимое stdlib) пока не писать.

## P2 — библиотека и инструментальная цепочка
- [~] Math module: `math_abs` безопасно обрабатывает INT_MIN через i64 return; min/max/is_even принимают i64. Остальные math API ещё отсутствуют.
- [ ] UTF-8 string representation and safe formatting.
- [ ] `fs`/`io` errors and resource cleanup.
- [~] GitHub Actions workflow добавлен для warnings, tests и sanitizers; проверить реальный remote run после push.
- [~] EBNF implemented-subset spec, changelog, compatibility policy добавлены; завершить полную спецификацию и deprecation warning mechanism.

## P3 — долгосрочно
- [ ] Typed aggregate storage (arrays/structs), safe typed pointers.
- [ ] Memory ownership/resource model with documentation.
- [ ] LLVM optimization pipeline and reproducible benchmarks.
- [ ] Generic type infrastructure, then collections.
- [ ] VS Code tooling / LSP after parser and diagnostics API stabilize.
- [ ] Self-hosting bootstrap.

Не входят в scope: игры и игровой движок.
