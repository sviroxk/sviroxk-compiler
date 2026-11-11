# Memory-safety проверки

`./test.sh` после успешного ASan probe запускает `tests/memory-safety.sh`.
Новых зависимостей нет: используются тот же clang и LLVM IR с `sanitize_address`.
Если clang не умеет ASan, сохраняется существующий явный `SKIP`.

## Что именно проверяется

| Режим | Случаи | Успех |
|---|---|---|
| clean | heap `rep`/`del`, рост dynamic array с сохранением данных и `pop` statement, пустой array/`del`, file write/read/EOF/close | Точный stdout, exit 0, пустой stderr |
| runtime | отрицательный dynamic index при чтении, index == length при записи, отсутствующий файл | SC runtime diagnostic, exit 1, без sanitizer report |
| detector | чтение dynamic array после `del`, повторный `del`, выход за выделение через raw ptr | Только ожидаемый класс AddressSanitizer и exit 86 |

Все исходники, бинарники и создаваемые файлы изолированы во временных каталогах.
Компиляция и каждый запуск ограничены таймаутом. Signal/crash/timeout без нужного
sanitizer report никогда не считается успешной отрицательной проверкой.
У 3 корректных heap lifecycle cases включён `detect_leaks=1`.
У file read и аварийных завершений — `detect_leaks=0`: строки `pro()` пока нельзя
освободить через `del`, а аварийный exit не выполняет cleanup. Это исключение
не распространяется на compiler/API tests под ASan/UBSan/LeakSanitizer.

## Это не новая модель памяти

Режим detector намеренно запускает небезопасные программы, чтобы доказать,
что ASan действительно инструментирует IR (в том числе reads, а не только free).
Он не делает UAF/double-free/raw OOB допустимым поведением SC. Когда появятся
checked pointers, self-tests нужно заменить ожиданием контролируемой диагностики.
Без ASan эти программы запускать не следует.

Распознавание dangling aliases, чтения/записи после закрытия файла, защита от
повторного close, owning strings и cleanup при выходе требуют отдельного
ownership/resource contract и метаданных. Эти гарантии здесь не добавлены.
ASan также не доказывает отсутствие утечек файловых дескрипторов или всех ошибок
внутри системной libc. UBSan проверяет C++ компилятор, но source-level UB checks
для арифметики SC нельзя получить простым линкованием generated IR с UBSan.

Выражение `v.pop()` после исправления PHI дополнительно покрыто 4 regression
cases в `test.sh`: рост/пустой массив, LIFO/длина, вложенный control-flow и
floating-point zero. Они также входят в общий ASan-прогон программ. Самостоятельный
`v.pop();` по-прежнему проверяется в memory-safety case динамического роста.
