# Очередь работ SC

Порядок — по приоритету и зависимостям. Не отмечать выполненным без тестов.

## P0 — корректность/безопасность
- [x] Состояние parser принадлежит одному разбору; `ParseResult` хранит дерево и статус ошибок. Копии `Node` удерживают expression subtrees через `shared_ptr` без глобальной arena; lifetime и повторные разборы покрыты API regression под ASan/UBSan.
- [~] ASan/UBSan LeakSanitizer проверяет parser arena, heap result `rep` и static array bounds. Динамические pointer/file/memory safety cases требуют runtime metadata.
- [x] Исправить compiler warnings под `-Wall -Wextra -Werror`; строгая сборка проходит локально и в CI.
- [x] Проверять LLVM module verifier до печати IR; некорректный IR возвращает internal diagnostic и exit code 3.
- [~] Прямые numeric literals во всех expression contexts проверяются до codegen. Остаются checked casts для вычисляемых значений и точное signed/unsigned conversion semantics.
- [~] Bounds checks для статических массивов проверены; dynamic arrays ещё не имеют length metadata.

## P1 — зрелость compiler
- [x] Short-circuit evaluation для `&&`/`||` реализована через LLVM CFG; side-effect regression покрывает пропуск правого операнда.
- [~] Shared diagnostics показывают source excerpt/caret; parser собирает несколько syntax errors с синхронизацией по `;`/`}`; lexer recovery и richer diagnostic codes ещё отсутствуют.
- [ ] Проверка definite assignment и return analysis для вложенных control-flow.
- [ ] Корректность LLVM IR при ранних return, вложенных циклах, break/continue.
- [ ] Namespace modules, visibility, stable symbol resolution.

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
