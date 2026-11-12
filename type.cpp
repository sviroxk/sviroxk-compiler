#include "type.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace sc {

static Type normalizeName(std::string_view raw) {
    if (raw == "int" || raw == "i32" || raw == "rox") return Type::integer(32, true);
    if (raw == "uint" || raw == "u32") return Type::integer(32, false);
    if (raw == "short" || raw == "i16") return Type::integer(16, true);
    if (raw == "ushort" || raw == "u16") return Type::integer(16, false);
    if (raw == "long" || raw == "i64" || raw == "rox64") return Type::integer(64, true);
    if (raw == "ulong" || raw == "u64") return Type::integer(64, false);
    if (raw == "byte" || raw == "u8") return Type::integer(8, false);
    if (raw == "i8") return Type::integer(8, true);
    if (raw == "float" || raw == "f32" || raw == "dor") return Type::floatTy(32);
    if (raw == "double" || raw == "f64") return Type::floatTy(64);
    if (raw == "string" || raw == "str" || raw == "cos") return Type::strTy();
    if (raw == "bool" || raw == "bue") return Type::boolTy();
    if (raw == "chr" || raw == "char") return Type::charTy();
    if (raw == "ptr") return Type::ptrTy();
    if (raw == "void") return Type::voidTy();
    if (raw == "file") return Type::fileTy();
    if (raw.starts_with("array<")) {
        return Type::arrayTy(Type::from(raw.substr(6, raw.size() - 7)), 0);
    }
    return Type::structTy(raw);
}

Type Type::from(std::string_view raw) {
    if (raw.empty() || raw == "<error>") return Type::error();
    return normalizeName(raw);
}

bool Type::compatible(const Type& other) const {
    if (isError() || other.isError()) return true;
    if (kind == other.kind) {
        if (isStruct()) return structName == other.structName;
        if (isArray() && other.isArray()) return element->compatible(*other.element);
        if (isDynArray() && other.isDynArray()) return element->compatible(*other.element);
        // Неявно — только без потерь: расширение той же знаковости или unsigned -> более широкий signed.
        if (isInteger()) {
            if (signed_ == other.signed_) return width >= other.width;
            return signed_ && width > other.width;
        }
        if (isFloat()) return width >= other.width;
        return true;
    }
    // char <-> u8 (historical alias)
    if ((isChar() && other.isInteger() && other.width == 8 && !other.signed_) ||
        (isInteger() && width == 8 && !signed_ && other.isChar()))
        return true;
    // ptr <-> array / dyn array
    if (isPtr() && (other.isArray() || other.isDynArray())) return true;
    if (other.isPtr() && (isArray() || isDynArray())) return true;
    // f64 <- f32 (widening)
    if (isFloat() && other.isFloat() && width >= other.width) return true;
    // integer widening only (same signedness, wider target)
    // integer -> float: в f64 любое целое, в f32 — только до 16 бит (точно представимо)
    if (isFloat() && other.isInteger()) return width == 64 || other.width <= 16;
    return false;
}

Type Type::commonType(const Type& other) const {
    if (isError() || other.isError()) return Type::error();
    if (isFloat() || other.isFloat()) {
        const unsigned w = std::max(width, other.width);
        return Type::floatTy(w == 64 ? 64 : 32);
    }
    if (isInteger() && other.isInteger()) {
        if (signed_ != other.signed_) {
            // Mixed signedness: narrow operands promote to i32; otherwise the
            // wider signed type wins if it can hold the unsigned range, else error.
            const unsigned unsignedBits = isUnsigned() ? width : other.width;
            const unsigned signedBits = isUnsigned() ? other.width : width;
            if (std::max(unsignedBits, signedBits) < 32) return Type::integer(32, true);
            if (signedBits > unsignedBits) return Type::integer(signedBits, true);
            return Type::error();
        }
        return Type::integer(std::max(width, other.width), signed_);
    }
    return Type::error();
}

Type Type::promote() const {
    if (isError()) return Type::error();
    if (isInteger() && width < 32) return Type::integer(32, signed_);
    if (isBool() || isChar()) return Type::integer(32, true);
    return *this;
}

std::string Type::toString() const {
    switch (kind) {
    case TypeKind::Error: return "<error>";
    case TypeKind::Integer: return (signed_ ? "i" : "u") + std::to_string(width);
    case TypeKind::Float:   return "f" + std::to_string(width);
    case TypeKind::Bool:    return "bool";
    case TypeKind::Char:    return "char";
    case TypeKind::Str:     return "str";
    case TypeKind::Ptr:     return "ptr";
    case TypeKind::Void:    return "void";
    case TypeKind::File:    return "file";
    case TypeKind::Struct:  return structName;
    case TypeKind::Array:   return "array<" + (element ? element->toString() : "?") + ">";
    case TypeKind::DynArray: return "dyn<" + (element ? element->toString() : "?") + ">";
    }
    return "<unknown>";
}

bool Type::fitsLiteral(const std::string& text) const {
    if (text.empty()) return false;
    if (isFloat()) {
        try {
            size_t parsed = 0;
            double value = std::stod(text, &parsed);
            if (parsed != text.size() || !std::isfinite(value)) return false;
            return width != 32 || std::fabs(value) <= std::numeric_limits<float>::max();
        } catch (const std::logic_error&) { return false; }
    }
    if (isInteger()) {
        try {
            if (signed_) {
                size_t n = 0;
                long long v = std::stoll(text, &n);
                if (n != text.size()) return false;
                switch (width) {
                case 8:  return v >= -128 && v <= 127;
                case 16: return v >= -32768 && v <= 32767;
                case 32: return v >= INT32_MIN && v <= INT32_MAX;
                case 64: return true;
                default: return false;
                }
            } else {
                if (text[0] == '-') return false;
                size_t n = 0;
                unsigned long long v = std::stoull(text, &n);
                if (n != text.size()) return false;
                switch (width) {
                case 8:  return v <= 255;
                case 16: return v <= 65535;
                case 32: return v <= UINT32_MAX;
                case 64: return true;
                default: return false;
                }
            }
        } catch (const std::logic_error&) { return false; }
    }
    return false;
}

bool Type::fitsArithmeticLiteral(const std::string& text) const {
    if (text.empty()) return false;
    if (isError()) return false;
    // Reject narrow integer types (< 32 bits) for arithmetic literals
    if (isInteger() && width < 32) return false;
    // Reject float literals for integer targets
    if (isInteger()) {
        try {
            size_t parsed = 0;
            std::stod(text, &parsed);
            if (parsed == text.size()) return false; // floating point literal
        } catch (const std::logic_error&) {}
    }
    return fitsLiteral(text);
}

} // namespace sc