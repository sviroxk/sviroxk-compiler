#include <climits>
#include <deque>
#include <unordered_map>
#include <optional>
#include <cctype>
#include "semantic.h"
#include "diagnostics.h"
#include "type.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <unordered_set>
#include <sstream>
#include <utility>
#include <vector>

namespace {

struct Symbol {
    std::string type;            // нормализованное имя (канонический ключ)
    sc::Type ty;                 // единое внутреннее представление типа
    int line;
    int column;
    bool isConst = false;
    bool isInitialized = false;  // definite assignment tracking
    // For dynamic arrays: track length and capacity
    bool isDynArray = false;
    std::string dynArrayElementType;
    // Для ptr p = &x: тип x, его возвращает *p.
    std::string pointee;
    int dynArrayLength = 0;   // current length (for bounds checking)
    int dynArrayCapacity = 0; // allocated capacity
    int droppedLine = 0;      // строка drop/close; дальше имя использовать нельзя
    bool pointsToLocal = false; // ptr p = &x, где x — локальная функции (возвращать нельзя)
    bool pointsToVariable = false; // ptr p = &x (или копия такого ptr): память не из new, del нельзя
    const char* releasedBy = "drop"; // чем освобождено: "drop" или "close" (для текста ошибки)
    int deferDropLine = 0;    // строка defer drop; освобождение при выходе из области
    std::map<std::string, int> droppedFields; // поле-массив после drop t.scores: путь -> строка (D-017)
};

// Тип поля по пути "a.b" в структуре (пусто — нет такого поля).
static std::string fieldTypeAt(const std::map<std::string, const Node*>& structs, std::string structName,
                               const std::string& path) {
    size_t pos = 0;
    for (int depth = 0; depth < 256; ++depth) {
        auto structure = structs.find(structName);
        if (structure == structs.end()) return "";
        const size_t dot = path.find('.', pos);
        const std::string name = path.substr(pos, dot == std::string::npos ? std::string::npos : dot - pos);
        const Node* found = nullptr;
        for (const Node& field : structure->second->children) if (field.varName == name) found = &field;
        if (!found) return "";
        if (dot == std::string::npos) return sc::Type::from(found->varType).toString();
        structName = found->varType;
        pos = dot + 1;
    }
    return "";
}

struct Function {
    std::string returnType;
    std::vector<std::string> parameters;
    sc::Type returnTy;
    int line;
    int column;
    bool isForwardDecl = false;  // есть только объявление, определения пока нет
};

class Checker {
public:
    Checker(const std::string& filename) : filename_(filename) {
        scopes_.emplace_back();
    }

    bool run(const Node& program) {
        for (const Node& node : program.children) {
            if (node.type == NODE_STRUCT_DEF) {
                if (structs_.contains(node.varName))
                    error(node, "повторное объявление структуры '" + node.varName + "'");
                else
                    structs_[node.varName] = &node;
            }
            if (node.type == NODE_FUNC_DEF) {
                Function fn;
                fn.returnType = normalize(node.varType);
                fn.returnTy = sc::Type::from(node.varType);
                fn.line = node.line;
                fn.column = node.column;
                fn.isForwardDecl = node.isForwardDecl;
                for (const std::string& param : node.params) {
                    size_t separator = param.find(':');
                    fn.parameters.push_back(normalize(param.substr(0, separator)));
                }
                if (isBuiltinFunction(node.varName))
                    error(node, "имя функции '" + node.varName + "' зарезервировано встроенной функцией");
                else if (node.varName == "main")
                    error(node, "имя функции 'main' зарезервировано: точка входа программы — код верхнего уровня файла");
                else if (functions_.contains(node.varName))
                    mergeDeclaration(functions_[node.varName], fn, node);
                else {
                    functions_[node.varName] = fn;
                    functionNodes_[node.varName] = &node;
                }
            }
        }
        for (const auto& [name, fn] : functions_)
            if (fn.isForwardDecl)
                error(*functionNodes_[name], "функция '" + name + "' объявлена, но не определена");

        for (const Node& node : program.children) {
            if (node.type != NODE_FUNC_DEF && node.type != NODE_STRUCT_DEF)
                checkNode(node);
        }
        for (const Node& node : program.children) {
            if (node.type == NODE_FUNC_DEF && !node.isForwardDecl)
                checkFunction(node);
            else if (node.type == NODE_STRUCT_DEF)
                checkStruct(node);
        }
        return errors_ == 0;
    }

private:
    std::string filename_;
    mutable unsigned errors_ = 0;
    std::vector<std::map<std::string, Symbol>> scopes_;
    std::map<std::string, Function> functions_;
    std::map<std::string, const Node*> functionNodes_;

    // Второе появление имени функции: объявление + определение (в любом порядке)
    // допустимо, если сигнатуры совпадают; два определения или два объявления — ошибка.
    void mergeDeclaration(Function& existing, const Function& fn, const Node& node) {
        const std::string& name = node.varName;
        if (existing.isForwardDecl == fn.isForwardDecl) {
            error(node, fn.isForwardDecl
                ? "повторное объявление функции '" + name + "'"
                : "повторное определение функции '" + name + "'");
            return;
        }
        if (existing.returnType != fn.returnType ||
            existing.parameters != fn.parameters) {
            error(node, "сигнатура функции '" + name +
                  "' не совпадает с её объявлением в строке " +
                  std::to_string(existing.line) + ": " + signature(existing) +
                  " и " + signature(fn));
            return;
        }
        if (existing.isForwardDecl) {
            existing = fn;
            functionNodes_[name] = &node;
        }
    }

    static std::string signature(const Function& fn) {
        std::string text = fn.returnType + "(";
        for (size_t i = 0; i < fn.parameters.size(); ++i)
            text += (i ? ", " : "") + fn.parameters[i];
        return text + ")";
    }
    std::map<std::string, const Node*> structs_;
    mutable std::deque<Symbol> fieldArrays_;  // символы полей-массивов p.scores (D-016)
    std::vector<std::string> returnTypes_;
    unsigned loopDepth_ = 0;
    size_t functionScope_ = 0; // индекс области параметров текущей функции
    size_t dropBarrier_ = 0;  // drop можно только для имён из областей с индексом >= этого

    static bool isBuiltinFunction(const std::string& name) {
        static const std::set<std::string> names = {
            "sqrt", "abs", "pow", "max", "min", "floor", "ceil", "round",
            "rand", "len", "replace", "str", "data", "as_ptr", "move"
        };
        return names.contains(name);
    }

    static std::string normalize(const std::string& type) {
        return sc::Type::from(type).toString();
    }

    static std::string typeName(const std::string& type) {
        const std::string n = normalize(type);
        if (n.starts_with("dyn<")) {  // растущий массив: int[] (1.3)
            const std::string element = typeName(n.substr(4, n.size() - 5));
            return element.substr(0, element.find('/')) + "[]";
        }
        if (n == "i32") return "int/i32";
        if (n == "u32") return "uint/u32";
        if (n == "i16") return "short/i16";
        if (n == "u16") return "ushort/u16";
        if (n == "i64") return "long/i64";
        if (n == "u64") return "ulong/u64";
        if (n == "u8") return "byte/u8";
        if (n == "f32") return "float/f32";
        if (n == "f64") return "double/f64";
        if (n == "str") return "str";
        if (n.empty()) return "unknown";
        return n;
    }

    static bool literalFits(const std::string& type, const std::string& text) {
        return sc::Type::from(type).fitsLiteral(text);
    }

    void error(const Node& node, const std::string& message) const {
        ++errors_;
        sc::reportDiagnostic(node.file.empty() ? filename_ : node.file,
                             node.line, node.column, "semantic", message);
    }

    // Первое поле string (или динамический массив), в том числе во вложенной
    // структуре: "s" или "inner.s". ptr-поле копирует только адрес (как ptr-переменная).
    std::string owningField(const std::string& structName, int depth = 0) const {
        auto structure = structs_.find(structName);
        if (structure == structs_.end() || depth > 256) return "";
        for (const Node& field : structure->second->children) {
            const sc::Type fieldTy = sc::Type::from(normalize(field.varType));
            if (fieldTy.isDynArray() || fieldTy.isStr()) return field.varName;
            if (fieldTy.isStruct()) {
                const std::string inner = owningField(fieldTy.structName, depth + 1);
                if (!inner.empty()) return field.varName + "." + inner;
            }
        }
        return "";
    }


    bool knownType(const std::string& raw) const {
        const sc::Type t = sc::Type::from(raw);
        if (!t.isKnown()) return false;
        if (t.isDynArray())  // int[] (D-016), растущий массив структур P[] (1.3)
            return t.element && (t.element->isScalar() || (t.element->isStruct() && structs_.contains(t.element->structName)));
        return t.isScalar() || t.isStr() || t.isPtr() || t.isVoid() || t.isFile() ||
               (t.isStruct() && structs_.contains(t.structName));
    }

    static bool isInteger(const std::string& type) {
        return sc::Type::from(type).isInteger();
    }

    static sc::Type ty(const std::string& type) {
        return sc::Type::from(type);
    }

    // Тип, на который указывает ptr: из `ptr p = &x`, иначе i32 (так читает *p).
    // ptr = &x или ptr = q, где q сам указывает на переменную: del такого ptr освобождал бы стек.
    // data(s) / as_ptr(s) — явный адрес символов строки (D-014; как в C++ и Rust).
    static bool isStringAddress(const Node& value) {
        return value.type == NODE_FUNC_CALL && (value.varName == "data" || value.varName == "as_ptr");
    }

    bool pointsToVariable(const Node& value) const {
        if (value.type == NODE_ADDR || isStringAddress(value)) return true;
        if (value.type != NODE_IDENTIFIER) return false;
        const Symbol* source = lookup(value.value);
        return source && source->pointsToVariable;
    }

    static std::string pointeeOf(const Symbol& pointer) {
        return pointer.pointee.empty() ? "i32" : normalize(pointer.pointee);
    }

    // Запись значения через указатель: тип должен подходить к тому, на что он указывает.
    void checkPointeeStore(const Node& at, const std::string& name, const Symbol& pointer,
                           const Node& valueNode, const std::string& value) {
        const std::string element = pointeeOf(pointer);
        if (value != "<error>" && !compatible(element, value) && !constantFits(valueNode, element))
            error(at, "'" + name + "' указывает на тип '" + typeName(element) +
                  "', записать значение типа '" + typeName(value) + "' нельзя" + castHint(element, value));
    }

    // const защищает и содержимое массива: элементы и push/pop/resize (раньше не защищал).
    void rejectConstArray(const Node& node, const Symbol* array) const {
        if (array && array->isConst)
            error(node, "нельзя изменять константный массив '" + node.varName + "'");
    }

    // Выражение-символ по форме: литерал 'a', приведение (char), тернарный из символов.
    static bool charLike(const Node& node) {
        if (node.type == NODE_CHAR) return true;
        if (node.type == NODE_CAST) return node.varType == "char";
        if (node.type == NODE_TERNARY && node.right && node.extra)
            return charLike(*node.right) && charLike(*node.extra);
        return false;
    }

    bool compatible(const std::string& expectedRaw, const std::string& actualRaw) const {
        const sc::Type expected = sc::Type::from(expectedRaw);
        const sc::Type actual = sc::Type::from(actualRaw);
        return expected.compatible(actual);
    }

    bool declare(const std::string& name, const std::string& type,
                 const Node& node, bool isConst = false, bool isInitialized = false,
                 bool isDynArray = false, const std::string& dynArrayElementType = "") {
        auto& scope = scopes_.back();
        // Allow shadowing (variable in inner scope with same name as outer scope)
        // This is a common pattern and not an error
        if (scope.contains(name)) {
            error(node, "повторное объявление '" + name + "' в этой области видимости");
            return false;
        }
        const std::string normalized = normalize(type);
        Symbol sym;
        sym.type = normalized;
        sym.ty = sc::Type::from(normalized);
        sym.line = node.line;
        sym.column = node.column;
        sym.isConst = isConst;
        sym.isInitialized = isInitialized;
        sym.isDynArray = isDynArray;
        sym.dynArrayElementType = dynArrayElementType;
        scope[name] = sym;
        return true;
    }

    const Symbol* lookup(const std::string& name) const {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            auto found = it->find(name);
            if (found != it->end()) return &found->second;
        }
        return nullptr;
    }

    // Использование имени: ошибка, если значение уже освобождено drop.
    // p.scores (D-016): поле-массив как символ динамического массива для проверок методов и индекса.
    const Symbol* fieldArray(const Node& node, const std::string& name, bool checkDropped = true) const {
        const size_t dot = name.find('.');
        const Symbol* root = use(node, name.substr(0, dot));
        if (!root || !root->ty.isStruct()) return nullptr;
        const sc::Type type = sc::Type::from(fieldTypeAt(structs_, root->ty.structName, name.substr(dot + 1)));
        if (!type.isDynArray()) return nullptr;
        if (auto dropped = root->droppedFields.find(name.substr(dot + 1));
            checkDropped && dropped != root->droppedFields.end())
            error(node, "'" + name + "' используется после drop (строка " + std::to_string(dropped->second) + ")");
        Symbol symbol;
        symbol.type = type.toString();
        symbol.isDynArray = true;
        symbol.dynArrayElementType = type.element->toString();
        symbol.isConst = root->isConst;
        symbol.isInitialized = true;
        fieldArrays_.push_back(std::move(symbol));
        return &fieldArrays_.back();
    }

    // 1.3 (D-018): значение растущего массива для параметра, переменной, присваивания и
    // return — имя массива (копия), обычный массив (копия), move(имя) (перенос), результат
    // функции `fn int[] f()`. Тип элемента; пусто — не растущий массив (выражение не проверено).
    std::string arrayValue(const Node& expr) {
        if (expr.type == NODE_IDENTIFIER) {
            const Symbol* symbol = lookup(expr.value);
            if (!symbol || (!symbol->isDynArray && !symbol->ty.isArray())) return "";
            expression(expr);
            return symbol->isDynArray ? normalize(symbol->dynArrayElementType) : symbol->ty.element->toString();
        }
        if (expr.type != NODE_FUNC_CALL) return "";
        if (expr.varName == "move" && expr.args.size() == 1 && expr.args[0].type == NODE_IDENTIFIER) {
            size_t index = 0;
            Symbol* moved = lookupMutable(expr.args[0].value, &index);
            if (!moved || !moved->isDynArray) return "";
            expression(expr.args[0]);
            if (index < dropBarrier_)
                error(expr, "move '" + expr.args[0].value + "' внутри цикла или функции, а объявлена она "
                      "снаружи: перенос выполнился бы несколько раз");
            else if (!moved->droppedLine) {
                moved->droppedLine = expr.line;
                moved->releasedBy = "move";
            }
            return normalize(moved->dynArrayElementType);
        }
        const auto function = functions_.find(expr.varName);
        if (function == functions_.end() || !function->second.returnTy.isDynArray()) return "";
        expression(expr);
        return function->second.returnTy.element->toString();
    }

    bool isArrayResult(const Node& expr) const {
        if (expr.type != NODE_FUNC_CALL) return false;
        if (expr.varName == "move" && expr.args.size() == 1 && expr.args[0].type == NODE_IDENTIFIER) {
            const Symbol* moved = lookup(expr.args[0].value);
            return moved && moved->isDynArray;
        }
        const auto function = functions_.find(expr.varName);
        return function != functions_.end() && function->second.returnTy.isDynArray();
    }

    // Проверка значения для места типа `T[]`: пусто — ошибок нет, иначе текст (без места).
    std::string checkArrayValue(const Node& expr, const std::string& element) {
        const std::string actual = arrayValue(expr);
        if (actual.empty()) {
            const std::string type = expression(expr);
            if (type == "<error>") return "";
            return "ожидался массив '" + typeName("dyn<" + element + ">") + "', получен '" + typeName(type) + "'";
        }
        if (normalize(actual) != normalize(element))
            return "ожидался массив '" + typeName("dyn<" + element + ">") + "', получен массив '" +
                   typeName("dyn<" + actual + ">") + "'";
        return "";
    }

    void droppedFieldError(const Node& node, const std::string& name, const Symbol& root,
                           const std::string& prefix) const {
        for (const auto& [path, line] : root.droppedFields)
            if (path.starts_with(prefix)) {
                error(node, "'" + name + (prefix.empty() ? "" : "." + prefix.substr(0, prefix.size() - 1)) +
                      "' нельзя копировать, передавать, возвращать или переносить: поле '" + name + "." + path +
                      "' освобождено drop (строка " + std::to_string(line) + "), сначала присвойте его заново");
                return;
            }
    }

    const Symbol* use(const Node& node, const std::string& name) const {
        if (name.find('.') != std::string::npos) return fieldArray(node, name);
        const Symbol* symbol = lookup(name);
        if (symbol && symbol->droppedLine)
            error(node, "'" + name + "' используется после " + symbol->releasedBy + " (строка " +
                  std::to_string(symbol->droppedLine) + ")");
        return symbol;
    }

    Symbol* lookupMutable(const std::string& name, size_t* scopeIndex = nullptr) {
        for (size_t i = scopes_.size(); i-- > 0;) {
            auto found = scopes_[i].find(name);
            if (found != scopes_[i].end()) {
                if (scopeIndex) *scopeIndex = i;
                return &found->second;
            }
        }
        return nullptr;
    }

    // Проверка drop; возвращает освобождаемый символ или nullptr при ошибке.
    // close(f) освобождает файл так же, как drop f (D-007): verb — только для текста ошибок.
    Symbol* checkDrop(const Node& node, const std::string& verb = "drop") {
        size_t index = 0;
        if (const size_t dot = node.varName.find('.'); dot != std::string::npos) {  // drop t.scores (D-017)
            if (!fieldArray(node, node.varName, false)) {
                error(node, "drop поля поддерживается только для поля-массива: '" + node.varName + "'");
                return nullptr;
            }
            Symbol* root = lookupMutable(node.varName.substr(0, dot), &index);
            const std::string path = node.varName.substr(dot + 1);
            if (auto dropped = root->droppedFields.find(path); dropped != root->droppedFields.end())
                error(node, "повторный drop '" + node.varName + "' (уже освобождено в строке " +
                      std::to_string(dropped->second) + ")");
            else if (index < dropBarrier_)
                error(node, "drop '" + node.varName + "' внутри цикла или функции, а структура объявлена "
                      "снаружи: освобождение выполнилось бы несколько раз");
            else root->droppedFields[path] = node.line;
            return nullptr;
        }
        Symbol* symbol = lookupMutable(node.varName, &index);
        if (!symbol) { error(node, "неизвестное имя '" + node.varName + "'"); return nullptr; }
        if (symbol->ty.isStr() && verb == "drop") {  // D-014: строка освобождается сама
            error(node, "drop для строки не пишется: '" + node.varName +
                  "' — string, её память освобождается в конце блока");
            return nullptr;
        }
        if (!symbol->isDynArray && !symbol->ty.isFile()) {
            error(node, "drop поддерживается только для динамического массива и file");
            return nullptr;
        }
        if (symbol->droppedLine) {
            error(node, "повторный " + verb + " '" + node.varName + "' (уже освобождено в строке " +
                  std::to_string(symbol->droppedLine) + ")");
            return nullptr;
        }
        if (symbol->deferDropLine) {
            error(node, "повторный " + verb + " '" + node.varName + "' (defer drop в строке " +
                  std::to_string(symbol->deferDropLine) + ")");
            return nullptr;
        }
        if (index < dropBarrier_) {
            error(node, verb + " '" + node.varName + "' внутри цикла или функции, а объявлена она снаружи: "
                  "освобождение выполнилось бы несколько раз");
            return nullptr;
        }
        return symbol;
    }

    // Имя объявлено внутри текущей функции (параметр или локальная)?
    bool isFunctionLocal(const std::string& name) {
        size_t index = 0;
        return !returnTypes_.empty() && lookupMutable(name, &index) && index >= functionScope_;
    }

    std::vector<std::vector<int>> droppedState() const {
        std::vector<std::vector<int>> state;
        for (const auto& scope : scopes_) {
            state.emplace_back();
            for (const auto& entry : scope) state.back().push_back(entry.second.droppedLine);
        }
        return state;
    }

    void setDroppedState(const std::vector<std::vector<int>>& state, bool merge) {
        for (size_t i = 0; i < state.size() && i < scopes_.size(); ++i) {
            size_t j = 0;
            for (auto& entry : scopes_[i]) {
                if (j >= state[i].size()) break;
                const int line = state[i][j++];
                if (!merge) entry.second.droppedLine = line;
                else if (line && !entry.second.droppedLine) entry.second.droppedLine = line;
            }
        }
    }

    // Check definite assignment - error if variable used before initialization
    void checkInitialized(const Node& node, const std::string& name) const {
        const Symbol* symbol = lookup(name);
        if (symbol && !symbol->isInitialized) {
            error(node, "переменная '" + name + "' используется до инициализации");
        }
    }

    // Mark variable as initialized (after successful assignment/declaration with initializer)
    void markInitialized(const std::string& name) {
        for (auto& scope : scopes_) {
            auto it = scope.find(name);
            if (it != scope.end()) {
                it->second.isInitialized = true;
                break;
            }
        }
    }

    // Бинарный оператор проверяется отдельно от остальных узлов: цепочка `a + b + …`
    // рекурсивно проходит только expression → binaryExpression → binary, а большой
    // кадр otherExpression в неё не попадает (предел длины выражения, spec §1).
    std::string expression(const Node& node) {
        if (node.type == NODE_BINOP) return binaryExpression(node);
        return otherExpression(node);
    }

    [[gnu::noinline]] std::string binaryExpression(const Node& node) {
        // Левый операнд проверяется здесь, а не в binary: её кадр большой.
        std::string left = expression(*node.left);
        const std::string result = binary(node, std::move(left));
        if (result != "<error>") checkConstantArithmetic(node, result);
        return result;
    }

    [[gnu::noinline]] std::string otherExpression(const Node& node) {
        switch (node.type) {
        case NODE_NUMBER: {
            const bool floatingLiteral = node.value.find_first_of(".eE") != std::string::npos;
            std::string literalType = !node.varType.empty()
                ? normalize(node.varType)
                : floatingLiteral ? "f64" : "i32";
            if (node.varType.empty() && !floatingLiteral) {
                // spec §2: без контекста литерал — i32, а если не помещается — i64.
                if (literalFits("i32", node.value)) literalType = "i32";
                else if (literalFits("i64", node.value)) literalType = "i64";
                else {
                    error(node, "числовой литерал " + node.value + " не помещается в тип i64 (long)");
                    return "<error>";
                }
            }
            if ((literalType == "f32" || literalType == "f64" || isInteger(literalType)) &&
                !literalFits(literalType, node.value)) {
                error(node, "числовой литерал вне диапазона типа '" + typeName(literalType) + "'");
                return "<error>";
            }
            if (floatingLiteral && isInteger(literalType)) {
                error(node, "float literal нельзя неявно преобразовать в целый тип; используйте явный cast");
                return "<error>";
            }
            return literalType;
        }
        case NODE_CHAR:
            return "u8";
        case NODE_STRING:
            return "str";
        case NODE_IDENTIFIER: {
            const Symbol* symbol = use(node, node.value);
            if (!symbol && node.value == "null") return "ptr";  // нулевой указатель
            if (!symbol) {
                error(node, "неизвестное имя '" + node.value + "'");
                return "<error>";
            }
            checkInitialized(node, node.value);
            if (!symbol->droppedFields.empty())  // D-017: структура со сброшенным полем целиком не уходит
                droppedFieldError(node, node.value, *symbol, "");
            return symbol->type;
        }
        case NODE_STRUCT_FIELD: {
            std::string currentStruct;
            if (node.right) {  // a[i].x: поле элемента массива структур
                Node access;
                access.type = NODE_ARRAY_ACCESS;
                access.varName = node.varName;
                access.left = node.right;
                access.line = node.line; access.column = node.column; access.file = node.file;
                const std::string element = expression(access);
                if (element == "<error>") return "<error>";
                if (!structs_.contains(element)) {
                    error(node, "элемент массива '" + node.varName + "' имеет тип '" + typeName(element) +
                          "' и не имеет полей");
                    return "<error>";
                }
                currentStruct = element;
            } else {
            const Symbol* object = use(node, node.varName);
            if (!object) {
                error(node, "неизвестное имя '" + node.varName + "'");
                return "<error>";
            }
            // Definite assignment: object must be initialized before field access
            checkInitialized(node, node.varName);
            droppedFieldError(node, node.varName, *object, node.op + ".");  // t.inner после drop t.inner.a
            currentStruct = object->ty.structName;
            }
            // Split field path on dots and traverse nested structs
            size_t pos = 0;
            std::string path = node.op;
            while (pos < path.size()) {
                size_t dotPos = path.find('.', pos);
                std::string fieldName = path.substr(pos, dotPos == std::string::npos ? std::string::npos : dotPos - pos);
                auto structure = structs_.find(currentStruct);
                if (structure == structs_.end()) {
                    error(node, "тип '" + typeName(currentStruct) + "' не имеет полей");
                    return "<error>";
                }
                const Node* fieldNode = nullptr;
                for (const Node& field : structure->second->children) {
                    if (field.varName == fieldName) { fieldNode = &field; break; }
                }
                if (!fieldNode) {
                    error(node, "у структуры '" + currentStruct + "' нет поля '" + fieldName + "'");
                    return "<error>";
                }
                std::string fieldType = normalize(fieldNode->varType);
                if (dotPos == std::string::npos) {
                    if (ty(fieldType).isDynArray())  // D-016: копия массива — только вместе со структурой
                        error(node, "поле-массив '" + node.varName + "." + node.op + "' целиком не берётся: "
                              "пишите " + node.varName + "." + node.op + "[i], .len(), .push(x)");
                    return fieldType;
                }
                currentStruct = fieldType;
                pos = dotPos + 1;
            }
            return "<error>";
        }
        case NODE_STRUCT_LITERAL: {
            if (node.varType.empty()) return "<error>";
            auto structure = structs_.find(node.varType);
            if (structure == structs_.end()) return "<error>";
            std::map<std::string, const Node*> named;
            std::vector<const Node*> positional;
            for (const Node& init : node.children) {
                if (init.varName.empty()) positional.push_back(&init);
                else if (!named.emplace(init.varName, &init).second)
                    error(init, "поле '" + init.varName + "' инициализировано дважды");
            }
            size_t position = 0;
            for (const Node& field : structure->second->children) {
                const Node* value = nullptr;
                auto it = named.find(field.varName);
                if (it != named.end()) value = it->second;
                else if (position < positional.size()) value = positional[position++];
                if (value) {
                    std::string actual = expression(*value->left);
                    if (actual != "<error>" && !compatible(field.varType, actual) &&
                        !constantFits(*value->left, field.varType))
                        error(*value, "поле '" + field.varName + "' ожидает тип '" +
                              typeName(normalize(field.varType)) + "', получен '" + typeName(actual) + "'");
                    if (value->left->type == NODE_NUMBER &&
                        !literalFits(normalize(field.varType), value->left->value))
                        error(*value, "литерал поля вне диапазона типа '" + typeName(normalize(field.varType)) + "'");
                }
                if (!value) named.erase(field.varName);
            }
            for (const auto& item : named) {
                bool exists = false;
                for (const Node& field : structure->second->children)
                    if (field.varName == item.first) exists = true;
                if (!exists) error(*item.second, "у структуры нет поля '" + item.first + "'");
            }
            if (position < positional.size()) error(node, "слишком много позиционных значений структуры");
            return node.varType;
        }
        case NODE_ARRAY_ACCESS: {
            const Symbol* symbol = use(node, node.varName);
            if (!symbol) {
                error(node, "неизвестный массив '" + node.varName + "'");
                return "<error>";
            }
            // Definite assignment: array must be initialized before element access
            checkInitialized(node, node.varName);
            std::string indexType = expression(*node.left);
            if (!isInteger(indexType) && indexType != "<error>")
                error(*node.left, "индекс массива должен иметь целочисленный тип");
            // Compile-time bounds check for static arrays with constant integer index
            if (symbol->ty.isArray() && node.left->type == NODE_NUMBER && node.left->varType.empty()) {
                // constantInteger понимает 0x/0b/0o и `_`; stoll читал `0x5` как 0.
                const unsigned len = symbol->ty.arrayLength;
                if (const auto idx = constantInteger(*node.left); idx && (*idx < 0 || *idx >= len))
                    error(*node.left, "индекс массива выходит за границы [" + std::to_string(len) + "]");
            }
            if (symbol->ty.isArray())
                return symbol->ty.element->toString();
            if (symbol->isDynArray)
                return symbol->dynArrayElementType.empty() ? "i32" : symbol->dynArrayElementType;
            if (symbol->ty.isPtr()) return pointeeOf(*symbol);
            error(node, "значение '" + node.varName + "' не является массивом");
            return "<error>";
        }
        case NODE_CAST: {
            std::string target = normalize(node.varType);
            if (node.left->type == NODE_NUMBER && node.left->varType.empty() &&
                !node.left->value.empty() && node.left->value.find_first_of(".eE") == std::string::npos)
                node.left->varType = target;
            std::string source = expression(*node.left);
            const std::set<std::string> numeric = {"i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "f32", "f64", "char"};
            if (!knownType(target))
                error(node, "неизвестный тип преобразования '" + node.varType + "'");
            else if (source == "<error>")
                ;  // ошибка операнда уже сообщена
            else if (!numeric.contains(target) || !numeric.contains(source))
                error(node, "явное преобразование между '" + typeName(source) +
                            "' и '" + typeName(target) + "' не поддерживается");
            else if (node.left->type == NODE_NUMBER &&
                     node.left->value.find_first_of(".eE") == std::string::npos &&
                     isInteger(target) && !literalFits(target, node.left->value))
                error(node, "числовой литерал вне диапазона типа '" + typeName(target) + "'");
            else if (isInteger(target) && source != "<error>") {
                const bool floatLiteral = node.left->type == NODE_NUMBER &&
                    node.left->value.find_first_of(".eE") != std::string::npos &&
                    node.left->value.rfind("0x", 0) != 0;
                if (floatLiteral) {
                    double v = 0;
                    v = std::strtod(node.left->value.c_str(), nullptr);  // без исключений
                    const sc::Type t = ty(target);
                    const double low = t.isUnsigned() ? -1.0 : -std::ldexp(1.0, static_cast<int>(t.width) - 1) - 1.0;
                    const double high = std::ldexp(1.0, static_cast<int>(t.isUnsigned() ? t.width : t.width - 1));
                    if (!(v > low && v < high))
                        error(node, "значение " + node.left->value + " не помещается в тип '" + typeName(target) + "'");
                } else if (constantInteger(*node.left) && !constantFits(*node.left, target)) {
                    error(node, "значение константного выражения не помещается в тип '" + typeName(target) + "'");
                }
            }
            return target;
        }
        case NODE_ADDR: {
            const Symbol* symbol = use(node, node.varName);
            if (!symbol) {
                error(node, "невозможно получить адрес неизвестного имени '" + node.varName + "'");
                return "<error>";
            }
            // &a для массива: codegen падал (статический) или давал адрес описателя (динамический).
            if (symbol->ty.isArray() || symbol->isDynArray) {
                error(node, "адрес массива '" + node.varName + "' взять нельзя: указатель на массив пока не поддержан");
                return "<error>";
            }
            if (symbol->ty.isStr()) {  // D-014: адрес строки — функцией, не значком
                error(node, "'&" + node.varName + "' для строки не пишется: адрес строки — data(" +
                      node.varName + ") или as_ptr(" + node.varName + ")");
                return "<error>";
            }
            return "ptr";
        }
        case NODE_DEREF: {
            const Symbol* symbol = use(node, node.varName);
            if (!symbol) {
                error(node, "неизвестный указатель '" + node.varName + "'");
                return "<error>";
            }
            if (!symbol->ty.isPtr()) {
                error(node, "разыменование требует тип ptr, получен '" + typeName(symbol->ty.toString()) + "'");
                return "<error>";
            }
            return symbol->pointee.empty() ? "i32" : symbol->pointee;
        }
        case NODE_DYN_ARRAY: {
            std::string element = normalize(node.varType);
            if (!knownType(element)) error(node, "неизвестный тип элемента массива '" + node.varType + "'");
            std::string sizeType = expression(*node.left);
            if (!isInteger(sizeType) && sizeType != "<error>")
                error(*node.left, "размер массива должен быть целым числом");
            // Dynamic arrays are represented at runtime as a heap data pointer (ptr).
            // Length/capacity metadata is tracked on the owning Symbol.
            return "ptr";
        }
        case NODE_INPUT:
            if (node.left) {
                const std::string prompt = expression(*node.left);
                if (prompt != "str" && prompt != "<error>")
                    error(*node.left, "подсказка ввода должна иметь тип str");
            }
            return normalize(node.varType) == "str" ? "str" : "i32";
        case NODE_FILE_OPEN: {
            const std::string pathType = node.left ? expression(*node.left) : "<error>";
            if (pathType != "str" && pathType != "<error>")
                error(node, "имя файла должно иметь тип str");
            if (node.right) {
                const std::string modeType = expression(*node.right);
                if (modeType != "str" && modeType != "<error>")
                    error(*node.right, "режим открытия файла должен иметь тип str");
            } else {
                // D-011: режим обязателен — раньше open(путь) молча открывал на запись ("w").
                error(node, "open: укажите режим открытия вторым аргументом: open(путь, \"r\"), \"w\" или \"a\"");
            }
            return "file";
        }
        case NODE_FILE_READ: {
            const Symbol* file = use(node, node.varName);
            if (!file) error(node, "неизвестный файл '" + node.varName + "'");
            else if (!file->ty.isFile())  // read(x) не-файла падал при выполнении
                error(node, "ожидался file, получен '" + typeName(file->ty.toString()) + "'");
            return "str";
        }
        case NODE_TERNARY: {
            const std::string conditionType = expression(*node.left);
            if (conditionType != "bool" && !isInteger(conditionType) && conditionType != "<error>")
                error(*node.left, "условие тернарного оператора должно иметь тип bool или integer");
            const std::string trueType = expression(*node.right);
            const std::string falseType = expression(*node.extra);
            if (trueType == falseType) return trueType;
            if (compatible(trueType, falseType)) return trueType;
            if (compatible(falseType, trueType)) return falseType;
            error(node, "ветви тернарного оператора имеют несовместимые типы '" +
                  typeName(trueType) + "' и '" + typeName(falseType) + "'");
            return "<error>";
        }
        case NODE_BINOP:
            return binaryExpression(node);
        case NODE_FUNC_CALL:
            return call(node);
        case NODE_ARRAY_PUSH: {
            const Symbol* array = use(node, node.varName);
            rejectConstArray(node, array);
            if (!array) {
                error(node, "неизвестный массив '" + node.varName + "'");
                for (const Node& arg : node.args) expression(arg);
                return "void";
            }
            if (!array->isDynArray) {
                error(node, "push поддерживается только для динамических массивов");
                return "void";
            }
            const std::string element = array->isDynArray
                ? array->dynArrayElementType : array->ty.element->toString();
            if (node.left) {
                std::string value = expression(*node.left);
                if (value != "<error>" && !element.empty() && !compatible(element, value))
                    error(*node.left, "элемент массива ожидает тип '" + typeName(element) + "'");
            }
            return "void";
        }
        case NODE_ARRAY_POP: {
            const Symbol* array = use(node, node.varName);
            rejectConstArray(node, array);
            if (!array) {
                error(node, "неизвестный массив '" + node.varName + "'");
                return "void";
            }
            if (!array->isDynArray) {  // у статического массива длина фиксирована
                error(node, "pop поддерживается только для динамических массивов");
                return "void";
            }
            if (structs_.contains(array->dynArrayElementType)) {
                error(node, "pop() у массива структур пока нет: читайте '" + node.varName + "[" + node.varName +
                      ".len() - 1]' и уменьшайте '" + node.varName + ".resize(…)'");
                return "<error>";
            }
            return array->isDynArray ? array->dynArrayElementType : array->ty.element->toString();
        }
        case NODE_ARRAY_RESIZE: {
            const Symbol* array = use(node, node.varName);
            rejectConstArray(node, array);
            if (!array) {
                error(node, "неизвестный массив '" + node.varName + "'");
                for (const Node& arg : node.args) expression(arg);
                return "void";
            }
            if (!array->isDynArray) {
                error(node, "'resize' доступен только для динамических массивов");
                return "void";
            }
            for (const Node& arg : node.args) {
                std::string size = expression(arg);
                if (size != "<error>" && !ty(size).isInteger())
                    error(arg, "размер массива должен быть целым числом");
            }
            return "void";
        }
        case NODE_ARRAY_LEN:
        case NODE_ARRAY_CAP: {
            const Symbol* array = use(node, node.varName);
            if (!array) {
                error(node, "неизвестный массив '" + node.varName + "'");
                return "i32";
            }
            if (!array->isDynArray && !array->ty.isArray()) {
                error(node, "'" + std::string(node.type == NODE_ARRAY_LEN ? "len" : "cap") +
                      "' доступен только для массивов");
                return "i32";
            }
            return "i64";
        }
        default:
            error(node, "внутренняя ошибка проверки типов: выражение не поддерживается");
            return "<error>";
        }
    }

    static bool isNumericLiteral(const Node& node) {
        return node.type == NODE_NUMBER && node.varType.empty();
    }

    bool fitsNumericLiteral(const Node& node, const std::string& target) const {
        if (!isNumericLiteral(node)) return false;
        const sc::Type targetTy = sc::Type::from(target);
        if (targetTy.isError()) return false;
        const bool floatingLiteral = node.value.find_first_of(".eE") != std::string::npos;
        if (floatingLiteral && targetTy.isInteger()) return false;
        if (targetTy.isInteger() && targetTy.width < 32) return false;
        return targetTy.fitsLiteral(node.value);
    }

    // Кэш значений поддеревьев по адресу узла: проверка каждого `+` в цепочке
    // `1 + 1 + …` иначе заново вычисляла бы всё левое поддерево (время O(n²)).
    // Ключ — только узлы AST в куче (left/right): они живут всю проверку, а
    // временные Node на стеке (Node self) в кэш не попадают.
    mutable std::unordered_map<const Node*, std::optional<__int128>> constantCache_;

    std::optional<__int128> cachedConstant(const std::shared_ptr<Node>& child) const {
        if (auto found = constantCache_.find(child.get()); found != constantCache_.end())
            return found->second;
        const auto value = constantInteger(*child);
        constantCache_.emplace(child.get(), value);
        return value;
    }

    // Значение целочисленного константного выражения (литералы и + - * / // % **).
    std::optional<__int128> constantInteger(const Node& node) const {
        if (node.type == NODE_NUMBER) {
            if (node.value.find_first_of(".eE") != std::string::npos &&
                node.value.rfind("0x", 0) != 0 && node.value.rfind("-0x", 0) != 0) return std::nullopt;
            std::string text;
            for (char c : node.value) if (c != '_') text += c;
            bool negative = false;
            size_t i = 0;
            if (i < text.size() && (text[i] == '-' || text[i] == '+')) negative = text[i++] == '-';
            unsigned base = 10;
            if (text.size() > i + 1 && text[i] == '0' && (text[i + 1] == 'x' || text[i + 1] == 'X')) { base = 16; i += 2; }
            else if (text.size() > i + 1 && text[i] == '0' && (text[i + 1] == 'b' || text[i + 1] == 'B')) { base = 2; i += 2; }
            else if (text.size() > i + 1 && text[i] == '0' && (text[i + 1] == 'o' || text[i + 1] == 'O')) { base = 8; i += 2; }
            if (i >= text.size()) return std::nullopt;
            __int128 value = 0;
            for (; i < text.size(); ++i) {
                const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(text[i])));
                unsigned digit;
                if (c >= '0' && c <= '9') digit = static_cast<unsigned>(c - '0');
                else if (c >= 'a' && c <= 'f') digit = static_cast<unsigned>(c - 'a' + 10);
                else return std::nullopt;  // суффиксы и прочее — не константа
                if (digit >= base) return std::nullopt;
                value = value * base + digit;
                if (value > (static_cast<__int128>(1) << 65)) return std::nullopt;
            }
            return negative ? -value : value;
        }
        if (node.type != NODE_BINOP || !node.left || !node.right) return std::nullopt;
        const auto left = cachedConstant(node.left);
        const auto right = cachedConstant(node.right);
        if (!left || !right) return std::nullopt;
        const __int128 limit = static_cast<__int128>(1) << 65;
        auto bounded = [&](__int128 v) -> std::optional<__int128> {
            if (v > limit || v < -limit) return std::nullopt;
            return v;
        };
        if (node.op == "+") return bounded(*left + *right);
        if (node.op == "-") return bounded(*left - *right);
        if (node.op == "*") {
            if ((*left > limit / 4 || *left < -limit / 4) && *right != 0 && *right != 1 && *right != -1) return std::nullopt;
            if ((*right > limit / 4 || *right < -limit / 4) && *left != 0 && *left != 1 && *left != -1) return std::nullopt;
            if (*left != 0 && (*left * *right) / *left != *right) return std::nullopt;
            return bounded(*left * *right);
        }
        if (node.op == "/") { if (*right == 0) return std::nullopt; return *left / *right; }
        if (node.op == "div") {  // округление вниз: -17 div 5 = -4
            if (*right == 0) return std::nullopt;
            __int128 q = *left / *right;
            if (*left % *right != 0 && ((*left < 0) != (*right < 0))) --q;
            return q;
        }
        if (node.op == "%") { if (*right == 0) return std::nullopt; return *left % *right; }
        if (node.op == "**") {
            if (*right < 0) {  // целая степень с отрицательным показателем усекается к нулю
                if (*left == 1) return 1;
                if (*left == -1) return (*right % 2 == 0) ? 1 : -1;
                if (*left == 0) return std::nullopt;
                return 0;
            }
            if (*right > 200) return std::nullopt;
            __int128 v = 1;
            for (__int128 k = 0; k < *right; ++k) {
                v *= *left;
                if (v > limit || v < -limit) return std::nullopt;
            }
            return v;
        }
        return std::nullopt;
    }

    static std::string int128ToString(__int128 v) {
        if (v == 0) return "0";
        const bool negative = v < 0;
        std::string digits;
        while (v != 0) {
            int d = static_cast<int>(v % 10);
            digits += static_cast<char>('0' + (d < 0 ? -d : d));
            v /= 10;
        }
        if (negative) digits += '-';
        return std::string(digits.rbegin(), digits.rend());
    }

    // Целочисленная политика: деление на константный ноль и константы вне типа — ошибки компиляции.
    void checkConstantArithmetic(const Node& node, const std::string& resultType) const {
        const sc::Type result = ty(resultType);
        if (!result.isInteger() || !node.right) return;
        const bool division = node.op == "/" || node.op == "div" || node.op == "%";
        if (division) {
            const auto divisor = constantInteger(*node.right);
            if (divisor && *divisor == 0) {
                error(*node.right, "деление на ноль");
                return;
            }
        }
        if (!division && node.op != "+" && node.op != "-" && node.op != "*" && node.op != "**") return;
        const auto left = constantInteger(*node.left);
        const auto right = constantInteger(*node.right);
        if (!left || !right) return;
        const unsigned width = result.width < 32 ? 32 : result.width;
        const __int128 low = result.isUnsigned() ? 0 : -(static_cast<__int128>(1) << (width - 1));
        const __int128 high = result.isUnsigned() ? (static_cast<__int128>(1) << width) - 1
                                                  : (static_cast<__int128>(1) << (width - 1)) - 1;
        if (*left < low || *left > high || *right < low || *right > high) return;  // уже сообщено ниже по дереву
        const auto value = constantInteger(node);
        if (!value || *value < low || *value > high)
            error(node, "переполнение: значение константного выражения " +
                  (value ? int128ToString(*value) + " " : std::string()) +
                  "не помещается в тип '" + typeName(resultType) + "'");
    }

    // Константа (литерал или константное целое выражение), точно представимая в target.
    bool constantFits(const Node& value, const std::string& targetRaw) const {
        const sc::Type target = ty(normalize(targetRaw));
        if (!target.isInteger() && !target.isFloat()) return false;
        if (value.type == NODE_NUMBER && value.varType.empty() &&
            value.value.find_first_of(".eE") != std::string::npos)
            return target.isFloat() && literalFits(target.toString(), value.value);
        const auto v = constantInteger(value);
        if (!v) return false;
        if (target.isFloat()) {
            const __int128 limit = static_cast<__int128>(1) << (target.width == 32 ? 24 : 53);
            return *v >= -limit && *v <= limit;
        }
        const unsigned w = target.width;
        const __int128 low = target.isUnsigned() ? 0 : -(static_cast<__int128>(1) << (w - 1));
        const __int128 high = target.isUnsigned() ? (static_cast<__int128>(1) << w) - 1
                                                  : (static_cast<__int128>(1) << (w - 1)) - 1;
        return *v >= low && *v <= high;
    }

    static bool sameTarget(const Node& a, const Node& b) {
        if (a.type != b.type) return false;
        if (a.type == NODE_IDENTIFIER) return a.value == b.value;
        if (a.type == NODE_STRUCT_FIELD) return a.varName == b.varName && a.op == b.op;
        if (a.type == NODE_ARRAY_ACCESS) return a.varName == b.varName;
        return false;
    }

    // t = t op v / t op= v для узкого целого t: разрешено, codegen проверяет диапазон результата.
    bool narrowCompound(const Node& target, const Node& value,
                        const std::string& targetType, const std::string& actual) const {
        if (!isInteger(normalize(targetType)) || !isInteger(actual)) return false;
        if (value.type != NODE_BINOP || !value.left) return false;
        if (value.op != "+" && value.op != "-" && value.op != "*" && value.op != "/" &&
            value.op != "%" && value.op != "div") return false;
        if (!sameTarget(*value.left, target)) return false;
        // Константа, не влезающая в тип цели (u8 x; x += 300), — ошибка компиляции.
        if (value.right && constantInteger(*value.right) && !constantFits(*value.right, targetType)) return false;
        return true;
    }

    std::string castHint(const std::string& expected, const std::string& actual) const {
        const sc::Type e = ty(normalize(expected));
        const sc::Type a = ty(normalize(actual));
        if ((e.isInteger() || e.isFloat()) && (a.isInteger() || a.isFloat()))
            return "; возможна потеря значения, нужно явное приведение: (" + typeName(normalize(expected)) + ") значение";
        if (a.isPtr() && !e.isPtr())
            return "; null и указатели допустимы только для типа ptr";
        if (e.isPtr() && a.isStr())  // D-014: string неявно в ptr не превращается
            return "; string неявно в ptr не превращается: храните строку в string "
                   "(string r = replace(…);), del для строки не пишется; адрес строки — data(s) или as_ptr(s)";
        return "";
    }

    [[gnu::noinline]] std::string binary(const Node& node, std::string left) {
        if (node.op == "!") {
            if (left == "<error>") return "<error>";
            if (left != "bool" && !ty(left).isInteger())
                error(node, "оператор ! ожидает bool или целое число");
            return "bool";
        }
        if (node.op == "~") {
            if (left == "<error>") return "<error>";
            if (!ty(left).isInteger()) {
                error(node, "побитовая инверсия требует целочисленный операнд");
                return "<error>";
            }
            return left;
        }
        std::string right = node.right ? expression(*node.right) : std::string("<error>");
        if (node.right && fitsNumericLiteral(*node.left, right))
            left = right;
        else if (node.right && fitsNumericLiteral(*node.right, left))
            right = left;
        if (left == "<error>" || right == "<error>") return "<error>";
        if (node.op == "div") {
            if (!ty(left).isInteger() || !ty(right).isInteger()) {
                error(node, "оператор div требует целочисленные операнды");
                return "<error>";
            }
            if (ty(left).width == 64 || ty(right).width == 64)
                return (ty(left).isUnsigned() && ty(right).isUnsigned()) ? "u64" : "i64";
            if (ty(left).isUnsigned() || ty(right).isUnsigned()) return "u32";
            return "i32";
        }
        if (node.op == "**") {
            if (!ty(left).isNumeric() || !ty(right).isNumeric()) {
                error(node, "оператор ** требует числовые операнды");
                return "<error>";
            }
            const sc::Type lTy = ty(left);
            const sc::Type rTy = ty(right);
            if (lTy.isFloat() || rTy.isFloat()) {
                const sc::Type result = lTy.commonType(rTy);
                return result.isError() ? "<error>" : result.toString();
            }
            const sc::Type result = lTy.commonType(rTy);
            return result.isError() ? "<error>" : result.toString();
        }
        if (node.op == "&" || node.op == "|" || node.op == "^" ||
            node.op == "<<" || node.op == ">>") {
            if (!ty(left).isInteger() || !ty(right).isInteger()) {
                error(node, "побитовые операторы требуют целочисленные операнды");
                return "<error>";
            }
            if ((node.op == "<<" || node.op == ">>") && node.right->type == NODE_NUMBER) {
                const unsigned width = ty(left).width;
                if (const auto shift = constantInteger(*node.right); shift && (*shift < 0 || *shift >= width)) {
                    error(*node.right, "счётчик сдвига должен быть меньше ширины левого операнда");
                    return "<error>";
                }
            }
            return left;
        }
        if (node.op == "&&" || node.op == "||") {
            if ((left != "bool" && !ty(left).isInteger()) ||
                (right != "bool" && !ty(right).isInteger()))
                error(node, "логические операторы ожидают bool или целые числа");
            return "bool";
        }
        const sc::Type leftTy = ty(left);
        const sc::Type rightTy = ty(right);
        const bool comparison = node.op == ">" || node.op == "<" || node.op == ">=" ||
            node.op == "<=" || node.op == "==" || node.op == "!=";
        if (comparison && leftTy.isStr() && rightTy.isStr()) {
            if (node.op == "==" || node.op == "!=") {
                node.varType = "string-compare";  // codegen: strcmp
                return "bool";
            }
            error(node, "строки сравниваются только через == и !=");
            return "<error>";
        }
        if (comparison && (leftTy.isPtr() || rightTy.isPtr())) {
            if ((node.op == "==" || node.op == "!=") && leftTy.isPtr() && rightTy.isPtr()) return "bool";
            error(node, "указатели можно сравнивать только между собой и с null через == и !=");
            return "<error>";
        }
        // c == 'a': символ с символом (литерал 'a' имеет тип u8) сравнивается как в C++ и Rust.
        if (comparison && (leftTy.isChar() || rightTy.isChar()) &&
            (leftTy.isChar() || charLike(*node.left)) && (rightTy.isChar() || charLike(*node.right)))
            return "bool";
        if (comparison) {
            if (!leftTy.isNumeric() || !rightTy.isNumeric()) {
                // 1 < 2 < 3 — это (1 < 2) < 3: bool сравнивается с числом. Как в Rust,
                // цепочка — ошибка; сказать это прямо и подсказать запись через &&.
                const auto isComparison = [](const Node& side) {
                    return side.type == NODE_BINOP && (side.op == "<" || side.op == ">" ||
                        side.op == "<=" || side.op == ">=" || side.op == "==" || side.op == "!=");
                };
                if (isComparison(*node.left) || isComparison(*node.right)) {
                    error(node, "сравнения нельзя соединять в цепочку (a < b < c): результат "
                          "сравнения — bool, а не число; пишите a < b && b < c");
                    return "<error>";
                }
                error(node, "оператор сравнения принимает только числа");
                return "<error>";
            }
            if (leftTy.isInteger() && rightTy.isInteger() && leftTy.isUnsigned() != rightTy.isUnsigned()) {
                error(node, "нельзя сравнивать signed- и unsigned-типы без явного преобразования");
                return "<error>";
            }
            return "bool";
        }
        // char в арифметике — как u8 (результат — целое, без неявного сужения обратно в char).
        const sc::Type leftArith = leftTy.isChar() ? sc::Type::integer(8, false) : leftTy;
        const sc::Type rightArith = rightTy.isChar() ? sc::Type::integer(8, false) : rightTy;
        if (leftTy.isChar() || rightTy.isChar()) {
            if (!leftArith.isInteger() || !rightArith.isInteger()) {
                error(node, "арифметический оператор '" + node.op + "' для char требует целый операнд");
                return "<error>";
            }
            return leftArith.width == 64 || rightArith.width == 64 ? "i64" : "i32";
        }
        if (!leftTy.isNumeric() || !rightTy.isNumeric()) {
            error(node, "арифметический оператор '" + node.op + "' требует числовые операнды");
            return "<error>";
        }
        // Arithmetic promotion: delegate to the unified commonType().
        const sc::Type result = leftTy.commonType(rightTy);
        if (!result.isError()) return result.toString();
        error(node, "нельзя смешивать типы '" + typeName(leftTy.toString()) +
              "' и '" + typeName(rightTy.toString()) + "' без явного преобразования");
        return "<error>";
    }

    std::string call(const Node& node) {
        static const std::map<std::string, std::pair<size_t, size_t>> builtins = {
            {"sqrt", {1,1}}, {"abs", {1,1}}, {"pow", {2,2}},
            {"max", {2,2}}, {"min", {2,2}}, {"floor", {1,1}},
            {"ceil", {1,1}}, {"round", {1,1}}, {"rand", {0,0}},
            {"len", {1,1}}, {"replace", {3,3}}, {"str", {1,1}},
            {"data", {1,1}}, {"as_ptr", {1,1}}, {"move", {1,1}}
        };
        auto builtin = builtins.find(node.varName);
        if (builtin != builtins.end()) {
            if (node.args.size() < builtin->second.first || node.args.size() > builtin->second.second) {
                error(node, "функция '" + node.varName + "' ожидает " +
                      std::to_string(builtin->second.first) + " аргументов, получено " +
                      std::to_string(node.args.size()));
            }
            std::vector<std::string> argumentTypes;
            for (size_t index = 0; index < node.args.size(); ++index) {
                const Node& arg = node.args[index];
                std::string type = expression(arg);
                argumentTypes.push_back(type);
                const bool expectsString = node.varName == "len" || node.varName == "replace" ||
                    isStringAddress(node) || node.varName == "move";
                if (node.varName == "move" && structs_.contains(type)) {}  // D-016: перенос структуры
                else if (expectsString && type != "str" && type != "<error>")
                    error(arg, "аргумент " + std::to_string(index + 1) + " функции '" +
                          node.varName + "' должен иметь тип str");
                else if (!expectsString && node.varName != "str" &&
                         !isInteger(type) && type != "f32" && type != "f64" &&
                         type != "char" && type != "<error>")
                    error(arg, "недопустимый тип аргумента функции '" + node.varName + "'");
            }
            if (node.varName == "len" || node.varName == "rand") return "i32";
            if (node.varName == "str") {
                // str(x): число, bool, char или строка (копия). Указатель, массив, структура,
                // файл раньше молча превращались в адрес-число.
                const std::string& argument = argumentTypes.empty() ? "<error>" : argumentTypes[0];
                const sc::Type argumentTy = ty(argument);
                const Symbol* named = !node.args.empty() && node.args[0].type == NODE_IDENTIFIER
                    ? lookup(node.args[0].value) : nullptr;
                if (argument != "<error>" && ((named && named->isDynArray) ||
                    !(argumentTy.isNumeric() || argumentTy.isStr() || argument == "bool" || argument == "char")))
                    error(node.args[0], "str: значение типа '" + typeName(argument) + "' в строку не преобразуется");
                return "str";
            }
            if (node.varName == "replace") return "str";  // D-014: только строка
            if (node.varName == "move") {  // D-015: перенос строки, старое имя дальше нельзя
                const Node& source = node.args[0];
                size_t index = 0;
                Symbol* moved = source.type == NODE_IDENTIFIER ? lookupMutable(source.value, &index) : nullptr;
                const bool movesStruct = structs_.contains(argumentTypes[0]);
                if (argumentTypes[0] != "str" && !movesStruct) {}  // ошибка типа уже выдана
                else if (!moved)
                    error(source, "move(…) переносит строку или структуру из переменной: пишите move(имя)");
                else if (index < dropBarrier_)
                    error(node, "move '" + source.value + "' внутри цикла или функции, а объявлена она "
                          "снаружи: перенос выполнился бы несколько раз");
                else if (!moved->droppedLine) {
                    moved->droppedLine = node.line;
                    moved->releasedBy = "move";
                }
                return movesStruct ? argumentTypes[0] : "str";
            }
            if (isStringAddress(node)) {  // D-014: адрес строки — только явно
                const Node& source = node.args[0];
                if (source.type != NODE_IDENTIFIER && source.type != NODE_STRUCT_FIELD &&
                    argumentTypes[0] == "str")  // временная строка живёт до конца оператора
                    error(source, node.varName + "(…) берёт адрес строки с именем: временная строка "
                          "освобождается в конце оператора, сначала положите её в переменную (string s = …;)");
                return "ptr";
            }
            if (node.varName == "sqrt" || node.varName == "pow" ||
                node.varName == "floor" || node.varName == "ceil" || node.varName == "round") {
                // D-009: целые аргументы. pow двух целых — целое, как `**` (с проверкой
                // переполнения), остальные — f64 (sqrt(2) = 1.414…).
                bool allInteger = !argumentTypes.empty();
                for (const std::string& type : argumentTypes)
                    allInteger = allInteger && isInteger(type);
                if (allInteger && node.varName == "pow") node.varType = "int-pow";
                else if (allInteger) return "f64";
                sc::Type resultTy = sc::Type::from(argumentTypes.empty() ? "i32" : argumentTypes.front());
                for (size_t index = 1; index < argumentTypes.size(); ++index) {
                    const sc::Type argTy = sc::Type::from(argumentTypes[index]);
                    if (!argTy.isError() && !resultTy.isError()) {
                        resultTy = resultTy.commonType(argTy);
                    }
                }
                return resultTy.toString();
            }
            if ((node.varName == "max" || node.varName == "min") && argumentTypes.size() == 2) {
                // Как сравнение `<`: знаковое с беззнаковым без приведения не смешиваются
                // (std::max в C++ и max в Rust тоже требуют один тип). Литерал, который
                // помещается в тип другого аргумента, допустим: max(u, 3).
                const sc::Type a = ty(argumentTypes[0]);
                const sc::Type b = ty(argumentTypes[1]);
                if (a.isInteger() && b.isInteger() && a.isUnsigned() != b.isUnsigned() &&
                    !constantFits(node.args[0], argumentTypes[1]) && !constantFits(node.args[1], argumentTypes[0])) {
                    error(node, node.varName + ": нельзя смешивать signed- и unsigned-типы без явного преобразования");
                    return "<error>";
                }
            }
            if (node.varName == "abs" && argumentTypes.size() == 1 && ty(argumentTypes[0]).isUnsigned()) {
                // abs беззнакового не определён ни в C++ (std::abs(unsigned) — неоднозначный вызов),
                // ни в Rust (у u32 нет abs): значение и так неотрицательно.
                error(node, "abs: значение беззнакового типа '" + typeName(argumentTypes[0]) +
                      "' и так неотрицательно");
                return "<error>";
            }
            if (node.varName == "abs" || node.varName == "max" || node.varName == "min") {
                sc::Type resultTy = sc::Type::from(argumentTypes.empty() ? "i32" : argumentTypes.front());
                for (size_t index = 1; index < argumentTypes.size(); ++index) {
                    const sc::Type argTy = sc::Type::from(argumentTypes[index]);
                    if (!argTy.isError() && !resultTy.isError()) {
                        resultTy = resultTy.commonType(argTy);
                    }
                }
                return resultTy.toString();
            }
            if (!argumentTypes.empty()) return argumentTypes[0];
            return "i32";
        }
        auto function = functions_.find(node.varName);
        if (function == functions_.end()) {
            error(node, "неизвестная функция '" + node.varName + "'");
            for (const Node& arg : node.args) expression(arg);
            return "<error>";
        }
        if (node.args.size() != function->second.parameters.size()) {
            error(node, "функция '" + node.varName + "' ожидает " +
                  std::to_string(function->second.parameters.size()) + " аргументов, получено " +
                  std::to_string(node.args.size()));
        }
        size_t count = std::min(node.args.size(), function->second.parameters.size());
        for (size_t i = 0; i < node.args.size(); ++i) {
            if (const sc::Type parameter = sc::Type::from(i < count ? function->second.parameters[i] : "");
                parameter.isDynArray()) {  // fn f(int[] v): копия, move — перенос (D-018)
                if (const std::string problem = checkArrayValue(node.args[i], parameter.element->toString());
                    !problem.empty())
                    error(node.args[i], "аргумент " + std::to_string(i + 1) + " функции '" + node.varName +
                          "': " + problem);
                continue;
            }
            if (node.args[i].type == NODE_IDENTIFIER) {
                const Symbol* argument = lookup(node.args[i].value);
                if (argument && argument->isDynArray) {
                    error(node.args[i], "динамический массив '" + node.args[i].value +
                          "' нельзя передать в параметр ptr (он не знает длину массива): объявите параметр "
                          "как массив, например 'int[] v'");
                    continue;
                }
            }
            std::string actual = expression(node.args[i]);
            const std::string expectedType = i < count
                ? normalize(function->second.parameters[i]) : std::string();
            const bool fittingLiteral = i < count && (constantFits(node.args[i], expectedType) ||
                (node.args[i].type == NODE_NUMBER && literalFits(expectedType, node.args[i].value)));
            if (i < count && actual != "<error>" &&
                !compatible(function->second.parameters[i], actual) && !fittingLiteral) {
                error(node.args[i], "аргумент " + std::to_string(i + 1) + " функции '" +
                      node.varName + "': ожидался '" + typeName(function->second.parameters[i]) +
                      "', получен '" + typeName(actual) + "'" +
                      castHint(function->second.parameters[i], actual));
            }
        }
        return function->second.returnType;
    }

    void checkStruct(const Node& node) {
        std::set<std::string> fields;
        for (const Node& field : node.children) {
            if (const sc::Type t = ty(field.varType); t.isDynArray() && !(t.element && t.element->isScalar()))
                error(field, "поле-массив: элементы пока только числа, bool и char, а не '" +
                      typeName(t.element->toString()) + "'");
            else if (!knownType(field.varType)) error(field, "неизвестный тип поля '" + field.varType + "'");
            if (!fields.insert(field.varName).second)
                error(field, "повторное объявление поля '" + field.varName + "'");
            if (field.left) {
                std::string actual = expression(*field.left);
                if (actual != "<error>" && !compatible(field.varType, actual)) {
                    bool fittingLiteral = field.left->type == NODE_NUMBER &&
                        literalFits(normalize(field.varType), field.left->value);
                    if (!fittingLiteral)
                        error(field, "значение по умолчанию поля '" + field.varName + "' имеет тип '" +
                              typeName(actual) + "', а ожидается '" + typeName(normalize(field.varType)) + "'");
                }
                if (field.left->type == NODE_NUMBER &&
                    !literalFits(normalize(field.varType), field.left->value))
                    error(field, "литерал поля вне диапазона типа '" + typeName(normalize(field.varType)) + "'");
            }
            }
        // Check for recursive struct definitions (infinite size)
        std::unordered_set<std::string> visited;
        checkStructRecursion(node, visited);
    }

    void checkStructRecursion(const Node& node, std::unordered_set<std::string>& visited) {
        if (visited.contains(node.varName)) {
            error(node, "рекурсивное определение структуры '" + node.varName + "' приводит к бесконечному размеру");
            return;
        }
        visited.insert(node.varName);
        for (const Node& field : node.children) {
            // Check if field type is a struct that could create a cycle
            sc::Type fieldTy = sc::Type::from(normalize(field.varType));
            if (fieldTy.isStruct() && structs_.contains(fieldTy.structName)) {
                checkStructRecursion(*structs_.at(fieldTy.structName), visited);
            }
        }
        visited.erase(node.varName);
    }

    bool alwaysReturns(const std::vector<Node>& statements) const {
        if (statements.empty()) return false;
        const Node& last = statements.back();
        if (last.type == NODE_RETURN) return true;
        if (last.type == NODE_IF && !last.elseChildren.empty())
            return alwaysReturns(last.children) && alwaysReturns(last.elseChildren);
        if (last.type == NODE_LOOP) return !breaksOut(last.children);  // бесконечный loop без break
        return false;
    }

    // Есть ли break, выходящий из текущего цикла (вложенные циклы не считаются).
    static bool breaksOut(const std::vector<Node>& statements) {
        for (const Node& s : statements) {
            if (s.type == NODE_BREAK) return true;
            if (s.type == NODE_IF && (breaksOut(s.children) || breaksOut(s.elseChildren))) return true;
        }
        return false;
    }

    bool alwaysTerminates(const Node& node) const {
        if (node.type == NODE_RETURN || node.type == NODE_BREAK ||
            node.type == NODE_CONTINUE) return true;
        if (node.type == NODE_IF && !node.elseChildren.empty())
            return alwaysTerminates(node.children) && alwaysTerminates(node.elseChildren);
        return false;
    }

    bool alwaysTerminates(const std::vector<Node>& statements) const {
        return !statements.empty() && alwaysTerminates(statements.back());
    }

    void checkStatements(const std::vector<Node>& statements) {
        bool terminated = false;
        for (const Node& statement : statements) {
            if (terminated)
                error(statement, "недостижимый код после return/break/continue");
            checkNode(statement);
            terminated = terminated || alwaysTerminates(statement);
        }
    }

    void checkFunction(const Node& node) {
        const size_t savedBarrier = dropBarrier_;
        dropBarrier_ = scopes_.size();
        functionScope_ = scopes_.size();
        scopes_.emplace_back();
        returnTypes_.push_back(normalize(node.varType));
        if (const sc::Type result = sc::Type::from(normalize(node.varType)); result.isDynArray() &&
            !knownType(node.varType)) {
            error(node, "неизвестный тип результата '" + node.varType + "'");
        } else if (result.isStruct()) {
            if (!structs_.contains(result.structName))
                error(node, "неизвестный тип результата '" + node.varType + "'");
        }
        for (const std::string& param : node.params) {
            size_t separator = param.find(':');
            std::string type = param.substr(0, separator);
            std::string name = separator == std::string::npos ? "" : param.substr(separator + 1);
            if (!knownType(type)) error(node, "неизвестный тип параметра '" + type + "'");
            if (const sc::Type t = ty(type); t.isDynArray())  // int[] v — своя копия массива (D-018)
                declare(name, "ptr", node, false, true, true, t.element ? t.element->toString() : "i32");
            else
                declare(name, type, node, false, true);
        }
        checkStatements(node.children);
        if (normalize(node.varType) != "void" && !alwaysReturns(node.children))
            error(node, "функция должна возвращать значение на всех путях выполнения");
        returnTypes_.pop_back();
        scopes_.pop_back();
        dropBarrier_ = savedBarrier;
    }

    void checkNode(const Node& node) {
        switch (node.type) {
        case NODE_VAR_DECL: {
            // int[] b = a (копия), = move(a), = f(), = new int[n]; ptr b = f() / move(a) (D-018)
            if (const sc::Type declared = ty(node.varType); (declared.isDynArray() && knownType(node.varType)) ||
                (declared.isPtr() && isArrayResult(*node.left))) {
                std::string element = declared.isDynArray() ? declared.element->toString() : arrayValue(*node.left);
                int capacity = 0;
                if (!declared.isDynArray()) {}
                else if (node.left->type == NODE_DYN_ARRAY) {
                    expression(*node.left);
                    if (normalize(node.left->varType) != element)
                        error(node, "переменная '" + node.varName + "': ожидался массив '" + typeName(node.varType) +
                              "', получен массив '" + typeName("dyn<" + node.left->varType + ">") + "'");
                    if (const auto n = node.left->left && node.left->left->type == NODE_NUMBER
                        ? constantInteger(*node.left->left) : std::nullopt; n && *n >= 0 && *n <= INT32_MAX)
                        capacity = static_cast<int>(*n);
                } else if (const std::string problem = checkArrayValue(*node.left, element); !problem.empty())
                    error(node, "переменная '" + node.varName + "': " + problem);
                declare(node.varName, "ptr", node, node.isConst, true, true, element);
                if (scopes_.back().contains(node.varName)) scopes_.back()[node.varName].dynArrayCapacity = capacity;
                break;
            }
            const bool inferType = node.varType.empty();
            if (!inferType && !knownType(node.varType)) error(node, "неизвестный тип переменной '" + node.varType + "'");
            const std::string declaredType = normalize(node.varType);
            const bool contextualIntegerLiteral = node.left->type == NODE_NUMBER &&
                node.left->varType.empty() &&
                node.left->value.find_first_of(".eE") == std::string::npos &&
                isInteger(declaredType);
            std::string actual = contextualIntegerLiteral ? declaredType : expression(*node.left);
            if (actual == "void") {  // let x = f(), где f ничего не возвращает: раньше крах компилятора
                error(*node.left, "значение не возвращается: функция без результата (void) — '" +
                      node.varName + "' нечем инициализировать");
                actual = "<error>";
            }
            const std::string effectiveType = inferType ? actual : declaredType;
            if (inferType && actual != "<error>") node.varType = actual;
            const bool numericLiteral = node.left->type == NODE_NUMBER &&
                (isInteger(effectiveType) || effectiveType == "f32" || effectiveType == "f64");
            const bool literalInRange = numericLiteral && literalFits(effectiveType, node.left->value);
            if (numericLiteral && !literalInRange) {
                error(*node.left, "числовой литерал вне диапазона типа '" + typeName(effectiveType) + "'");
            } else if (!inferType && actual != "<error>" && !compatible(node.varType, actual) &&
                       !literalInRange && !constantFits(*node.left, node.varType)) {
                error(node, "переменная '" + node.varName + "' имеет тип '" +
                      typeName(effectiveType) + "', но инициализируется значением типа '"+
                      typeName(actual) + "'" + castHint(effectiveType, actual));
            }
            // Variable with initializer is considered initialized
            bool dynArrayInit = false;
            std::string dynElementType;
            int dynCapacity = 0;
            if (node.left->type == NODE_DYN_ARRAY) {
                dynArrayInit = true;
                dynElementType = normalize(node.left->varType);
                if (node.left->left && node.left->left->type == NODE_NUMBER) {
                    if (const auto n = constantInteger(*node.left->left); n && *n >= 0 && *n <= INT32_MAX)
                        dynCapacity = static_cast<int>(*n);
                }
            }
            // С инициализатором переменная инициализирована, даже если в нём ошибка:
            // иначе каждое её использование дало бы ложную вторую ошибку (1.7).
            declare(node.varName, node.varType, node, node.isConst, true,
                    dynArrayInit, dynElementType);
            if (dynArrayInit && scopes_.back().contains(node.varName))
                scopes_.back()[node.varName].dynArrayCapacity = dynCapacity;
            if (isStringAddress(*node.left) && scopes_.back().contains(node.varName))
                scopes_.back()[node.varName].pointee = "char";
            if (node.left->type == NODE_ADDR && scopes_.back().contains(node.varName)) {
                if (const Symbol* target = lookup(node.left->varName))
                    scopes_.back()[node.varName].pointee = target->type;
                scopes_.back()[node.varName].pointsToLocal = isFunctionLocal(node.left->varName);
            }
            if (scopes_.back().contains(node.varName))
                scopes_.back()[node.varName].pointsToVariable = pointsToVariable(*node.left);
            break;
        }
        case NODE_ARRAY_DECL: {
            std::string element = normalize(node.varType);
            if (!knownType(element)) error(node, "неизвестный тип элемента массива '" + element + "'");
            const bool elementIsInteger = isInteger(element);
            for (const Node& value : node.args) {
                std::string actual;
                const bool contextualIntegerLiteral = value.type == NODE_NUMBER &&
                    value.varType.empty() &&
                    value.value.find_first_of(".eE") == std::string::npos &&
                    elementIsInteger;
                if (contextualIntegerLiteral && !literalFits(element, value.value)) {
                    error(value, "элемент массива " + value.value + " вне диапазона типа '" +
                          typeName(element) + "'");
                    continue;
                }
                actual = contextualIntegerLiteral ? element : expression(value);
                if (actual != "<error>" && !compatible(element, actual) && !constantFits(value, element))
                    error(value, "элемент массива должен иметь тип '" + typeName(element) + "'" +
                          castHint(element, actual));
            }
            unsigned length = node.args.size();
            sc::Type arrayType = sc::Type::arrayTy(sc::Type::from(element), length);
            auto& scope = scopes_.back();
            if (scope.contains(node.varName)) {
                error(node, "повторное объявление '" + node.varName + "' в этой области видимости");
                break;
            }
            Symbol sym;
            sym.type = arrayType.toString();
            sym.ty = arrayType;
            sym.line = node.line;
            sym.column = node.column;
            sym.isConst = node.isConst;  // const a: int[] = [...] — раньше константность терялась
            sym.isInitialized = true;
            sym.isDynArray = false;
            scope[node.varName] = sym;
            break;
        }
        case NODE_STRUCT_INSTANCE: {
            auto structure = structs_.find(node.varType);
            if (structure == structs_.end()) {
                error(node, "неизвестная структура '" + node.varType + "'");
                break;
            }
            if (node.left) {  // P q = make(…); / P w = q;
                const std::string actual = expression(*node.left);
                if (actual != "<error>" && normalize(actual) != normalize(node.varType))
                    error(node, "переменная '" + node.varName + "' имеет тип '" + node.varType +
                          "', но инициализируется значением типа '" + typeName(actual) + "'");
            }
            std::map<std::string, const Node*> named;
            std::vector<const Node*> positional;
            for (const Node& init : node.children) {
                if (init.varName.empty()) positional.push_back(&init);
                else if (!named.emplace(init.varName, &init).second)
                    error(init, "поле '" + init.varName + "' инициализировано дважды");
            }
            size_t position = 0;
            for (const Node& field : structure->second->children) {
                const Node* value = nullptr;
                auto it = named.find(field.varName);
                if (it != named.end()) value = it->second;
                else if (position < positional.size()) value = positional[position++];
                if (value) {
                    std::string actual = expression(*value->left);
                    if (actual != "<error>" && !compatible(field.varType, actual) &&
                        !constantFits(*value->left, field.varType))
                        error(*value, "поле '" + field.varName + "' ожидает тип '" +
                              typeName(normalize(field.varType)) + "', получен '" + typeName(actual) + "'");
                    if (value->left->type == NODE_NUMBER &&
                        !literalFits(normalize(field.varType), value->left->value))
                        error(*value, "литерал поля вне диапазона типа '" + typeName(normalize(field.varType)) + "'");
                }
                // Check for mandatory fields (no default value and not provided)
                // Only enforce when a struct literal is provided (instance has children)
                bool hasDefault = field.left != nullptr || ty(field.varType).isDynArray();  // int[] — пустой
                if (!value && !hasDefault && !node.children.empty()) {
                    error(node, "поле '" + field.varName + "' не имеет значения по умолчанию и должно быть инициализировано");
                }
                if (!value) named.erase(field.varName);
            }
            for (const auto& item : named) {
                bool exists = false;
                for (const Node& field : structure->second->children)
                    if (field.varName == item.first) exists = true;
                if (!exists) error(*item.second, "у структуры нет поля '" + item.first + "'");
            }
            if (position < positional.size()) error(node, "слишком много позиционных значений структуры");
            // Struct instances are considered initialized since all fields have
            // default values from the struct definition or were explicitly provided
            declare(node.varName, node.varType, node, false, true);
            for (const Node& field : structure->second->children)
                declare(node.varName + "." + field.varName, field.varType, node, false, true);
            break;
        }
        case NODE_ASSIGN: {
            Symbol* released = lookupMutable(node.varName);
            const bool afterDel = released && released->droppedLine &&
                                  (std::string_view(released->releasedBy) == "del" ||
                                   std::string_view(released->releasedBy) == "move");
            if (afterDel) released->droppedLine = 0;  // p = new … / s = … после move: имя снова пригодно
            if (released) released->droppedFields.clear();  // t = q: сброшенные поля снова есть (D-017)
            const Symbol* symbol = afterDel ? released : use(node, node.varName);
            if (!symbol) {
                error(node, "присваивание неизвестному имени '" + node.varName + "'");
                expression(*node.left);
                break;
            }
            const Symbol* sourceArray = node.left->type == NODE_IDENTIFIER ? lookup(node.left->value) : nullptr;
            if (symbol->isDynArray && !symbol->isConst && (isArrayResult(*node.left) ||
                (sourceArray && (sourceArray->isDynArray || sourceArray->ty.isArray())))) {  // v = a: копия (D-018)
                if (const std::string problem = checkArrayValue(*node.left, symbol->dynArrayElementType);
                    !problem.empty())
                    error(node, "'" + node.varName + "': " + problem);
                if (released && std::string_view(released->releasedBy) == "move")
                    released->droppedLine = 0;  // v = f(move(v)): после присваивания имя снова пригодно
                break;
            }
            if (symbol && symbol->isConst) {
                error(node, "нельзя присвоить значение константе '" + node.varName + "'");
                expression(*node.left);
                break;
            }
            // Массив целиком не присваивается (codegen падал на `a = b`): только элементы.
            if (symbol->ty.isArray()) {
                error(node, "массив '" + node.varName + "' нельзя присвоить целиком: присваивайте элементы, '" +
                      node.varName + "[i] = …'");
                expression(*node.left);
                break;
            }
            // p = &y / p = q: тип, на который указывает p, задан при объявлении и не меняется.
            if (symbol->ty.isPtr() && !symbol->isDynArray &&
                (node.left->type == NODE_ADDR || node.left->type == NODE_IDENTIFIER ||
                 isStringAddress(*node.left))) {
                const bool address = node.left->type == NODE_ADDR;
                const Symbol* source = isStringAddress(*node.left) ? nullptr
                    : lookup(address ? node.left->varName : node.left->value);
                std::string target = isStringAddress(*node.left) ? "char" : "";
                if (source && address) target = normalize(source->type);
                else if (source && source->ty.isPtr() && !source->isDynArray) target = pointeeOf(*source);
                if (!target.empty() && target != pointeeOf(*symbol))
                    error(node, "'" + node.varName + "' указывает на тип '" + typeName(pointeeOf(*symbol)) +
                          "', присвоить ему адрес значения типа '" + typeName(target) +
                          "' нельзя; объявите отдельный указатель: ptr q = &…");
            }
            if (symbol->ty.isPtr())
                if (Symbol* target = lookupMutable(node.varName)) target->pointsToVariable = pointsToVariable(*node.left);
            std::string expected = symbol->ty.toString();
            std::string actual = expression(*node.left);
            bool literalArithmeticFits = false;
            if (symbol && node.left->type == NODE_BINOP &&
                (node.left->op == "+" || node.left->op == "-" ||
                 node.left->op == "*" || node.left->op == "/" || node.left->op == "%")) {
                const Node* literal = nullptr;
                const Node* other = nullptr;
                if (isNumericLiteral(*node.left->left)) {
                    literal = node.left->left.get();
                    other = node.left->right.get();
                } else if (isNumericLiteral(*node.left->right)) {
                    literal = node.left->right.get();
                    other = node.left->left.get();
                }
                if (literal && other->type == NODE_IDENTIFIER && other->value == node.varName) {
                    const bool floatingLiteral = literal->value.find_first_of(".eE") != std::string::npos;
                    literalArithmeticFits = floatingLiteral
                        ? (expected == "f32" || expected == "f64") && literalFits(expected, literal->value)
                        : isInteger(expected) && literalFits(expected, literal->value);
                }
            }
            Node self;
            self.type = NODE_IDENTIFIER;
            self.value = node.varName;
            if (actual != "<error>" && !compatible(expected, actual) && !literalArithmeticFits &&
                !(node.left && constantFits(*node.left, expected)) &&
                !(node.left && narrowCompound(self, *node.left, expected, actual)))
                error(node, "нельзя присвоить '" + typeName(actual) + "' значению типа '" + typeName(expected) + "'" +
                      castHint(expected, actual));
            else
                markInitialized(node.varName);
            // s = move(s): после присваивания у имени снова есть значение (D-015).
            if (Symbol* target = lookupMutable(node.varName);
                target && std::string_view(target->releasedBy) == "move")
                target->droppedLine = 0;
            break;
        }
        case NODE_STRUCT_FIELD_ASSIGN: {
            if (Symbol* root = lookupMutable(node.varName); root && root->ty.isStruct()) {
                const sc::Type fieldTy = ty(fieldTypeAt(structs_, root->ty.structName, node.op));
                if (node.left->type == NODE_ARRAY_DECL) {  // t.scores = [1, 2] (D-017)
                    if (!fieldTy.isDynArray())
                        error(node, "список [...] присваивается только полю-массиву");
                    else if (root->isConst)
                        error(node, "нельзя изменять поле константной структуры '" + node.varName + "'");
                    for (const Node& element : node.left->args) {
                        const std::string value = expression(element);
                        if (fieldTy.isDynArray() && value != "<error>" &&
                            !compatible(fieldTy.element->toString(), value) &&
                            !constantFits(element, fieldTy.element->toString()))
                            error(element, "элемент массива ожидает тип '" + typeName(fieldTy.element->toString()) + "'");
                    }
                    root->droppedFields.erase(node.op);
                    break;
                }
                if (fieldTy.isDynArray()) {
                    error(node, "полю-массиву '" + node.varName + "." + node.op + "' присваивается только "
                          "список: " + node.varName + "." + node.op + " = [ ... ];");
                    expression(*node.left);
                    break;
                }
                std::erase_if(root->droppedFields,  // t.inner = q: сброшенные поля t.inner снова есть
                              [&](const auto& item) { return item.first.starts_with(node.op + "."); });
            }
            if (node.op.find('.') != std::string::npos || node.right) {
                const Symbol* root = use(node, node.varName);
                if (root && root->isConst) {
                    error(node, "нельзя изменять поле константной структуры '" + node.varName + "'");
                    expression(*node.left);
                    break;
                }
                Node field;
                field.type = NODE_STRUCT_FIELD;
                field.varName = node.varName;
                field.op = node.op;
                field.right = node.right;  // a[i].x = v
                field.line = node.line; field.column = node.column; field.file = node.file;
                const std::string fieldType = expression(field);
                if (fieldType == "<error>") { expression(*node.left); break; }
                const std::string actual = expression(*node.left);
                if (actual != "<error>" && !compatible(fieldType, actual) && !constantFits(*node.left, fieldType) &&
                    !narrowCompound(field, *node.left, fieldType, actual))
                    error(*node.left, "поле ожидает тип '" + typeName(fieldType) +
                          "', получен '" + typeName(actual) + "'" + castHint(fieldType, actual));
                break;
            }
            const Symbol* object = use(node, node.varName);
            if (!object) {
                error(node, "неизвестная структура '" + node.varName + "'");
                expression(*node.left);
                break;
            }
            // Definite assignment: object must be initialized before field assignment
            checkInitialized(node, node.varName);
            auto structure = structs_.find(object->ty.structName);
            if (structure == structs_.end()) {
                error(node, "тип '" + typeName(object->ty.structName) + "' не имеет полей");
                expression(*node.left);
                break;
            }
            auto field = std::find_if(structure->second->children.begin(),
                                      structure->second->children.end(),
                [&](const Node& candidate) { return candidate.varName == node.op; });
            if (field == structure->second->children.end()) {
                error(node, "у структуры '" + node.varName + "' нет поля '" + node.op + "'");
                expression(*node.left);
                break;
            }
            if (object->isConst) {
                error(node, "нельзя изменять поле константной структуры '" + node.varName + "'");
                expression(*node.left);
                break;
            }
            const std::string fieldType = normalize(field->varType);
            if (node.left->type == NODE_NUMBER && node.left->varType.empty() && isInteger(fieldType) &&
                !node.left->value.empty() && node.left->value.find_first_of(".eE") == std::string::npos)
                node.left->varType = fieldType;  // s.name = 5 раньше «проходил» как string
            const std::string actual = expression(*node.left);
            Node self;
            self.type = NODE_STRUCT_FIELD;
            self.varName = node.varName;
            self.op = node.op;
            if (actual != "<error>" && !compatible(fieldType, actual) && !constantFits(*node.left, fieldType) &&
                !narrowCompound(self, *node.left, fieldType, actual))
                error(*node.left, "поле ожидает тип '" + typeName(fieldType) +
                      "', получен '" + typeName(actual) + "'" + castHint(fieldType, actual));
            break;
        }
        case NODE_ARRAY_ASSIGN: {
            const Symbol* array = use(node, node.varName);
            rejectConstArray(node, array);
            if (!array) error(node, "неизвестный массив '" + node.varName + "'");
            // Definite assignment: array must be initialized before element assignment
            if (array) checkInitialized(node, node.varName);
            std::string index = expression(*node.left);
            if (!isInteger(index) && index != "<error>") error(*node.left, "индекс массива должен быть целым числом");
            // Compile-time bounds check for static arrays with constant integer index
            if (array && array->ty.isArray() && node.left->type == NODE_NUMBER && node.left->varType.empty()) {
                // constantInteger понимает 0x/0b/0o и `_`; stoll читал `0x5` как 0.
                const unsigned len = array->ty.arrayLength;
                if (const auto idx = constantInteger(*node.left); idx && (*idx < 0 || *idx >= len))
                    error(*node.left, "индекс массива выходит за границы [" + std::to_string(len) + "]");
            }
            std::string value = expression(*node.right);
            Node self;
            self.type = NODE_ARRAY_ACCESS;
            self.varName = node.varName;
            if (array && array->ty.isArray()) {
                std::string element = array->ty.element->toString();
                if (value != "<error>" && !compatible(element, value) && !constantFits(*node.right, element) &&
                    !narrowCompound(self, *node.right, element, value))
                    error(node, "элемент массива ожидает тип '" + typeName(element) + "'" + castHint(element, value));
            } else if (array && array->isDynArray) {
                const std::string& element = array->dynArrayElementType;
                if (value != "<error>" && !element.empty() && !compatible(element, value) &&
                    !constantFits(*node.right, element) && !narrowCompound(self, *node.right, element, value))
                    error(node, "элемент массива ожидает тип '" + typeName(element) + "'" + castHint(element, value));
            } else if (array && array->ty.isPtr()) {
                checkPointeeStore(node, node.varName, *array, *node.right, value);
            } else if (array) error(node, "значение не является массивом");
            break;
        }
        case NODE_DEREF_ASSIGN: {
            const Symbol* pointer = use(node, node.varName);
            if (!pointer) error(node, "неизвестный указатель '" + node.varName + "'");
            else if (!pointer->ty.isPtr()) error(node, "запись через разыменование требует тип ptr");
            const std::string value = expression(*node.left);
            if (pointer && pointer->ty.isPtr()) checkPointeeStore(node, node.varName, *pointer, *node.left, value);
            break;
        }
        case NODE_RETURN: {
            if (returnTypes_.empty()) {
                error(node, "return разрешён только внутри функции");
                if (node.left) expression(*node.left);
                break;
            }
            if (returnTypes_.back() == "void") {
                if (node.left) {
                    error(node, "функция без типа результата ничего не возвращает: укажите тип, например 'fn int имя(…)'");
                    expression(*node.left);
                }
                break;
            }
            if (!node.left) {
                error(node, "функция с результатом должна вернуть значение");
                break;
            }
            if (const sc::Type result = ty(returnTypes_.back()); result.isDynArray()) {  // fn int[] f() (D-018)
                if (const std::string problem = checkArrayValue(*node.left, result.element->toString());
                    !problem.empty())
                    error(node, "return: " + problem);
                break;
            }
            // D-007 / аудит 07f: адрес локальной переменной после return висит.
            if (node.left->type == NODE_ADDR && isFunctionLocal(node.left->varName))
                error(node, "возврат адреса локальной переменной '" + node.left->varName +
                      "': после выхода из функции она перестаёт существовать");
            if (node.left->type == NODE_IDENTIFIER) {
                const Symbol* returned = use(node, node.left->value);
                if (returned && returned->pointsToLocal)
                    error(node, "возврат '" + node.left->value +
                          "' — адреса локальной переменной: после выхода из функции она перестаёт существовать");
                if (returned && returned->isDynArray) {
                    error(node, "возврат массива: объявите результат функции массивом, например 'fn int[] имя(…)'");
                    break;
                }
            }
            std::string actual = expression(*node.left);
            const std::string expectedReturnType = returnTypes_.back();
            bool fittingLiteral = node.left->type == NODE_NUMBER &&
                isInteger(expectedReturnType) &&
                literalFits(expectedReturnType, node.left->value);
            if (node.left->type == NODE_NUMBER && isInteger(expectedReturnType) && !fittingLiteral)
                error(node, "числовой литерал возврата вне диапазона типа '" + typeName(expectedReturnType) + "'");
            else if (actual != "<error>" && !compatible(expectedReturnType, actual) && !fittingLiteral &&
                     !constantFits(*node.left, expectedReturnType))
                error(node, "функция должна вернуть '" + typeName(returnTypes_.back()) +
                      "', получено '" + typeName(actual) + "'" + castHint(expectedReturnType, actual));
            break;
        }
        case NODE_FUNC_CALL:
            call(node);
            break;
        case NODE_SLOV: {
            const std::string printed = node.left ? expression(*node.left) : "<error>";
            // Символ печатается символом, а не кодом: char, литерал 'a', тернарный из символов
            // (литерал 'a' имеет тип u8, поэтому смотрим и на форму выражения).
            if (printed == "char" || (printed == "u8" && charLike(*node.left))) node.varType = "char";
            if (printed == "void") {
                error(*node.left, "print: функция без результата (void) не возвращает значения");
                break;
            }
            // Массив, структура и файл печатью не выводятся: раньше codegen падал (массив)
            // или печатал мусор (поле структуры, байты FILE*, адрес динамического массива).
            const Symbol* named = node.left->type == NODE_IDENTIFIER ? lookup(node.left->value) : nullptr;
            const sc::Type printedTy = ty(printed);
            if ((named && named->isDynArray) || printedTy.isArray() || printedTy.isDynArray() ||
                printedTy.isStruct() || printedTy.isFile())
                error(*node.left, "print: " + (named && named->isDynArray ? std::string("динамический массив")
                                    : printedTy.isArray() ? std::string("массив")
                                    : "значение типа '" + typeName(printed) + "'") +
                      " напечатать нельзя: печатайте элементы или поля");
            break;
        }
        case NODE_IF: {
            std::string condition = expression(*node.left);
            if (condition != "bool" && !isInteger(condition) && condition != "<error>")
                error(*node.left, "условие if должно быть bool или int");
            const auto beforeIf = droppedState();
            scopes_.emplace_back();
            checkStatements(node.children);
            scopes_.pop_back();
            const auto afterThen = droppedState();
            setDroppedState(beforeIf, false);
            scopes_.emplace_back();
            checkStatements(node.elseChildren);
            scopes_.pop_back();
            setDroppedState(afterThen, true);  // освобождено хотя бы в одной ветке
            break;
        }
        case NODE_LOOP_N: {
            std::string count = expression(*node.left);
            if (!isInteger(count) && count != "<error>") error(*node.left, "число итераций должно иметь целочисленный тип");
            ++loopDepth_;
            const size_t savedBarrier = dropBarrier_;
            dropBarrier_ = scopes_.size();
            scopes_.emplace_back();
            checkStatements(node.children);
            scopes_.pop_back();
            --loopDepth_;
            dropBarrier_ = savedBarrier;
            break;
        }
        case NODE_LOOP_W:
        case NODE_WHILE: {
            std::string condition = expression(*node.left);
            if (condition != "bool" && !isInteger(condition) && condition != "<error>") error(*node.left, "условие цикла должно быть bool или int");
            ++loopDepth_;
            const size_t savedBarrier = dropBarrier_;
            dropBarrier_ = scopes_.size();
            scopes_.emplace_back();
            checkStatements(node.children);
            scopes_.pop_back();
            --loopDepth_;
            dropBarrier_ = savedBarrier;
            break;
        }
        case NODE_FOR_EACH: {
            const std::string collectionType = expression(*node.left);
            if (collectionType.rfind("array<", 0) != 0) {
                error(*node.left, "for-each сейчас требует статический массив с известной длиной");
                break;
            }
            const std::string elementType = collectionType.substr(6, collectionType.size() - 7);
            scopes_.emplace_back();
            declare(node.varName, elementType, node, false, true);
            ++loopDepth_;
            const size_t savedBarrier = dropBarrier_;
            dropBarrier_ = scopes_.size() - 1;  // область переменной цикла
            checkStatements(node.children);
            --loopDepth_;
            dropBarrier_ = savedBarrier;
            scopes_.pop_back();
            break;
        }
        case NODE_FOR_RANGE: {
            const std::string startType = expression(*node.left);
            const std::string endType = expression(*node.right);
            if (!isInteger(startType) && startType != "<error>")
                error(*node.left, "граница for должна иметь целочисленный тип");
            if (!isInteger(endType) && endType != "<error>")
                error(*node.right, "граница for должна иметь целочисленный тип");
            scopes_.emplace_back();
            declare(node.varName, startType == "<error>" ? "i32" : startType, node, false, true);
            ++loopDepth_;
            const size_t savedBarrier = dropBarrier_;
            dropBarrier_ = scopes_.size() - 1;  // область переменной цикла
            checkStatements(node.children);
            --loopDepth_;
            dropBarrier_ = savedBarrier;
            scopes_.pop_back();
            break;
        }
        case NODE_LOOP: {
            ++loopDepth_;
            const size_t savedBarrier = dropBarrier_;
            dropBarrier_ = scopes_.size();
            scopes_.emplace_back();
            checkStatements(node.children);
            scopes_.pop_back();
            --loopDepth_;
            dropBarrier_ = savedBarrier;
            break;
        }
        case NODE_BREAK:
        case NODE_CONTINUE:
            if (loopDepth_ == 0) error(node, "оператор цикла находится вне цикла");
            break;
        case NODE_FUNC_DEF:
            error(node, "вложенные определения функций пока не поддерживаются");
            break;
        case NODE_STRUCT_DEF:
        case NODE_IMPORT:
            break;
        case NODE_DEFER: {
            switch (node.left->type) {
            case NODE_RETURN: case NODE_BREAK: case NODE_CONTINUE:
            case NODE_VAR_DECL: case NODE_ARRAY_DECL: case NODE_STRUCT_INSTANCE:
            case NODE_FUNC_DEF: case NODE_STRUCT_DEF: case NODE_IMPORT:
                error(node, "defer: переходы управления и объявления пока запрещены");
                break;
            case NODE_DROP:
                if (node.left->varName.find('.') != std::string::npos)
                    error(node, "defer drop поля не нужен: массив в поле освобождается вместе со структурой");
                else if (Symbol* symbol = checkDrop(*node.left)) symbol->deferDropLine = node.line;
                break;
            case NODE_ASSIGN: case NODE_ARRAY_ASSIGN: case NODE_DEREF_ASSIGN:
            case NODE_STRUCT_FIELD_ASSIGN: case NODE_FUNC_CALL: case NODE_SLOV:
            case NODE_DELETE: case NODE_FILE_WRITE: case NODE_FILE_CLOSE:
            case NODE_ARRAY_PUSH: case NODE_ARRAY_POP: case NODE_ARRAY_RESIZE:
            case NODE_INPUT: {
                // Validate now, but do not make delayed assignment effective now.
                const auto savedScopes = scopes_;
                checkNode(*node.left);
                scopes_ = savedScopes;
                break;
            }
            default:
                error(node, "defer: ожидается обычная операция или вызов");
                break;
            }
            break;
        }
        case NODE_DROP:
            if (Symbol* symbol = checkDrop(node)) {
                symbol->droppedLine = node.line;
                symbol->releasedBy = "drop";
            }
            break;
        case NODE_DELETE: {
            // После del память из new недоступна по этому имени, как после drop
            // (решение владельца № 6); новое присваивание имени снова делает его пригодным.
            size_t index = 0;
            Symbol* symbol = lookupMutable(node.varName, &index);
            if (!symbol) error(node, "неизвестное имя '" + node.varName + "'");
            else if (symbol->droppedLine)
                error(node, "повторный del '" + node.varName + "' (уже освобождено в строке " +
                      std::to_string(symbol->droppedLine) + ")");
            else if (symbol->ty.isArray() && !symbol->isDynArray)
                error(node, "освобождать нечего: массив '" + node.varName +
                      "' не создан через new (его память освобождается сама)");
            else if (symbol->ty.isStr())  // D-014: память строки — не ручной режим
                error(node, "del для строки не пишется: '" + node.varName +
                      "' — string, её память освобождает язык");
            else if (!symbol->isDynArray && !symbol->ty.isPtr())
                error(node, "освободить можно только динамический массив или ptr");
            else if (symbol->pointsToVariable)
                error(node, "освобождать нечего: '" + node.varName +
                      "' указывает на переменную (&…) или строку (data/as_ptr), а не на память из new");
            else if (index >= dropBarrier_) {  // del внешнего имени внутри цикла/функции — без отметки
                symbol->droppedLine = node.line;
                symbol->releasedBy = "del";
            }
            break;
        }
        case NODE_ARRAY_PUSH: {
            const Symbol* array = use(node, node.varName);
            rejectConstArray(node, array);
            if (!array) {
                error(node, "неизвестный массив '" + node.varName + "'");
                if (!node.args.empty()) expression(node.args[0]);
                break;
            }
            if (!array->isDynArray) {
                error(node, "push поддерживается только для динамических массивов");
                if (!node.args.empty()) expression(node.args[0]);
                break;
            }
            if (node.left) {
                std::string value = expression(*node.left);
                if (value != "<error>" && !array->dynArrayElementType.empty() &&
                    !compatible(array->dynArrayElementType, value))
                    error(*node.left, "элемент массива ожидает тип '" +
                          typeName(array->dynArrayElementType) + "'");
            }
            break;
        }
        case NODE_ARRAY_POP: {
            const Symbol* array = use(node, node.varName);
            rejectConstArray(node, array);
            if (!array) error(node, "неизвестный массив '" + node.varName + "'");
            else if (!array->isDynArray) error(node, "pop поддерживается только для динамических массивов");
            break;
        }
        case NODE_ARRAY_RESIZE: {
            const Symbol* array = use(node, node.varName);
            rejectConstArray(node, array);
            if (!array) {
                error(node, "неизвестный массив '" + node.varName + "'");
                if (!node.args.empty()) expression(node.args[0]);
                break;
            }
            if (!array->isDynArray) {
                error(node, "resize поддерживается только для динамических массивов");
                if (!node.args.empty()) expression(node.args[0]);
                break;
            }
            if (!node.args.empty()) {
                std::string size = expression(node.args[0]);
                if (size != "<error>" && !ty(size).isInteger())
                    error(node.args[0], "размер массива должен быть целым числом");
            }
            break;
        }
        case NODE_ARRAY_LEN:
        case NODE_ARRAY_CAP: {
            const Symbol* array = use(node, node.varName);
            if (!array) error(node, "неизвестный массив '" + node.varName + "'");
            else if (!array->isDynArray && !array->ty.isArray())
                error(node, "'" + std::string(node.type == NODE_ARRAY_LEN ? "len" : "cap") +
                      "' доступен только для массивов");
            break;
        }
        case NODE_FILE_CLOSE: {
            const Symbol* symbol = use(node, node.varName);
            if (!symbol) error(node, "неизвестный файл '" + node.varName + "'");
            else if (!symbol->ty.isFile()) error(node, "ожидался file, получен '" + typeName(symbol->ty.toString()) + "'");
            else if (!symbol->droppedLine) {
                if (Symbol* file = checkDrop(node, "close")) {
                    file->droppedLine = node.line;
                    file->releasedBy = "close";
                }
            }
            break;
        }
        case NODE_FILE_WRITE: {
            const Symbol* symbol = use(node, node.varName);
            if (!symbol) error(node, "неизвестный файл '" + node.varName + "'");
            else if (!symbol->ty.isFile()) error(node, "ожидался file, получен '" + typeName(symbol->ty.toString()) + "'");
            if (node.left) {
                std::string value = expression(*node.left);
                if (node.type == NODE_FILE_WRITE && value != "str" && value != "<error>") error(node, "запись в файл принимает string");
            }
            break;
        }
        default:
            expression(node);
            break;
        }
    }
};

} // namespace

bool checkSemantics(const Node& program, const std::string& filename) {
    Checker checker(filename);
    return checker.run(program);
}
