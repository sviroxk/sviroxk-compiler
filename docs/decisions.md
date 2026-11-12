# Технические решения SC

## D-001. Синтаксис стандартной библиотеки и namespace imports

**Статус:** временное решение, текущая реализация.

**Варианты:**
1. Ввести сразу `use math` и квалифицированные `math.abs`.
2. Использовать относительный `imp "path/file.svx"` и именованные функции с префиксом модуля.

**Выбор:** вариант 2 до появления per-module AST/symbol tables. Сейчас `imp` рекурсивно склеивает токены в общий compilation unit, поэтому `use`/qualified lookup потребовали бы поверхностной подстановки имён и создали бы конфликты. Путь и API явно отражают реальное поведение: `stdlib/math/math.svx`, `math_abs(...)`.

**Последствия:** старый синтаксис сохраняется; namespace call syntax отложен до реализации symbol resolution. Не документировать `use math` как поддерживаемый API.

## D-002. Возврат результата `rep`

**Статус:** текущий legacy-контракт.

**Выбор:** `rep(source, needle, replacement)` заменяет первое вхождение, выделяет новый `ptr`; вызывающий обязан вызвать `del`. На allocation failure/size overflow backend сейчас завершает процесс через `abort`.

**Причина:** в текущем языке нет string-with-length и Result/OOM error contract. Fixed-size stack buffer был небезопасен; heap result без скрытого владения хотя бы имеет явный release.

**Риски/следующий шаг:** утечки возможны при забытом `del`; recoverable allocation error и owning string откладываются до общей модели ресурсов.

## D-003. Логические операторы

**Статус:** LLVM short-circuit CFG.

`&&` и `||` генерируются как условный переход с merge/PHI, чтобы не вычислять правый операнд без необходимости. Проверяется side-effect regression. Сейчас условия принимают bool и integer truthy/falsy; строгая bool-only policy отложена.

## D-004. Bounds checks

**Статус:** runtime check только для статических массивов.

Индекс проверяется перед inbounds GEP; ошибка печатает source line/column и завершает программу ненулевым кодом. Dynamic `ptr` arrays не хранят length, поэтому безопасная проверка невозможна без descriptor/allocator redesign.


## D-005. Перенос структур по значению из PR22 (шаг 4б)

**Источник поведения:** закрытый PR22 `edcaacf`, stage 5 `5cfce47`.
Параметр `tip Name value` — aggregate copy в локальный слот функции; скалярные
и вложенные value-поля независимы от исходной переменной. `tip Inner field`
добавлен как историческая запись типа вложенного поля; bare `Inner field` сохранён.
Типы структур регистрируются до LLVM function signatures, параметр получает
metadata для lookup полей. Существующий load aggregate переменной уже подходит
для аргумента, поэтому лишняя временная materialization из PR22 не требуется.
Это не ownership ABI: pointer-поля копируют адрес, не выделение; clone/move,
struct returns, `defer`/`drop` не входят в шаг. Новых решений о памяти не принято.
