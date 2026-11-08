#ifndef SC_TYPEID_H
#define SC_TYPEID_H

// ---------------------------------------------------------------------------
// Unified type representation (ST §2.1, stage 2)
//
// The compiler currently carries types as std::string ("i32", "f64", "array<i32>",
// "struct.Point", ...).  That works but forces every consumer to re-derive the
// kind of a type (is it signed? how wide? is it an aggregate?) from the spelling.
//
// This header introduces a lightweight, interning-based TypeId layer that sits
// ON TOP of the existing string machinery.  It does NOT replace the string
// normalization yet — that is a later refactor.  What it provides:
//
//   * a single TypeKind enum (the 5-class taxonomy the spec asks for)
//   * a stable TypeId handle (uint32_t) for every canonical type the compiler
//     actually meets, plus the new architecture types usize / isize
//   * query helpers (isInteger, isSigned, width, isAggregate, ...) that every
//     downstream phase can call instead of re-deriving them from the spelling
//
// The mapping is total: every type the semantic checker can produce has a
// TypeId, so no call site is left with a "what kind is this?" question.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <string>

namespace sc {

enum class TypeKind : uint8_t {
    Primitive,   // i8/i16/i32/i64/u8/u16/u32/u64/f32/f64/bool/char/void
    Pointer,     // ptr, file, str (all lowered to pointer)
    Array,       // static [N x T] and dynamic array<T>
    Struct,      // user-defined struct (rec keyword)
    Function,    // first-class function type
};

struct TypeInfo {
    TypeKind kind;
    std::string name;        // canonical spelling, e.g. "i32", "array<i32>", "struct.Point"
    uint8_t bits = 0;        // bit width for primitives (0 = not a primitive)
    bool isSigned = false;   // signedness for integers
    bool isUnsigned = false; // convenience: u8/u16/u32/u64
    bool isAggregate = false;// arrays and structs
};

// Returns the canonical TypeId for a type name.  Interns on first call so
// repeated lookups are O(1) hash + one indirection.  Unknown names still get
// a TypeId (kind = Primitive, bits = 0) so callers never see a null.
uint32_t typeIdFor(const std::string& canonicalName);

// Resolve a raw (possibly aliased) name through the same canonicalization the
// semantic checker uses, then return its TypeId.
uint32_t typeIdForRaw(const std::string& rawName);

// Look up the full descriptor for a TypeId.  Never returns null.
const TypeInfo& typeInfo(uint32_t id);

// Convenience predicates — implemented in terms of the descriptor so every
// phase shares one definition of "integer", "signed", "aggregate".
bool isIntegerType(uint32_t id);
bool isSignedType(uint32_t id);
bool isUnsignedType(uint32_t id);
bool isFloatType(uint32_t id);
bool isAggregateType(uint32_t id);
bool isPointerLikeType(uint32_t id);
unsigned typeBits(uint32_t id);

// Architecture types required by the spec.  usize/isize are pointer-width
// integers (64 bits on the host); they are first-class in the type table.
uint32_t usizeTypeId();
uint32_t isizeTypeId();

// Common result of a binary operation between two integer types, following
// the "narrow integer arithmetic promoted to 32 bits" rule already in force.
// Returns the TypeId of the promoted result, or 0 if no promotion applies.
uint32_t promoteInt(uint32_t leftId, uint32_t rightId);

} // namespace sc

#endif