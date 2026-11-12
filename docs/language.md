# SC language reference — implemented subset

Эта страница описывает только поведение, присутствующее в компиляторе текущей версии. Это не полная спецификация запланированного языка.

## Совместимость

Текущая версия `0.3.0` — экспериментальный прототип, поэтому стабильность source/ABI не гарантируется. Расширение существующего синтаксиса обратно совместимо, если не указано в changelog. Legacy-ключевые слова пока поддерживаются; политика deprecated warnings ещё не реализована, ни одна конструкция здесь не объявляется deprecated. До `1.0.0` изменение синтаксиса может происходить в minor-релизе с тестами и записью в `CHANGELOG.md`.

## EBNF фрагмент

Грамматика ниже намеренно ограничена реально принимаемыми конструкциями; детали лексических токенов см. в lexer. Терминал `identifier` означает лексически распознанное имя, `number` — decimal/exponent literal, `string` — строковый literal, а `type` — реально известный scalar type alias.

```ebnf
program       = { declaration | statement } ;
declaration   = variable-decl | function-decl | struct-decl | import-decl ;
variable-decl = "let", identifier, ":", type, "=", expression, ";"
              | type, identifier, "=", expression, ";"
              | "svi", type, "lor", identifier, "=", expression, ";" ;
function-decl = "fn", [ "void" | type ], identifier, "(", [ parameters ], ")", block
              | "fucn", [ type ], identifier, "(", [ legacy-parameters ], ")", block ;
parameters    = parameter, { ",", parameter } ;
parameter     = (type | "tip", identifier), identifier ;
legacy-parameters = parameter, { ",", parameter } ;
struct-decl   = "tip", identifier, "{", { field-decl }, "}" ;
field-decl    = (type | identifier | "tip", identifier), identifier,
                [ "=", expression ], ";" ;
import-decl   = "imp", string, ";" ;
block         = "{", { statement }, "}" ;
statement     = variable-decl | assignment | return-stmt | print-stmt
              | if-stmt | loop-stmt | break-stmt | continue-stmt
              | function-call, ";" ;
assignment    = identifier, "=", expression, ";"
              | identifier, "[", expression, "]", "=", expression, ";" ;
return-stmt   = ("return" | "rev"), expression, ";" ;
print-stmt    = ("print" | "slov"), "(", expression, ")", ";" ;
if-stmt       = ("if" | "eles"), "(", expression, ")", block,
                [ ("else" | "sele"), block ] ;
loop-stmt     = "cert", ("usy" | "ceh"), "(", expression, ")", block ;
break-stmt    = ("break" | "brk"), ";" ;
continue-stmt = ("continue" | "cont"), ";" ;
expression    = logical-or ;
logical-or    = logical-and, { "||", logical-and } ;
logical-and   = comparison, { "&&", comparison } ;
comparison    = additive, [ ("<" | ">" | "<=" | ">=" | "==" | "!="), additive ] ;
additive      = multiplicative, { ("+" | "-"), multiplicative } ;
multiplicative= unary, { ("*" | "/" | "%"), unary } ;
unary         = [ "!" | "-" ], primary ;
primary       = number | string | "true" | "false" | identifier
              | function-call | identifier, "[", expression, "]"
              | "(", expression, ")" | cast ;
function-call = identifier, "(", [ expression, { ",", expression } ], ")" ;
cast          = "(", type, ")", unary ;
type          = "int" | "uint" | "short" | "ushort" | "long" | "ulong"
              | "byte" | "float" | "double" | "bool" | "str"
              | "i8" | "i16" | "i32" | "i64"
              | "u8" | "u16" | "u32" | "u64" | "f32" | "f64" ;
```

EBNF — удобочитаемая схема покрытия, не формальная грамматика всех исторических legacy-форм. Conformance fixtures находятся в `docs/conformance/` и автоматизированы `test.sh`.

## Типы и семантика

- Алиасы: `int/i32`, `uint/u32`, `short/i16`, `ushort/u16`, `long/i64`, `ulong/u64`, `byte/u8`, `float/f32`, `double/f64`, `str` и `bool`.
- Прямые integer/float literals проверяются semantic pass до LLVM lowering; поддержана десятичная точка и `e/E` exponent. Неявный float-literal-to-integer запрещён; для преобразования нужен явный cast, например `(int) 3.0`.
- Присваивания и вызовы проверяют типы. Безопасные widening rules ограничены текущей реализацией; mixed signed/unsigned arithmetic и comparisons отклоняются.
- Узкая integer арифметика продвигается к 32 битам. Для полных правил signedness см. `README.md` и тесты `examples/fixed_width.svx`.
- `&&`/`||` выполняются short-circuit; целочисленные условия интерпретируются как false при нуле и true иначе.
- Статические массивы `svi type[] lor name` проверяют отрицательные и верхние индексы в runtime. Heap arrays и raw pointers пока не имеют length metadata/bounds safety.
- Относительные `imp "path.svx";` раскрывают исходники рекурсивно; проверяются отсутствующий файл и циклы. Namespaces, `use`, `pub` и изоляция символов отсутствуют.
- Функции без явного return type используют текущую legacy-семантику `int`; `fn void name(...)` объявляет процедуру.
- Диагностика semantic errors имеет форму `file:line:column: error[semantic]`. Parser сообщает синтаксическую ошибку и останавливается на первой ошибке.

## Структуры в параметрах функций (шаг 4б PR22)

```sc
tip Pair { int x = 0; int y = 0; }
fn int sum(tip Pair value) { return value.x + value.y; }
svi tip Pair lor point = { x = 3; y = 4; };
print(sum(point)); // 7
```

Параметр `tip Pair value` получает копию aggregate по значению. Изменение
скалярного поля параметра не меняет исходную структуру. Вложенный aggregate
копируется так же: доступны чтение `value.inner.x` и замена `value.inner` целиком.
В определении вложенного поля принимаются `Inner inner` и `tip Inner inner`;
вложенный тип пока нужно определить раньше содержащего. Объявления функций
без тела поддерживают те же типы параметров и проверяют совпадение сигнатур.
Проверенный пример изменения копии: `examples/struct_by_value.svx`.

Это перенос поведения PR22, не новая модель владения: pointer-поля не клонируют
свою heap-память, автоматического освобождения/передачи владения нет. Существующий
запрет присваивания owning-структур не снят. Возврат структур, анонимные struct
literals как аргументы, `defer` и `drop` этим шагом не добавлены.

## Пока не специфицировано как поддерживаемое

Generics, enum, nullable/Option/Result, const immutability, owning typed pointers, точные массивы/поля struct, short-circuit отдельного error model, bounds metadata динамических массивов, overflow checks вычисляемых выражений, многопоточность, FFI, форматтер, LSP, раздельная компиляция и self-hosting. Не полагайтесь на эти возможности.
