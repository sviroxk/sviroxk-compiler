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
    int dynArrayLength = 0;   // current length (for bounds checking)
    int dynArrayCapacity = 0; // allocated capacity
};

struct Function {
    std::string returnType;
    std::vector<std::string> parameters;
    sc::Type returnTy;
    int line;
    int column;
};

class Checker {
public:
    Checker(const std::string& filename) : filename_(filename) {
        scopes_.emplace_back();
    }

    bool run(const Node& program) {
        for (const Node& node : program.children) {
            if (node.type == NODE_STRUCT_DEF) {
                if (structs_.count(node.varName))
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
                for (const std::string& param : node.params) {
                    size_t separator = param.find(':');
                    fn.parameters.push_back(normalize(param.substr(0, separator)));
                }
                if (isBuiltinFunction(node.varName))
                    error(node, "имя функции '" + node.varName + "' зарезервировано встроенной функцией");
                else if (functions_.count(node.varName))
                    error(node, "повторное объявление функции '" + node.varName + "'");
                else
                    functions_[node.varName] = fn;
            }
        }

        for (const Node& node : program.children) {
            if (node.type != NODE_FUNC_DEF && node.type != NODE_STRUCT_DEF)
                checkNode(node);
        }
        for (const Node& node : program.children) {
            if (node.type == NODE_FUNC_DEF)
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
    std::map<std::string, const Node*> structs_;
    std::vector<std::string> returnTypes_;
    unsigned loopDepth_ = 0;

    static bool isBuiltinFunction(const std::string& name) {
        static const std::set<std::string> names = {
            "sqrt", "abs", "pow", "max", "min", "flo", "cel", "ron",
            "ran", "ar", "ser", "rep", "str"
        };
        return names.count(name) != 0;
    }

    static std::string normalize(const std::string& type) {
        return sc::Type::from(type).toString();
    }

    static std::string typeName(const std::string& type) {
        const std::string n = normalize(type);
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

    bool knownType(const std::string& raw) const {
        const sc::Type t = sc::Type::from(raw);
        if (!t.isKnown()) return false;
        return t.isScalar() || t.isStr() || t.isPtr() || t.isVoid() || t.isFile() ||
               t.isStruct() && structs_.count(t.structName);
    }

    static bool isInteger(const std::string& type) {
        return sc::Type::from(type).isInteger();
    }

    static sc::Type ty(const std::string& type) {
        return sc::Type::from(type);
    }

    bool compatible(const std::string& expectedRaw, const std::string& actualRaw) const {
        const sc::Type expected = sc::Type::from(expectedRaw);
        const sc::Type actual = sc::Type::from(actualRaw);
        if (expected.compatible(actual)) return true;
        // Историческая совместимость: char <-> u8, array<...> <-> ptr
        if ((expected.isChar() && actual.isInteger() && actual.width == 8 && !actual.signed_) ||
            (actual.isChar() && expected.isInteger() && expected.width == 8 && !expected.signed_))
            return true;
        if (expected.isPtr() && (actual.isArray() || actual.isDynArray())) return true;
        if (actual.isPtr() && (expected.isArray() || expected.isDynArray())) return true;
        return false;
    }

    bool declare(const std::string& name, const std::string& type,
                 const Node& node, bool isConst = false, bool isInitialized = false,
                 bool isDynArray = false, const std::string& dynArrayElementType = "") {
        auto& scope = scopes_.back();
        // Allow shadowing (variable in inner scope with same name as outer scope)
        // This is a common pattern and not an error
        if (scope.count(name)) {
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

    std::string expression(const Node& node) {
        switch (node.type) {
        case NODE_NUMBER: {
            const bool floatingLiteral = node.value.find_first_of(".eE") != std::string::npos;
            std::string literalType = !node.varType.empty()
                ? normalize(node.varType)
                : floatingLiteral ? "f64" : "i32";
            if (node.varType.empty() && !floatingLiteral) {
                if (literalFits("i32", node.value)) literalType = "i32";
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
            const Symbol* symbol = lookup(node.value);
            if (!symbol) {
                error(node, "неизвестное имя '" + node.value + "'");
                return "<error>";
            }
            checkInitialized(node, node.value);
            return symbol->type;
        }
        case NODE_STRUCT_FIELD: {
            const Symbol* object = lookup(node.varName);
            if (!object) {
                error(node, "неизвестное имя '" + node.varName + "'");
                return "<error>";
            }
            std::string currentStruct = object->ty.structName;
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
                if (dotPos == std::string::npos) return fieldType;
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
                    if (actual != "<error>" && !compatible(field.varType, actual))
                        error(*value, "поле '" + field.varName + "' ожидает тип '" +
                              typeName(normalize(field.varType)) + "', получен '" + typeName(actual) + "'");
                    if (value->left->type == NODE_NUMBER &&
                        !literalFits(normalize(field.varType), value->left->value))
                        error(*value, "литерал поля вне диапазона типа '" + typeName(normalize(field.varType)) + "'");
                }
                if (!value && named.count(field.varName)) named.erase(field.varName);
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
            const Symbol* symbol = lookup(node.varName);
            if (!symbol) {
                error(node, "неизвестный массив '" + node.varName + "'");
                return "<error>";
            }
            std::string indexType = expression(*node.left);
            if (!isInteger(indexType) && indexType != "<error>")
                error(*node.left, "индекс массива должен иметь целочисленный тип");
            if (symbol->ty.isArray())
                return symbol->ty.element->toString();
            if (symbol->isDynArray)
                return symbol->dynArrayElementType.empty() ? "i32" : symbol->dynArrayElementType;
            if (symbol->ty.isPtr()) return "i32";
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
            else if (!numeric.count(target) || !numeric.count(source))
                error(node, "явное преобразование между '" + typeName(source) +
                            "' и '" + typeName(target) + "' не поддерживается");
            else if (node.left->type == NODE_NUMBER &&
                     node.left->value.find_first_of(".eE") == std::string::npos &&
                     isInteger(target) && !literalFits(target, node.left->value))
                error(node, "числовой литерал вне диапазона типа '" + typeName(target) + "'");
            return target;
        }
        case NODE_ADDR: {
            const Symbol* symbol = lookup(node.varName);
            if (!symbol) {
                error(node, "невозможно получить адрес неизвестного имени '" + node.varName + "'");
                return "<error>";
            }
            return "ptr";
        }
        case NODE_DEREF: {
            const Symbol* symbol = lookup(node.varName);
            if (!symbol) {
                error(node, "неизвестный указатель '" + node.varName + "'");
                return "<error>";
            }
            if (!symbol->ty.isPtr()) {
                error(node, "разыменование требует тип ptr, получен '" + typeName(symbol->ty.toString()) + "'");
                return "<error>";
            }
            return "i32";
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
            return normalize(node.varType) == "str" ? "str" : "i32";
        case NODE_FILE_OPEN: {
            const std::string pathType = node.left ? expression(*node.left) : "<error>";
            if (pathType != "str" && pathType != "<error>")
                error(node, "имя файла должно иметь тип str");
            if (node.right) {
                const std::string modeType = expression(*node.right);
                if (modeType != "str" && modeType != "<error>")
                    error(*node.right, "режим открытия файла должен иметь тип str");
            }
            return "file";
        }
        case NODE_FILE_READ:
            if (!lookup(node.varName)) error(node, "неизвестный файл '" + node.varName + "'");
            return "str";
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
            return binary(node);
        case NODE_FUNC_CALL:
            return call(node);
        case NODE_ARRAY_PUSH: {
            const Symbol* array = lookup(node.varName);
            if (!array) {
                error(node, "неизвестный массив '" + node.varName + "'");
                for (const Node& arg : node.args) expression(arg);
                return "void";
            }
            if (!array->isDynArray && !array->ty.isArray()) {
                error(node, "'push' доступен только для массивов");
                return "void";
            }
            const std::string element = array->isDynArray
                ? array->dynArrayElementType : array->ty.element->toString();
            if (!node.args.empty()) {
                std::string value = expression(node.args[0]);
                if (value != "<error>" && !element.empty() && !compatible(element, value))
                    error(node.args[0], "элемент массива ожидает тип '" + typeName(element) + "'");
            }
            return "void";
        }
        case NODE_ARRAY_POP: {
            const Symbol* array = lookup(node.varName);
            if (!array) {
                error(node, "неизвестный массив '" + node.varName + "'");
                return "void";
            }
            if (!array->isDynArray && !array->ty.isArray()) {
                error(node, "'pop' доступен только для массивов");
                return "void";
            }
            return array->isDynArray ? array->dynArrayElementType : array->ty.element->toString();
        }
        case NODE_ARRAY_RESIZE: {
            const Symbol* array = lookup(node.varName);
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
            const Symbol* array = lookup(node.varName);
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
        const bool floatingLiteral = node.value.find_first_of(".eE") != std::string::npos;
        if (floatingLiteral && isInteger(target)) return false;
        if (target == "i8" || target == "i16" || target == "u8" || target == "u16")
            return false;
        return literalFits(target, node.value);
    }

    std::string binary(const Node& node) {
        std::string left = expression(*node.left);
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
        const bool narrowUnsignedPromotion = ty(left).isInteger() && ty(right).isInteger() &&
            ty(left).isUnsigned() != ty(right).isUnsigned() &&
            ty(left).width < 32 && ty(right).width < 32;
        if (!narrowUnsignedPromotion && node.right && fitsNumericLiteral(*node.left, right))
            left = right;
        else if (!narrowUnsignedPromotion && node.right && fitsNumericLiteral(*node.right, left))
            right = left;
        if (left == "<error>" || right == "<error>") return "<error>";
        if (node.op == "//") {
            if (!ty(left).isInteger() || !ty(right).isInteger()) {
                error(node, "оператор // требует целочисленные операнды");
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
                try {
                    const long long shift = std::stoll(node.right->value);
                    const unsigned width = ty(left).width;
                    if (shift < 0 || static_cast<unsigned long long>(shift) >= width) {
                        error(*node.right, "счётчик сдвига должен быть меньше ширины левого операнда");
                        return "<error>";
                    }
                } catch (...) {}
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
        if (comparison) {
            if (!leftTy.isNumeric() || !rightTy.isNumeric()) {
                error(node, "оператор сравнения принимает только числа");
                return "<error>";
            }
            if (leftTy.isInteger() && rightTy.isInteger() && leftTy.isUnsigned() != rightTy.isUnsigned()) {
                error(node, "нельзя сравнивать signed- и unsigned-типы без явного преобразования");
                return "<error>";
            }
            return "bool";
        }
        if (!leftTy.isNumeric() || !rightTy.isNumeric()) {
            error(node, "арифметический оператор '" + node.op + "' требует числовые операнды");
            return "<error>";
        }
        // Arithmetic promotion with mixed signedness:
        // - both narrower than 32 bits -> i32
        // - signed wider than unsigned -> signed type
        // - otherwise -> error (requires explicit cast)
        if (leftTy.isInteger() && rightTy.isInteger() &&
            leftTy.isUnsigned() != rightTy.isUnsigned()) {
            const unsigned unsignedBits = leftTy.isUnsigned() ? leftTy.width : rightTy.width;
            const unsigned signedBits = leftTy.isUnsigned() ? rightTy.width : leftTy.width;
            if (std::max(unsignedBits, signedBits) < 32) return "i32";
            if (signedBits > unsignedBits)
                return (leftTy.isUnsigned() ? rightTy : leftTy).toString();
            error(node, "нельзя смешивать signed- и unsigned-типы этой ширины без явного преобразования");
            return "<error>";
        }
        if (leftTy.isFloat() || rightTy.isFloat()) {
            const sc::Type result = leftTy.commonType(rightTy);
            if (!result.isError()) return result.toString();
        }
        if (leftTy.isInteger() && rightTy.isInteger()) {
            const sc::Type result = leftTy.commonType(rightTy);
            if (!result.isError()) return result.toString();
        }
        return "i32";
    }

    std::string call(const Node& node) {
        static const std::map<std::string, std::pair<size_t, size_t>> builtins = {
            {"sqrt", {1,1}}, {"abs", {1,1}}, {"pow", {2,2}},
            {"max", {2,2}}, {"min", {2,2}}, {"flo", {1,1}},
            {"cel", {1,1}}, {"ron", {1,1}}, {"ran", {0,0}},
            {"ar", {1,1}}, {"ser", {2,2}}, {"rep", {3,3}}, {"str", {1,1}}
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
                const bool expectsString = node.varName == "ar" || node.varName == "ser" ||
                    node.varName == "rep";
                if (expectsString && type != "str" && type != "<error>")
                    error(arg, "аргумент " + std::to_string(index + 1) + " функции '" +
                          node.varName + "' должен иметь тип str");
                else if (!expectsString && node.varName != "str" &&
                         !isInteger(type) && type != "f32" && type != "f64" &&
                         type != "char" && type != "<error>")
                    error(arg, "недопустимый тип аргумента функции '" + node.varName + "'");
            }
            if (node.varName == "ser") return "bool";
            if (node.varName == "ar" || node.varName == "ran") return "i32";
            if (node.varName == "str") return "str";
            if (node.varName == "rep") return "ptr";
            if (node.varName == "sqrt" || node.varName == "pow" ||
                node.varName == "flo" || node.varName == "cel" || node.varName == "ron") {
                sc::Type resultTy = sc::Type::from(argumentTypes.empty() ? "i32" : argumentTypes.front());
                for (size_t index = 1; index < argumentTypes.size(); ++index) {
                    const sc::Type argTy = sc::Type::from(argumentTypes[index]);
                    if (!argTy.isError() && !resultTy.isError()) {
                        resultTy = resultTy.commonType(argTy);
                    }
                }
                return resultTy.toString();
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
            std::string actual = expression(node.args[i]);
            const std::string expectedType = i < count
                ? normalize(function->second.parameters[i]) : std::string();
            const bool fittingLiteral = i < count && node.args[i].type == NODE_NUMBER &&
                literalFits(expectedType, node.args[i].value);
            if (i < count && actual != "<error>" &&
                !compatible(function->second.parameters[i], actual) && !fittingLiteral) {
                error(node.args[i], "аргумент " + std::to_string(i + 1) + " функции '" +
                      node.varName + "': ожидался '" + typeName(function->second.parameters[i]) +
                      "', получен '" + typeName(actual) + "'");
            }
        }
        return function->second.returnType;
    }

    void checkStruct(const Node& node) {
        std::set<std::string> fields;
        for (const Node& field : node.children) {
            if (!knownType(field.varType)) error(field, "неизвестный тип поля '" + field.varType + "'");
            if (!fields.insert(field.varName).second)
                error(field, "повторное объявление поля '" + field.varName + "'");
            std::string actual = expression(*field.left);
            if (actual != "<error>" && !compatible(field.varType, actual)) {
                bool fittingLiteral = field.left->type == NODE_NUMBER &&
                    literalFits(normalize(field.varType), field.left->value);
                if (!fittingLiteral)
                    error(field, "значение по умолчанию поля '" + field.varName + "' имеет тип '" +
                          typeName(actual) + "', ожидался '" + typeName(normalize(field.varType)) + "'");
            }
            if (field.left->type == NODE_NUMBER &&
                !literalFits(normalize(field.varType), field.left->value))
                error(field, "литерал поля вне диапазона типа '" + typeName(normalize(field.varType)) + "'");
        }
    }

    bool alwaysReturns(const std::vector<Node>& statements) const {
        if (statements.empty()) return false;
        const Node& last = statements.back();
        if (last.type == NODE_RETURN) return true;
        if (last.type == NODE_IF && !last.elseChildren.empty())
            return alwaysReturns(last.children) && alwaysReturns(last.elseChildren);
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
        scopes_.emplace_back();
        returnTypes_.push_back(normalize(node.varType));
        for (const std::string& param : node.params) {
            size_t separator = param.find(':');
            std::string type = param.substr(0, separator);
            std::string name = separator == std::string::npos ? "" : param.substr(separator + 1);
            if (!knownType(type)) error(node, "неизвестный тип параметра '" + type + "'");
            declare(name, type, node, false, true);
        }
        checkStatements(node.children);
        if (normalize(node.varType) != "void" && !alwaysReturns(node.children))
            error(node, "функция должна возвращать значение на всех путях выполнения");
        returnTypes_.pop_back();
        scopes_.pop_back();
    }

    void checkNode(const Node& node) {
        switch (node.type) {
        case NODE_VAR_DECL: {
            const bool inferType = node.varType.empty();
            if (!inferType && !knownType(node.varType)) error(node, "неизвестный тип переменной '" + node.varType + "'");
            const std::string declaredType = normalize(node.varType);
            const bool contextualIntegerLiteral = node.left->type == NODE_NUMBER &&
                node.left->varType.empty() &&
                node.left->value.find_first_of(".eE") == std::string::npos &&
                isInteger(declaredType);
            std::string actual = contextualIntegerLiteral ? declaredType : expression(*node.left);
            const std::string effectiveType = inferType ? actual : declaredType;
            if (inferType && actual != "<error>") node.varType = actual;
            const bool numericLiteral = node.left->type == NODE_NUMBER &&
                (isInteger(effectiveType) || effectiveType == "f32" || effectiveType == "f64");
            const bool literalInRange = numericLiteral && literalFits(effectiveType, node.left->value);
            if (numericLiteral && !literalInRange) {
                error(*node.left, "числовой литерал вне диапазона типа '" + typeName(effectiveType) + "'");
            } else if (!inferType && actual != "<error>" && !compatible(node.varType, actual) && !literalInRange) {
                error(node, "переменная '" + node.varName + "' имеет тип '" +
                      typeName(effectiveType) + "', но инициализируется значением типа '"+
                      typeName(actual) + "'");
            }
            // Variable with initializer is considered initialized
            bool dynArrayInit = false;
            std::string dynElementType;
            int dynCapacity = 0;
            if (node.left->type == NODE_DYN_ARRAY) {
                dynArrayInit = true;
                dynElementType = normalize(node.left->varType);
                if (node.left->left && node.left->left->type == NODE_NUMBER) {
                    try { dynCapacity = static_cast<int>(std::stoll(node.left->left->value)); }
                    catch (...) {}
                }
            }
            declare(node.varName, node.varType, node, node.isConst, actual != "<error>",
                    dynArrayInit, dynElementType);
            if (dynArrayInit && scopes_.back().count(node.varName))
                scopes_.back()[node.varName].dynArrayCapacity = dynCapacity;
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
                actual = contextualIntegerLiteral ? element : expression(value);
                if (actual != "<error>" && !compatible(element, actual))
                    error(value, "элемент массива должен иметь тип '" + typeName(element) + "'");
            }
            declare(node.varName, "array<" + element + ">", node, false, true);
            break;
        }
        case NODE_STRUCT_INSTANCE: {
            auto structure = structs_.find(node.varType);
            if (structure == structs_.end()) {
                error(node, "неизвестная структура '" + node.varType + "'");
                break;
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
                    if (actual != "<error>" && !compatible(field.varType, actual))
                        error(*value, "поле '" + field.varName + "' ожидает тип '" +
                              typeName(normalize(field.varType)) + "', получен '" + typeName(actual) + "'");
                    if (value->left->type == NODE_NUMBER &&
                        !literalFits(normalize(field.varType), value->left->value))
                        error(*value, "литерал поля вне диапазона типа '" + typeName(normalize(field.varType)) + "'");
                }
                if (!value && named.count(field.varName)) named.erase(field.varName);
            }
            for (const auto& item : named) {
                bool exists = false;
                for (const Node& field : structure->second->children)
                    if (field.varName == item.first) exists = true;
                if (!exists) error(*item.second, "у структуры нет поля '" + item.first + "'");
            }
            if (position < positional.size()) error(node, "слишком много позиционных значений структуры");
            declare(node.varName, node.varType, node);
            for (const Node& field : structure->second->children)
                declare(node.varName + "." + field.varName, field.varType, node);
            break;
        }
        case NODE_ASSIGN: {
            const Symbol* symbol = lookup(node.varName);
            if (!symbol) {
                error(node, "присваивание неизвестному имени '" + node.varName + "'");
                expression(*node.left);
                break;
            }
            if (symbol && symbol->isConst) {
                error(node, "нельзя присвоить значение константе '" + node.varName + "'");
                expression(*node.left);
                break;
            }
            std::string expected = symbol->ty.toString();
            std::string actual = expression(*node.left);
            bool literalArithmeticFits = false;
            if (symbol && node.left->type == NODE_BINOP &&
                (node.left->op == "+" || node.left->op == "-" ||
                 node.left->op == "*" || node.left->op == "/" || node.left->op == "%")) {
                const Node* literal = nullptr;
                const Node* other = nullptr;
                if (isNumericLiteral(*node.left->left)) {
                    literal = node.left->left;
                    other = node.left->right;
                } else if (isNumericLiteral(*node.left->right)) {
                    literal = node.left->right;
                    other = node.left->left;
                }
                if (literal && other->type == NODE_IDENTIFIER && other->value == node.varName) {
                    const bool floatingLiteral = literal->value.find_first_of(".eE") != std::string::npos;
                    literalArithmeticFits = floatingLiteral
                        ? (expected == "f32" || expected == "f64") && literalFits(expected, literal->value)
                        : isInteger(expected) && literalFits(expected, literal->value);
                }
            }
            if (actual != "<error>" && !compatible(expected, actual) && !literalArithmeticFits)
                error(node, "нельзя присвоить '" + typeName(actual) + "' значению типа '" + typeName(expected) + "'");
            else
                markInitialized(node.varName);
            break;
        }
        case NODE_STRUCT_FIELD_ASSIGN: {
            const Symbol* object = lookup(node.varName);
            if (!object) {
                error(node, "неизвестная структура '" + node.varName + "'");
                expression(*node.left);
                break;
            }
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
            if (node.left->type == NODE_NUMBER && node.left->varType.empty() &&
                !node.left->value.empty() && node.left->value.find_first_of(".eE") == std::string::npos)
                node.left->varType = fieldType;
            const std::string actual = expression(*node.left);
            if (actual != "<error>" && !compatible(fieldType, actual))
                error(*node.left, "поле ожидает тип '" + typeName(fieldType) +
                      "', получен '" + typeName(actual) + "'");
            break;
        }
        case NODE_ARRAY_ASSIGN: {
            const Symbol* array = lookup(node.varName);
            if (!array) error(node, "неизвестный массив '" + node.varName + "'");
            std::string index = expression(*node.left);
            if (!isInteger(index) && index != "<error>") error(*node.left, "индекс массива должен быть целым числом");
            std::string value = expression(*node.right);
            if (array && array->ty.isArray()) {
                std::string element = array->ty.element->toString();
                if (value != "<error>" && !compatible(element, value)) error(node, "элемент массива ожидает тип '" + typeName(element) + "'");
            } else if (array && array->isDynArray) {
                const std::string& element = array->dynArrayElementType;
                if (value != "<error>" && !element.empty() && !compatible(element, value))
                    error(node, "элемент массива ожидает тип '" + typeName(element) + "'");
            } else if (array && !array->ty.isPtr()) error(node, "значение не является массивом");
            break;
        }
        case NODE_DEREF_ASSIGN: {
            const Symbol* pointer = lookup(node.varName);
            if (!pointer) error(node, "неизвестный указатель '" + node.varName + "'");
            else if (!pointer->ty.isPtr()) error(node, "запись через разыменование требует тип ptr");
            expression(*node.left);
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
                    error(node, "процедура без типа результата не может возвращать значение");
                    expression(*node.left);
                }
                break;
            }
            if (!node.left) {
                error(node, "функция с результатом должна вернуть значение");
                break;
            }
            std::string actual = expression(*node.left);
            const std::string expectedReturnType = returnTypes_.back();
            bool fittingLiteral = node.left->type == NODE_NUMBER &&
                isInteger(expectedReturnType) &&
                literalFits(expectedReturnType, node.left->value);
            if (node.left->type == NODE_NUMBER && isInteger(expectedReturnType) && !fittingLiteral)
                error(node, "числовой литерал возврата вне диапазона типа '" + typeName(expectedReturnType) + "'");
            else if (actual != "<error>" && !compatible(expectedReturnType, actual) && !fittingLiteral)
                error(node, "функция должна вернуть '" + typeName(returnTypes_.back()) +
                      "', получено '" + typeName(actual) + "'");
            break;
        }
        case NODE_FUNC_CALL:
            call(node);
            break;
        case NODE_SLOV:
            if (node.left) expression(*node.left);
            break;
        case NODE_IF: {
            std::string condition = expression(*node.left);
            if (condition != "bool" && !isInteger(condition) && condition != "<error>")
                error(*node.left, "условие if должно быть bool или int");
            scopes_.emplace_back();
            checkStatements(node.children);
            scopes_.pop_back();
            scopes_.emplace_back();
            checkStatements(node.elseChildren);
            scopes_.pop_back();
            break;
        }
        case NODE_LOOP_N: {
            std::string count = expression(*node.left);
            if (!isInteger(count) && count != "<error>") error(*node.left, "число итераций должно иметь целочисленный тип");
            ++loopDepth_;
            scopes_.emplace_back();
            checkStatements(node.children);
            scopes_.pop_back();
            --loopDepth_;
            break;
        }
        case NODE_LOOP_W:
        case NODE_WHILE: {
            std::string condition = expression(*node.left);
            if (condition != "bool" && !isInteger(condition) && condition != "<error>") error(*node.left, "условие цикла должно быть bool или int");
            ++loopDepth_;
            scopes_.emplace_back();
            checkStatements(node.children);
            scopes_.pop_back();
            --loopDepth_;
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
            checkStatements(node.children);
            --loopDepth_;
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
            checkStatements(node.children);
            --loopDepth_;
            scopes_.pop_back();
            break;
        }
        case NODE_LOOP: {
            ++loopDepth_;
            scopes_.emplace_back();
            checkStatements(node.children);
            scopes_.pop_back();
            --loopDepth_;
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
        case NODE_DELETE: {
            const Symbol* symbol = lookup(node.varName);
            if (!symbol) error(node, "неизвестное имя '" + node.varName + "'");
            else if (!symbol->isDynArray && !symbol->ty.isArray() && !symbol->ty.isPtr())
                error(node, "освободить можно только динамический массив или ptr");
            break;
        }
        case NODE_ARRAY_PUSH: {
            const Symbol* array = lookup(node.varName);
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
            if (!node.args.empty()) {
                std::string value = expression(node.args[0]);
                if (value != "<error>" && !array->dynArrayElementType.empty() &&
                    !compatible(array->dynArrayElementType, value))
                    error(node.args[0], "элемент массива ожидает тип '" +
                          typeName(array->dynArrayElementType) + "'");
            }
            break;
        }
        case NODE_ARRAY_POP: {
            const Symbol* array = lookup(node.varName);
            if (!array) error(node, "неизвестный массив '" + node.varName + "'");
            else if (!array->isDynArray) error(node, "pop поддерживается только для динамических массивов");
            break;
        }
        case NODE_ARRAY_RESIZE: {
            const Symbol* array = lookup(node.varName);
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
            const Symbol* array = lookup(node.varName);
            if (!array) error(node, "неизвестный массив '" + node.varName + "'");
            else if (!array->isDynArray && !array->ty.isArray())
                error(node, "'" + std::string(node.type == NODE_ARRAY_LEN ? "len" : "cap") +
                      "' доступен только для массивов");
            break;
        }
        case NODE_FILE_WRITE:
        case NODE_FILE_CLOSE: {
            const Symbol* symbol = lookup(node.varName);
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
