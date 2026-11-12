#ifndef SC_TYPE_H
#define SC_TYPE_H

#include <memory>
#include <string>
#include <string_view>

namespace llvm {
class LLVMContext;
class Type;
}

// LLVM тип для IR-генерации (реализация в codegen.cpp).

namespace sc {

// Единое внутреннее представление типа.
// Заменяет строковые сравнения (normalize/compatible/isInteger) в semantic.cpp.
enum class TypeKind {
    Error,
    Integer,
    Float,
    Bool,
    Char,
    Str,
    Ptr,
    Void,
    File,
    Struct,
    Array,
    DynArray,
};

struct Type {
    TypeKind kind = TypeKind::Error;
    unsigned width = 0;        // биты: для Integer/Float
    bool signed_ = false;      // для Integer
    std::string structName;     // для Struct
    std::unique_ptr<Type> element; // элемент для Array / DynArray
    unsigned arrayLength = 0;   // количество элементов для Array

    Type() = default;
    explicit Type(TypeKind k) : kind(k) {}

    // Глубокое копирование (element — unique_ptr, копируется рекурсивно).
    Type(const Type& other)
        : kind(other.kind), width(other.width), signed_(other.signed_),
          structName(other.structName), element(other.element ? std::make_unique<Type>(*other.element) : nullptr),
          arrayLength(other.arrayLength) {}

    Type& operator=(const Type& other) {
        if (this != &other) {
            kind = other.kind;
            width = other.width;
            signed_ = other.signed_;
            structName = other.structName;
            element = other.element ? std::make_unique<Type>(*other.element) : nullptr;
            arrayLength = other.arrayLength;
        }
        return *this;
    }

    static Type error() { return Type(); }

    static Type integer(unsigned width, bool isSigned) {
        Type t;
        t.kind = TypeKind::Integer;
        t.width = width;
        t.signed_ = isSigned;
        return t;
    }
    static Type floatTy(unsigned width) {
        Type t;
        t.kind = TypeKind::Float;
        t.width = width;
        return t;
    }
    static Type boolTy() {
        Type t;
        t.kind = TypeKind::Bool;
        t.width = 8;
        return t;
    }
    static Type charTy() {
        Type t;
        t.kind = TypeKind::Char;
        t.width = 8;
        return t;
    }
    static Type strTy() {
        Type t;
        t.kind = TypeKind::Str;
        return t;
    }
    static Type ptrTy() {
        Type t;
        t.kind = TypeKind::Ptr;
        return t;
    }
    static Type voidTy() {
        Type t;
        t.kind = TypeKind::Void;
        return t;
    }
    static Type fileTy() {
        Type t;
        t.kind = TypeKind::File;
        return t;
    }
    static Type structTy(std::string_view name) {
        Type t;
        t.kind = TypeKind::Struct;
        t.structName = name;
        return t;
    }
    static Type arrayTy(const Type& element, unsigned length) {
        Type t;
        t.kind = TypeKind::Array;
        t.element = std::make_unique<Type>(element);
        t.arrayLength = length;
        return t;
    }
    static Type dynArrayTy(const Type& element) {
        Type t;
        t.kind = TypeKind::DynArray;
        t.element = std::make_unique<Type>(element);
        return t;
    }

    // Parse a normalized type name ("i32", "f64", "array<i32>", "MyStruct", ...).
    static Type from(std::string_view raw);

    bool isError()   const { return kind == TypeKind::Error; }
    bool isInteger() const { return kind == TypeKind::Integer; }
    bool isUnsigned()const { return isInteger() && !signed_; }
    bool isFloat()   const { return kind == TypeKind::Float; }
    bool isBool()    const { return kind == TypeKind::Bool; }
    bool isChar()    const { return kind == TypeKind::Char; }
    bool isStr()     const { return kind == TypeKind::Str; }
    bool isPtr()     const { return kind == TypeKind::Ptr; }
    bool isVoid()    const { return kind == TypeKind::Void; }
    bool isFile()    const { return kind == TypeKind::File; }
    bool isStruct()  const { return kind == TypeKind::Struct; }
    bool isArray()   const { return kind == TypeKind::Array; }
    bool isDynArray()const { return kind == TypeKind::DynArray; }

    bool isSigned() const { return signed_; }

    bool isScalar() const {
        return isInteger() || isFloat() || isBool() || isChar();
    }
    bool isNumeric() const {
        return isInteger() || isFloat();
    }
    bool isKnown() const { return kind != TypeKind::Error; }
    bool isIntegerOrBool() const { return isInteger() || isBool(); }

    // LLVM тип для IR-генерации (реализация в codegen.cpp).
    llvm::Type* llvmType(llvm::LLVMContext& ctx) const;

    // Проверяет, что текстовый литерал вписывается в тип (диапазон, формат).
    bool fitsLiteral(const std::string& text) const;

    // Проверяет, что числовой литерал может использоваться в арифметическом
    // контексте для этого типа (отвергает узкие типы narrower 32 бит и
    // float-литералы для целевых типов).
    bool fitsArithmeticLiteral(const std::string& text) const;

    // Неявное преобразование без потерь данных ( widening only ).
    bool compatible(const Type& other) const;

    // Тип результата бинарной операции (numeric promotion).
    Type commonType(const Type& other) const;

    // Арифметическое продвижение: целые narrower 32 бит -> i32,
    // f32/f64 остаются собой, bool/char -> i32.
    Type promote() const;

    std::string toString() const;
};

} // namespace sc

#endif