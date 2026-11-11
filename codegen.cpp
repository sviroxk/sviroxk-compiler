#include "codegen.h"
#include "type.h"
#include <llvm/Support/raw_ostream.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IR/GlobalVariable.h>
#include <map>
#include <vector>
#include <iostream>
#include <cstdint>
#include <climits>
#include <cstdlib>
#include <cmath>
#include <stdexcept>

namespace {

[[noreturn]] void codegenFailure(const Node& node, const std::string& message) {
    throw std::logic_error(node.file + ":" + std::to_string(node.line) + ":" +
                           std::to_string(node.column) + ": внутренняя ошибка codegen: " + message);
}

bool blockHasTerminator(const llvm::BasicBlock* block) {
    return block && !block->empty() && block->back().isTerminator();
}

} // namespace

// Тип значения, на которое указывает слот переменной: локальная (alloca) или глобальная.
llvm::Type* CodeGen::storageType(llvm::Value* storage) {
    if (auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(storage))
        return alloca->getAllocatedType();
    if (auto* global = llvm::dyn_cast<llvm::GlobalVariable>(storage))
        return global->getValueType();
    throw std::logic_error("внутренняя ошибка codegen: неизвестный вид хранилища переменной");
}

// Переменные кода верхнего уровня живут в глобальных переменных LLVM, чтобы их
// видели функции; локальные переменные функций остаются на стеке (alloca).
// Глобалы получают префикс "sc.global.", чтобы не конфликтовать с рантаймом C.
llvm::Value* CodeGen::createStorage(llvm::Type* type, const std::string& name) {
    if (!globalStorage)
        return builder.CreateAlloca(type, nullptr, name);
    return new llvm::GlobalVariable(module, type, false,
        llvm::GlobalValue::InternalLinkage, llvm::Constant::getNullValue(type),
        "sc.global." + name);
}

// Единая таблица функций C-рантайма: одно имя — одна сигнатура.
llvm::Function* CodeGen::runtimeFunction(const std::string& name) {
    if (llvm::Function* existing = module.getFunction(name)) return existing;
    llvm::Type* i32 = builder.getInt32Ty();
    llvm::Type* i64 = builder.getInt64Ty();
    llvm::Type* ptr = builder.getPtrTy();
    llvm::Type* f64 = builder.getDoubleTy();
    llvm::Type* voidTy = builder.getVoidTy();
    struct Signature { llvm::Type* result; std::vector<llvm::Type*> params; bool variadic; };
    const std::map<std::string, Signature> table = {
        {"puts",    {i32, {ptr}, false}},
        {"printf",  {i32, {ptr}, true}},
        {"scanf",   {i32, {ptr}, true}},
        {"abort",   {voidTy, {}, false}},
        {"exit",    {voidTy, {i32}, false}},
        {"malloc",  {ptr, {i64}, false}},
        {"calloc",  {ptr, {i64, i64}, false}},
        {"snprintf", {i32, {ptr, i64, ptr}, true}},
        {"free",    {voidTy, {ptr}, false}},
        {"memcpy",  {ptr, {ptr, ptr, i64}, false}},
        {"strlen",  {i64, {ptr}, false}},
        {"strcmp",  {i32, {ptr, ptr}, false}},
        {"strstr",  {ptr, {ptr, ptr}, false}},
        {"fopen",   {ptr, {ptr, ptr}, false}},
        {"fgets",   {ptr, {ptr, i32, ptr}, false}},
        {"fputs",   {i32, {ptr, ptr}, false}},
        {"fclose",  {i32, {ptr}, false}},
        {"rand",    {i32, {}, false}},
        {"pow",     {f64, {f64, f64}, false}},
        {"sqrt",    {f64, {f64}, false}},
        {"floor",   {f64, {f64}, false}},
        {"ceil",    {f64, {f64}, false}},
        {"round",   {f64, {f64}, false}},
    };
    auto entry = table.find(name);
    if (entry == table.end())
        throw std::logic_error("внутренняя ошибка codegen: функция рантайма '" + name +
                               "' отсутствует в таблице");
    llvm::FunctionType* type = llvm::FunctionType::get(
        entry->second.result, entry->second.params, entry->second.variadic);
    return llvm::Function::Create(type, llvm::Function::ExternalLinkage, name, module);
}

CodeGen::ScopeSnapshot CodeGen::saveScope() const {
    return {vars, unsignedVars, charVars, constVars, integerWidths, arrays, arrayLengths,
            arrayElementTypes, structTypesByVariable};
}

void CodeGen::restoreScope(const ScopeSnapshot& scope) {
    vars = scope.vars;
    unsignedVars = scope.unsignedVars;
    charVars = scope.charVars;
    constVars = scope.constVars;
    integerWidths = scope.integerWidths;
    arrays = scope.arrays;
    arrayLengths = scope.arrayLengths;
    arrayElementTypes = scope.arrayElementTypes;
    structTypesByVariable = scope.structTypesByVariable;
}

llvm::Value* CodeGen::getStringPtr(const std::string& str) {
    llvm::GlobalVariable* gv = builder.CreateGlobalString(str);
    return builder.CreateInBoundsGEP(gv->getValueType(), gv,
        {builder.getInt32(0), builder.getInt32(0)});
}

llvm::Type* sc::Type::llvmType(llvm::LLVMContext& ctx) const {
    switch (kind) {
    case TypeKind::Integer: return llvm::Type::getIntNTy(ctx, width);
    case TypeKind::Float:   return width == 64 ? llvm::Type::getDoubleTy(ctx) : llvm::Type::getFloatTy(ctx);
    case TypeKind::Bool:    return llvm::Type::getInt8Ty(ctx);
    case TypeKind::Char:    return llvm::Type::getInt8Ty(ctx);
    case TypeKind::Str:     return llvm::PointerType::get(ctx, 0);
    case TypeKind::Ptr:     return llvm::PointerType::get(ctx, 0);
    case TypeKind::Void:    return llvm::Type::getVoidTy(ctx);
    case TypeKind::File:    return llvm::PointerType::get(ctx, 0);
    case TypeKind::Struct:  return llvm::StructType::getTypeByName(ctx, structName);
    case TypeKind::Array:   return llvm::PointerType::get(ctx, 0);
    case TypeKind::DynArray: return llvm::PointerType::get(ctx, 0);
    default: break;
    }
    return llvm::Type::getInt32Ty(ctx);
}

llvm::Type* CodeGen::svType(const std::string& name) {
    return sc::Type::from(name).llvmType(context);
}

llvm::Value* CodeGen::castTo(llvm::Value* val, llvm::Type* ty, bool sourceUnsigned) {
    if (!val || !ty) return val;
    if (val->getType() == ty) return val;
    if (ty->isFloatingPointTy() && val->getType()->isIntegerTy(1))
        return builder.CreateUIToFP(val, ty);
    if (ty->isFloatingPointTy() && val->getType()->isIntegerTy())
        return sourceUnsigned ? builder.CreateUIToFP(val, ty) : builder.CreateSIToFP(val, ty);
    if (ty->isFloatTy() && val->getType()->isDoubleTy())
        return builder.CreateFPTrunc(val, ty);
    if (ty->isDoubleTy() && val->getType()->isFloatTy())
        return builder.CreateFPExt(val, ty);
    if (ty->isIntegerTy() && val->getType()->isIntegerTy() &&
        ty->getIntegerBitWidth() > val->getType()->getIntegerBitWidth()) {
        if (val->getType()->isIntegerTy(1) || ty->getIntegerBitWidth() == 1 || sourceUnsigned)
            return builder.CreateZExt(val, ty);
        return builder.CreateSExt(val, ty);
    }
    if (ty->isIntegerTy() && val->getType()->isIntegerTy() &&
        ty->getIntegerBitWidth() < val->getType()->getIntegerBitWidth())
        return builder.CreateTrunc(val, ty);
    if (ty->isIntegerTy(32) && val->getType()->isIntegerTy(1))
        return builder.CreateZExt(val, ty);
    if (ty->isIntegerTy(32) && val->getType()->isDoubleTy())
        return builder.CreateFPToSI(val, ty);
    if (ty->isIntegerTy(64) && val->getType()->isIntegerTy(32))
        return builder.CreateSExt(val, ty);
    if (ty->isIntegerTy(32) && val->getType()->isIntegerTy(64))
        return builder.CreateTrunc(val, ty);
    if (ty->isIntegerTy(32) && val->getType()->isIntegerTy() &&
        val->getType()->getIntegerBitWidth() < 32)
        return builder.CreateZExt(val, ty);
    if (ty->isPointerTy() && !val->getType()->isPointerTy())
        return val;
    return val;
}

llvm::Value* CodeGen::toBool(llvm::Value* value) {
    if (value->getType()->isIntegerTy(1)) return value;
    if (value->getType()->isIntegerTy())
        return builder.CreateICmpNE(value, llvm::ConstantInt::get(value->getType(), 0));
    if (value->getType()->isFloatingPointTy()) {
        llvm::Constant* zero = llvm::ConstantFP::get(value->getType(), 0.0);
        return builder.CreateFCmpONE(value, zero);
    }
    return builder.getFalse();
}

unsigned CodeGen::integerWidth(const Node& node) const {
    if (node.type == NODE_IDENTIFIER) {
        auto found = integerWidths.find(node.value);
        return found == integerWidths.end() ? 32 : found->second;
    }
    if (node.type == NODE_NUMBER) return 32;
    if (node.type == NODE_BINOP && node.left && node.right)
        return std::max(integerWidth(*node.left), integerWidth(*node.right));
    return 32;
}

// Ошибка выполнения: если failed истинно, печатает "SC runtime error: <message> at line L, column C"
// и завершает программу с кодом 1. detail (строка C) подставляется вместо %s в message.
void CodeGen::emitRuntimeFailureIf(llvm::Value* failed, const std::string& message,
                                   const Node& location, llvm::Value* detail) {
    llvm::Function* function = builder.GetInsertBlock()->getParent();
    llvm::BasicBlock* fail = llvm::BasicBlock::Create(context, "runtime_error", function);
    llvm::BasicBlock* ok = llvm::BasicBlock::Create(context, "runtime_ok", function);
    builder.CreateCondBr(failed, fail, ok);
    builder.SetInsertPoint(fail);
    const std::string text = "SC runtime error: " + message + " at line " +
        std::to_string(location.line) + ", column " + std::to_string(location.column);
    if (detail)
        builder.CreateCall(printfFunc, {getStringPtr(text + "\n"), detail});
    else
        builder.CreateCall(putsFunc, {getStringPtr(text)});
    builder.CreateCall(runtimeFunction("exit"), {builder.getInt32(1)});
    builder.CreateUnreachable();
    builder.SetInsertPoint(ok);
}

// Единственное место, где компилятор выделяет строковые буферы в куче (str(),
// строковый ввод, чтение файла). Владелец буфера — переменная, которой присвоен
// результат. Пока буфер не освобождается (утечка вместо висячего указателя);
// освобождение при выходе из области видимости — этап модели памяти.
llvm::Value* CodeGen::allocateStringBuffer(llvm::Value* bytes, const Node& location) {
    if (bytes->getType()->getIntegerBitWidth() < 64)
        bytes = builder.CreateZExt(bytes, builder.getInt64Ty());
    llvm::Value* buffer = builder.CreateCall(runtimeFunction("malloc"), {bytes});
    emitRuntimeFailureIf(builder.CreateICmpEQ(buffer,
        llvm::ConstantPointerNull::get(builder.getPtrTy())), "out of memory", location);
    builder.CreateStore(builder.getInt8(0), buffer);
    return buffer;
}

void CodeGen::emitArrayBoundsCheck(llvm::Value* index, llvm::Value* length,
                                   const Node& location) {
    if (index->getType()->isIntegerTy(1))
        index = builder.CreateZExt(index, builder.getInt32Ty());
    if (index->getType()->getIntegerBitWidth() < 64) {
        index = isUnsignedExpr(location) ? builder.CreateZExt(index, builder.getInt64Ty())
                                         : builder.CreateSExt(index, builder.getInt64Ty());
    }
    if (length->getType()->getIntegerBitWidth() < 64) {
        length = isUnsignedExpr(location) ? builder.CreateZExt(length, builder.getInt64Ty())
                                           : builder.CreateSExt(length, builder.getInt64Ty());
    }
    llvm::Value* nonNegative = isUnsignedExpr(location)
        ? builder.getTrue()
        : builder.CreateICmpSGE(index, builder.getInt64(0));
    llvm::Value* belowLength = builder.CreateICmpULT(index, length);
    llvm::Value* inBounds = builder.CreateAnd(nonNegative, belowLength);
    llvm::Function* function = builder.GetInsertBlock()->getParent();
    llvm::BasicBlock* valid = llvm::BasicBlock::Create(context, "index_valid", function);
    llvm::BasicBlock* invalid = llvm::BasicBlock::Create(context, "index_invalid", function);
    builder.CreateCondBr(inBounds, valid, invalid);

    builder.SetInsertPoint(invalid);
    const std::string message = "SC runtime error: array index out of bounds at line " +
        std::to_string(location.line) + ", column " + std::to_string(location.column);
    builder.CreateCall(putsFunc, {getStringPtr(message)});
    llvm::Function* exitF = runtimeFunction("exit");
    builder.CreateCall(exitF, {builder.getInt32(1)});
    builder.CreateUnreachable();
    builder.SetInsertPoint(valid);
}

llvm::StructType* CodeGen::dynArrayType() {
    llvm::StructType* existing = llvm::StructType::getTypeByName(context, "sc.dyn_array");
    if (existing) return existing;
    llvm::StructType* type = llvm::StructType::create(context, "sc.dyn_array");
    type->setBody({
        builder.getPtrTy(),        // data pointer
        builder.getInt64Ty(),      // length
        builder.getInt64Ty(),      // capacity
    }, false);
    return type;
}

void CodeGen::growDynArray(const std::string& varName, llvm::Value* newCapacity) {
    auto found = dynArrays.find(varName);
    if (found == dynArrays.end()) return;
    llvm::StructType* descType = dynArrayType();
    llvm::Value* desc = found->second;
    llvm::Type* elementType = dynArrayElementTypes[varName];
    if (!elementType) elementType = builder.getInt32Ty();
    llvm::Value* dataPtr = builder.CreateLoad(builder.getPtrTy(),
        builder.CreateStructGEP(descType, desc, 0));
    llvm::Function* mallocF = runtimeFunction("malloc");
    llvm::Value* bytes = builder.CreateMul(newCapacity,
        builder.getInt64(elementType->getPrimitiveSizeInBits() / 8));
    llvm::Value* newData = builder.CreateCall(mallocF, {bytes});
    llvm::Value* oldLength = builder.CreateLoad(builder.getInt64Ty(),
        builder.CreateStructGEP(descType, desc, 1));
    // Copy existing elements into the new buffer.
    llvm::Value* copyBytes = builder.CreateMul(oldLength,
        builder.getInt64(elementType->getPrimitiveSizeInBits() / 8));
    llvm::Function* memcpyF = runtimeFunction("memcpy");
    builder.CreateCall(memcpyF, {newData, dataPtr, copyBytes});
    // Free the old buffer.
    llvm::Function* freeF = runtimeFunction("free");
    builder.CreateCall(freeF, {dataPtr});
    builder.CreateStore(newData, builder.CreateStructGEP(descType, desc, 0));
    builder.CreateStore(newCapacity, builder.CreateStructGEP(descType, desc, 2));
}

bool CodeGen::isUnsignedExpr(const Node& node) const {
    if (node.type == NODE_NUMBER) return false;
    if (node.type == NODE_IDENTIFIER) {
        auto found = unsignedVars.find(node.value);
        return found != unsignedVars.end() && found->second;
    }
    if (node.type == NODE_BINOP && node.left && node.right) {
        if (node.op == ">" || node.op == "<" || node.op == "==" || node.op == "!=")
            return isUnsignedExpr(*node.left);
        return isUnsignedExpr(*node.left) || isUnsignedExpr(*node.right);
    }
    return false;
}

void CodeGen::initStructLiteral(llvm::Value* storage, llvm::StructType* aggregateType,
                                const std::string& structName, const Node& literal) {
    if (!structDefs.count(structName)) {
        codegenFailure(literal, "неизвестная структура: " + structName);
        return;
    }
    const auto& fields = structDefs[structName];
    std::map<std::string, const Node*> named;
    std::vector<const Node*> positional;
    for (const Node& c : literal.children) {
        if (!c.varName.empty()) named[c.varName] = &c;
        else if (c.left) positional.push_back(&c);
    }
    size_t positionalIndex = 0;
    for (size_t index = 0; index < fields.size(); ++index) {
        const StructField& field = fields[index];
        const Node* initializer;
        if (named.count(field.name)) {
            initializer = named.at(field.name)->left.get();
        } else if (positionalIndex < positional.size()) {
            initializer = positional[positionalIndex++]->left.get();
        } else {
            initializer = &field.defaultVal;
        }
        llvm::Type* fieldType = aggregateType->getStructElementType(static_cast<unsigned>(index));
        llvm::Value* address = builder.CreateStructGEP(aggregateType, storage,
                                                        static_cast<unsigned>(index));
        if (initializer->type == NODE_STRUCT_LITERAL) {
            // Nested struct field: allocate a temp, initialize it, store the pointer
            llvm::AllocaInst* temp = builder.CreateAlloca(fieldType, nullptr, field.name);
            llvm::StructType* nestedType = llvmStructTypes[field.type];
            initStructLiteral(temp, nestedType, field.type, *initializer);
            llvm::Value* loaded = builder.CreateLoad(fieldType, temp);
            builder.CreateStore(loaded, address);
        } else {
            llvm::Value* value = genExpr(*initializer);
            value = castTo(value, fieldType, isUnsignedExpr(*initializer));
            builder.CreateStore(value, address);
        }
    }
}

llvm::Value* CodeGen::genExpr(const Node& node) {
    if (node.type == NODE_CHAR) {
        return builder.getInt8(static_cast<uint8_t>(std::stoul(node.value)));
    }
    if (node.type == NODE_NUMBER) {
        if (node.value.find_first_of(".eE") != std::string::npos) {
            size_t parsed = 0;
            double value = std::stod(node.value, &parsed);
            if (parsed != node.value.size() || !std::isfinite(value)) {
                std::cerr << "Ошибка: вещественный литерал вне диапазона f64: "
                          << node.value << std::endl;
                std::exit(1);
            }
            return llvm::ConstantFP::get(builder.getDoubleTy(), value);
        }
        if (!node.value.empty() && node.value[0] == '-') {
            const long long signedValue = std::stoll(node.value);
            if (signedValue >= INT32_MIN && signedValue <= INT32_MAX)
                return builder.getInt32(static_cast<int32_t>(signedValue));
            return builder.getInt64(signedValue);
        }
        try {
            const unsigned long long value = std::stoull(node.value);
            if (value <= static_cast<unsigned long long>(INT32_MAX))
                return builder.getInt32(static_cast<uint32_t>(value));
            return llvm::ConstantInt::get(builder.getInt64Ty(), value);
        } catch (const std::exception&) {
            std::cerr << "Ошибка: целочисленный литерал вне диапазона i64/u64: "
                      << node.value << std::endl;
            std::exit(1);
        }
    }
    if (node.type == NODE_STRING)
        return getStringPtr(node.value);
    if (node.type == NODE_STRUCT_LITERAL) {
        // Struct literals are only valid as initializers, not as standalone expressions.
        // They are handled in genNode for NODE_STRUCT_INSTANCE and NODE_VAR_DECL with struct type.
        return nullptr;
    }
    if (node.type == NODE_STRUCT_FIELD) {
    // Handle nested struct access by following the field path chain.
    // Maintain a pointer through the chain; only load the final value.
    std::string currentVar = node.varName;
    std::string fieldPath = node.op;
    auto objectIt = vars.find(currentVar);
    if (objectIt == vars.end()) {
        codegenFailure(node, "неизвестная переменная '" + currentVar + "'");
        return nullptr;
    }
    llvm::Value* currentPtr = objectIt->second;
    auto structNameIt = structTypesByVariable.find(currentVar);
    if (structNameIt == structTypesByVariable.end()) {
        codegenFailure(node, "неизвестная структура '" + currentVar + "'");
        return nullptr;
    }
    std::string structName = structNameIt->second;
    auto llvmTypeIt = llvmStructTypes.find(structName);
    if (llvmTypeIt == llvmStructTypes.end()) {
        codegenFailure(node, "неизвестная структура '" + structName + "'");
        return nullptr;
    }
    llvm::Type* currentType = llvmTypeIt->second;
    // Split field path by dots and traverse nested structs
    size_t pos = 0;
    while (pos < fieldPath.size()) {
        size_t dotPos = fieldPath.find('.', pos);
        std::string fieldName = fieldPath.substr(pos, dotPos == std::string::npos ? std::string::npos : dotPos - pos);
        auto fieldIndicesIt = structFieldIndices[structName].find(fieldName);
        if (fieldIndicesIt == structFieldIndices[structName].end()) {
            codegenFailure(node, "неизвестное поле '" + fieldName + "' в структуре '" + structName + "'");
            return nullptr;
        }
        unsigned fieldIndex = fieldIndicesIt->second;
        if (!currentType->isStructTy()) {
            codegenFailure(node, "storage '" + currentVar + "' не является структурой для доступа к полю '" + fieldName + "'");
            return nullptr;
        }
        currentPtr = builder.CreateStructGEP(currentType, currentPtr, fieldIndex);
        llvm::Type* fieldType = currentType->getStructElementType(fieldIndex);
        // Look up the declared type name of this field for the next hop
        auto defIt = structDefs.find(structName);
        if (defIt != structDefs.end()) {
            for (const auto& sf : defIt->second) {
                if (sf.name == fieldName) {
                    structName = sf.type;
                    break;
                }
            }
        }
        currentType = fieldType;
        currentVar = fieldName;
        if (dotPos == std::string::npos) {
            // Final field: load and return the value
            return builder.CreateLoad(fieldType, currentPtr);
        }
        pos = dotPos + 1;
    }
    return nullptr;
    }
    if (node.type == NODE_IDENTIFIER) {
        llvm::Value* alloca = vars[node.value];
        if (alloca) {
            llvm::Type* ty = storageType(alloca);
            if (ty->isPointerTy())
                return builder.CreateLoad(builder.getPtrTy(), alloca);
            return builder.CreateLoad(ty, alloca);
        }
        codegenFailure(node, "неизвестная переменная '" + node.value + "'");
    }
    if (node.type == NODE_ARRAY_ACCESS) {
        if (dynArrays.count(node.varName)) {
            llvm::Value* desc = dynArrays[node.varName];
            llvm::StructType* descType = dynArrayType();
            llvm::Value* dataPtr = builder.CreateLoad(builder.getPtrTy(),
                builder.CreateStructGEP(descType, desc, 0));
            llvm::Value* length = builder.CreateLoad(builder.getInt64Ty(),
                builder.CreateStructGEP(descType, desc, 1));
            llvm::Value* idx = genExpr(*node.left);
            emitArrayBoundsCheck(idx, length, node);
            llvm::Type* elementType = dynArrayElementTypes[node.varName];
            if (!elementType) elementType = builder.getInt32Ty();
            llvm::Value* gep = builder.CreateGEP(elementType, dataPtr, {idx});
            return builder.CreateLoad(elementType, gep);
        }
        if (vars.count(node.varName) &&
            storageType(vars[node.varName])->isPointerTy()) {
            llvm::Value* ptr = builder.CreateLoad(
                builder.getPtrTy(), vars[node.varName]);
            llvm::Value* idx = genExpr(*node.left);
            llvm::Value* gep = builder.CreateGEP(
                builder.getInt32Ty(), ptr, {idx});
            return builder.CreateLoad(builder.getInt32Ty(), gep);
        }
        if (arrays.count(node.varName)) {
            llvm::Value* arr = arrays[node.varName];
            unsigned n = arrayLengths[node.varName];
            llvm::Type* elementType = arrayElementTypes[node.varName];
            if (!elementType) elementType = builder.getInt32Ty();
            llvm::ArrayType* at = llvm::ArrayType::get(elementType, n);
            llvm::Value* idx = genExpr(*node.left);
            emitArrayBoundsCheck(idx, builder.getInt64(n), node);
            llvm::Value* gep = builder.CreateInBoundsGEP(
                at, arr, {builder.getInt32(0), idx});
            return builder.CreateLoad(elementType, gep);
        }
        codegenFailure(node, "неизвестный массив '" + node.varName + "'");
    }
    if (node.type == NODE_ADDR) {
        llvm::Value* alloca = vars[node.varName];
        if (alloca) return alloca;
        codegenFailure(node, "невозможно получить адрес неизвестной переменной '" + node.varName + "'");
    }
    if (node.type == NODE_DEREF) {
        llvm::Value* alloca = vars[node.varName];
        if (alloca) {
            llvm::Value* ptr = builder.CreateLoad(builder.getPtrTy(), alloca);
            auto pointee = pointeeTypes.find(node.varName);
            llvm::Type* type = pointee != pointeeTypes.end() ? pointee->second : builder.getInt32Ty();
            return builder.CreateLoad(type, ptr);
        }
        codegenFailure(node, "неизвестный указатель '" + node.varName + "'");
    }
    if (node.type == NODE_ARRAY_LEN || node.type == NODE_ARRAY_CAP) {
        auto found = dynArrays.find(node.varName);
        if (found == dynArrays.end()) {
            codegenFailure(node, "неизвестный динамический массив '" + node.varName + "'");
        }
        llvm::StructType* descType = dynArrayType();
        llvm::Value* desc = found->second;
        unsigned field = (node.type == NODE_ARRAY_LEN) ? 1 : 2;
        return builder.CreateLoad(builder.getInt64Ty(),
            builder.CreateStructGEP(descType, desc, field));
    }
    if (node.type == NODE_ARRAY_POP) {
        auto found = dynArrays.find(node.varName);
        if (found == dynArrays.end()) {
            codegenFailure(node, "неизвестный динамический массив '" + node.varName + "'");
        }
        llvm::StructType* descType = dynArrayType();
        llvm::Value* desc = found->second;
        llvm::Value* length = builder.CreateLoad(builder.getInt64Ty(),
            builder.CreateStructGEP(descType, desc, 1));
        llvm::Value* dataPtr = builder.CreateLoad(builder.getPtrTy(),
            builder.CreateStructGEP(descType, desc, 0));
        llvm::Value* isNonEmpty = builder.CreateICmpNE(length, builder.getInt64(0));
        llvm::BasicBlock* emptyFrom = builder.GetInsertBlock();
        llvm::Function* function = emptyFrom->getParent();
        llvm::BasicBlock* hasElem = llvm::BasicBlock::Create(context, "pop_has", function);
        llvm::BasicBlock* done = llvm::BasicBlock::Create(context, "pop_done", function);
        builder.CreateCondBr(isNonEmpty, hasElem, done);
        builder.SetInsertPoint(hasElem);
        llvm::Value* lastIdx = builder.CreateSub(length, builder.getInt64(1));
        llvm::Type* elementType = dynArrayElementTypes[node.varName];
        if (!elementType) elementType = builder.getInt32Ty();
        llvm::Value* gep = builder.CreateGEP(elementType, dataPtr, {lastIdx});
        llvm::Value* value = builder.CreateLoad(elementType, gep);
        builder.CreateStore(lastIdx,
            builder.CreateStructGEP(descType, desc, 1));
        builder.CreateBr(done);
        builder.SetInsertPoint(done);
        llvm::PHINode* phi = builder.CreatePHI(elementType, 2);
        phi->addIncoming(value, hasElem);
        // Empty arrays reach done directly from the block before the conditional.
        phi->addIncoming(llvm::Constant::getNullValue(elementType), emptyFrom);
        return phi;
    }
    if (node.type == NODE_ARRAY_PUSH || node.type == NODE_ARRAY_RESIZE) {
        genNode(node);
        return builder.getTrue();
    }
    if (node.type == NODE_DYN_ARRAY) {
        llvm::Function* mallocF = runtimeFunction("malloc");
        llvm::Value* size = genExpr(*node.left);
        size = builder.CreateSExt(size, builder.getInt64Ty());
        llvm::Value* bytes = builder.CreateMul(size,
            builder.getInt64(4)); // 4 байта на int
        return builder.CreateCall(mallocF, {bytes});
    }
    if (node.type == NODE_CAST) {
        llvm::Value* val = genExpr(*node.left);
        llvm::Type* target = svType(node.varType);
        if (val->getType()->isFloatingPointTy() && target->isFloatingPointTy())
            return castTo(val, target);
        if (val->getType()->isFloatingPointTy() && target->isIntegerTy())
            return builder.CreateFPToSI(val, target);
        if (val->getType()->isIntegerTy() && target->isFloatingPointTy()) {
            llvm::Value* converted = isUnsignedExpr(*node.left)
                ? builder.CreateUIToFP(val, target) : builder.CreateSIToFP(val, target);
            return converted;
        }
        if (val->getType()->isIntegerTy() && target->isIntegerTy()) {
            if (val->getType()->getIntegerBitWidth() == target->getIntegerBitWidth()) return val;
            if (val->getType()->getIntegerBitWidth() > target->getIntegerBitWidth())
                return builder.CreateTrunc(val, target);
            return isUnsignedExpr(*node.left)
                ? builder.CreateZExt(val, target) : builder.CreateSExt(val, target);
        }
        codegenFailure(node, "невозможно преобразовать значение к типу '" + node.varType + "'");
    }
    if (node.type == NODE_FILE_READ) {
        llvm::Value* fileAlloca = vars[node.varName];
        if (!fileAlloca) return getStringPtr("");
        llvm::Value* fp = builder.CreateLoad(builder.getPtrTy(), fileAlloca);
        emitRuntimeFailureIf(builder.CreateICmpEQ(fp,
            llvm::ConstantPointerNull::get(builder.getPtrTy())), "file is not open", node);
        llvm::Function* fgetsF = runtimeFunction("fgets");
        llvm::Value* bufPtr = allocateStringBuffer(builder.getInt64(4096), node);
        builder.CreateCall(fgetsF,
            {bufPtr, builder.getInt32(4096), fp});
        return bufPtr;
    }
    if (node.type == NODE_FILE_OPEN) {
        llvm::Function* fopenF = runtimeFunction("fopen");
        llvm::Value* fname = node.left ? genExpr(*node.left) : getStringPtr("");
        llvm::Value* mode = node.right ? genExpr(*node.right) : getStringPtr("w");
        llvm::Value* file = builder.CreateCall(fopenF, {fname, mode});
        emitRuntimeFailureIf(builder.CreateICmpEQ(file,
            llvm::ConstantPointerNull::get(builder.getPtrTy())), "cannot open file '%s'", node, fname);
        return file;
    }
    if (node.type == NODE_INPUT && node.varType == "cos") {
        llvm::Function* printfF = runtimeFunction("printf");
        llvm::Function* scanfF = runtimeFunction("scanf");
        llvm::Value* prompt = getStringPtr(node.value);
        builder.CreateCall(printfF, {prompt});
        llvm::Value* bufPtr = allocateStringBuffer(builder.getInt64(256), node);
        llvm::Value* fmt = getStringPtr("%255s");
        builder.CreateCall(scanfF, {fmt, bufPtr});
        return bufPtr;
    }
    if (node.type == NODE_INPUT) {
        llvm::Function* printfF = runtimeFunction("printf");
        llvm::Function* scanfF = runtimeFunction("scanf");
        llvm::Value* prompt = getStringPtr(node.value);
        builder.CreateCall(printfF, {prompt});
        llvm::AllocaInst* tmp =
            builder.CreateAlloca(builder.getInt32Ty(), nullptr, "input_tmp");
        llvm::Value* fmt = getStringPtr("%d");
        builder.CreateCall(scanfF, {fmt, tmp});
        return builder.CreateLoad(builder.getInt32Ty(), tmp);
    }
    if (node.type == NODE_TERNARY) {
        llvm::Value* condition = toBool(genExpr(*node.left));
        llvm::Function* function = builder.GetInsertBlock()->getParent();
        llvm::BasicBlock* trueBlock = llvm::BasicBlock::Create(context, "ternary_true", function);
        llvm::BasicBlock* falseBlock = llvm::BasicBlock::Create(context, "ternary_false", function);
        llvm::BasicBlock* mergeBlock = llvm::BasicBlock::Create(context, "ternary_merge", function);
        builder.CreateCondBr(condition, trueBlock, falseBlock);
        builder.SetInsertPoint(trueBlock);
        llvm::Value* trueValue = genExpr(*node.right);
        llvm::BasicBlock* trueEnd = builder.GetInsertBlock();
        if (!blockHasTerminator(trueEnd)) builder.CreateBr(mergeBlock);
        builder.SetInsertPoint(falseBlock);
        llvm::Value* falseValue = genExpr(*node.extra);
        llvm::BasicBlock* falseEnd = builder.GetInsertBlock();
        if (!blockHasTerminator(falseEnd)) builder.CreateBr(mergeBlock);
        llvm::Type* resultType = trueValue->getType();
        if (resultType != falseValue->getType()) {
            if (resultType->isFloatingPointTy() || falseValue->getType()->isFloatingPointTy()) {
                resultType = resultType->isDoubleTy() || falseValue->getType()->isDoubleTy()
                    ? builder.getDoubleTy() : builder.getFloatTy();
            } else if (resultType->isIntegerTy() && falseValue->getType()->isIntegerTy()) {
                resultType = resultType->getIntegerBitWidth() >= falseValue->getType()->getIntegerBitWidth()
                    ? resultType : falseValue->getType();
            }
            trueValue = castTo(trueValue, resultType, isUnsignedExpr(*node.right));
            falseValue = castTo(falseValue, resultType, isUnsignedExpr(*node.extra));
        }
        builder.SetInsertPoint(mergeBlock);
        llvm::PHINode* result = builder.CreatePHI(resultType, 2, "ternary_result");
        result->addIncoming(trueValue, trueEnd);
        result->addIncoming(falseValue, falseEnd);
        return result;
    }
    if (node.type == NODE_BINOP) {
        llvm::Value* L = genExpr(*node.left);
        if (node.op == "!")
            return builder.CreateNot(toBool(L));
        if (node.op == "~")
            return builder.CreateNot(L);
        if (node.op == "&&" || node.op == "||") {
            llvm::Function* function = builder.GetInsertBlock()->getParent();
            llvm::BasicBlock* leftBlock = builder.GetInsertBlock();
            llvm::Value* left = toBool(L);
            llvm::BasicBlock* rhsBlock = llvm::BasicBlock::Create(context, "logic_rhs", function);
            llvm::BasicBlock* mergeBlock = llvm::BasicBlock::Create(context, "logic_merge", function);
            if (node.op == "&&")
                builder.CreateCondBr(left, rhsBlock, mergeBlock);
            else
                builder.CreateCondBr(left, mergeBlock, rhsBlock);
            builder.SetInsertPoint(rhsBlock);
            llvm::Value* right = toBool(genExpr(*node.right));
            llvm::BasicBlock* rhsEnd = builder.GetInsertBlock();
            if (!blockHasTerminator(rhsEnd)) builder.CreateBr(mergeBlock);
            builder.SetInsertPoint(mergeBlock);
            llvm::PHINode* result = builder.CreatePHI(builder.getInt1Ty(), 2, "logic_result");
            result->addIncoming(node.op == "&&" ? builder.getFalse() : builder.getTrue(), leftBlock);
            result->addIncoming(right, rhsEnd);
            return result;
        }
        llvm::Value* R = genExpr(*node.right);
        if (node.op == "**") {
            const bool floatingOperands = L->getType()->isFloatingPointTy() ||
                R->getType()->isFloatingPointTy();
            const bool unsignedResult = isUnsignedExpr(*node.left) && isUnsignedExpr(*node.right);
            const unsigned resultWidth = std::max(integerWidth(*node.left), integerWidth(*node.right));
            llvm::Value* leftValue = L;
            llvm::Value* rightValue = R;
            if (leftValue->getType()->isIntegerTy())
                leftValue = isUnsignedExpr(*node.left)
                    ? builder.CreateUIToFP(leftValue, builder.getDoubleTy())
                    : builder.CreateSIToFP(leftValue, builder.getDoubleTy());
            else if (leftValue->getType()->isFloatTy())
                leftValue = builder.CreateFPExt(leftValue, builder.getDoubleTy());
            if (rightValue->getType()->isIntegerTy())
                rightValue = isUnsignedExpr(*node.right)
                    ? builder.CreateUIToFP(rightValue, builder.getDoubleTy())
                    : builder.CreateSIToFP(rightValue, builder.getDoubleTy());
            else if (rightValue->getType()->isFloatTy())
                rightValue = builder.CreateFPExt(rightValue, builder.getDoubleTy());
            llvm::Function* powerFunction = runtimeFunction("pow");
            llvm::Value* result = builder.CreateCall(powerFunction, {leftValue, rightValue});
            if (floatingOperands)
                return (L->getType()->isDoubleTy() || R->getType()->isDoubleTy())
                    ? result : builder.CreateFPTrunc(result, builder.getFloatTy());
            llvm::Type* integerResult = builder.getIntNTy(std::max(32U, resultWidth));
            return unsignedResult ? builder.CreateFPToUI(result, integerResult)
                                  : builder.CreateFPToSI(result, integerResult);
        }
        bool isFloat = L->getType()->isFloatingPointTy() || R->getType()->isFloatingPointTy();
        if (isFloat) {
            llvm::Type* common = L->getType()->isDoubleTy() || R->getType()->isDoubleTy()
                ? builder.getDoubleTy() : builder.getFloatTy();
            if (L->getType() != common) L = castTo(L, common);
            if (R->getType() != common) R = castTo(R, common);
            if (node.op == "+") return builder.CreateFAdd(L, R);
            if (node.op == "-") return builder.CreateFSub(L, R);
            if (node.op == "*") return builder.CreateFMul(L, R);
            if (node.op == "/") return builder.CreateFDiv(L, R);
            if (node.op == ">") return builder.CreateFCmpOGT(L, R);
            if (node.op == "<") return builder.CreateFCmpOLT(L, R);
            if (node.op == ">=") return builder.CreateFCmpOGE(L, R);
            if (node.op == "<=") return builder.CreateFCmpOLE(L, R);
            if (node.op == "==") return builder.CreateFCmpOEQ(L, R);
            if (node.op == "!=") return builder.CreateFCmpONE(L, R);
        }
        const unsigned resultWidth = std::max(integerWidth(*node.left), integerWidth(*node.right));
        const unsigned promotedWidth = resultWidth < 32 ? 32 : resultWidth;
        llvm::Type* common = builder.getIntNTy(promotedWidth);
        if (L->getType()->getIntegerBitWidth() > common->getIntegerBitWidth()) common = L->getType();
        if (R->getType()->getIntegerBitWidth() > common->getIntegerBitWidth()) common = R->getType();
        auto extendToCommon = [&](llvm::Value* value, const Node& expression) -> llvm::Value* {
            if (value->getType() == common || !value->getType()->isIntegerTy()) return value;
            return isUnsignedExpr(expression) ? builder.CreateZExt(value, common)
                                              : builder.CreateSExt(value, common);
        };
        if (L->getType() != common) L = extendToCommon(L, *node.left);
        if (R->getType() != common) R = extendToCommon(R, *node.right);
        if (node.op == "&") return builder.CreateAnd(L, R);
        if (node.op == "|") return builder.CreateOr(L, R);
        if (node.op == "^") return builder.CreateXor(L, R);
        if (node.op == "<<" || node.op == ">>") {
            const unsigned width = common->getIntegerBitWidth();
            llvm::Value* belowWidth = builder.CreateICmpULT(R, builder.getIntN(width, width));
            llvm::Value* valid = belowWidth;
            if (!isUnsignedExpr(*node.right)) {
                llvm::Value* nonNegative = builder.CreateICmpSGE(R, builder.getIntN(width, 0));
                valid = builder.CreateAnd(nonNegative, belowWidth);
            }
            llvm::Function* function = builder.GetInsertBlock()->getParent();
            llvm::BasicBlock* validBlock = llvm::BasicBlock::Create(context, "shift_valid", function);
            llvm::BasicBlock* trapBlock = llvm::BasicBlock::Create(context, "shift_invalid", function);
            llvm::Function* abortFunction = runtimeFunction("abort");
            builder.CreateCondBr(valid, validBlock, trapBlock);
            builder.SetInsertPoint(trapBlock);
            builder.CreateCall(abortFunction, {});
            builder.CreateUnreachable();
            builder.SetInsertPoint(validBlock);
            if (node.op == "<<") return builder.CreateShl(L, R);
            return isUnsignedExpr(*node.left) ? builder.CreateLShr(L, R) : builder.CreateAShr(L, R);
        }
        if (node.op == "+") return builder.CreateAdd(L, R);
        if (node.op == "-") return builder.CreateSub(L, R);
        if (node.op == "*") return builder.CreateMul(L, R);
        if (node.op == "/" || node.op == "//") return isUnsignedExpr(*node.left)
            ? builder.CreateUDiv(L, R) : builder.CreateSDiv(L, R);
        if (node.op == "%") return isUnsignedExpr(*node.left)
            ? builder.CreateURem(L, R) : builder.CreateSRem(L, R);
        if (node.op == ">") return isUnsignedExpr(*node.left)
            ? builder.CreateICmpUGT(L, R) : builder.CreateICmpSGT(L, R);
        if (node.op == "<") return isUnsignedExpr(*node.left)
            ? builder.CreateICmpULT(L, R) : builder.CreateICmpSLT(L, R);
        if (node.op == ">=") return isUnsignedExpr(*node.left)
            ? builder.CreateICmpUGE(L, R) : builder.CreateICmpSGE(L, R);
        if (node.op == "<=") return isUnsignedExpr(*node.left)
            ? builder.CreateICmpULE(L, R) : builder.CreateICmpSLE(L, R);
        if (node.op == "==") return builder.CreateICmpEQ(L, R);
        if (node.op == "!=") return builder.CreateICmpNE(L, R);
    }
    if (node.type == NODE_FUNC_CALL) {
        std::string name = node.varName;
        // математика
        if (name == "sqrt" || name == "pow" || name == "flo" ||
            name == "cel" || name == "ron") {
            const std::string mathName = name == "flo" ? "floor" :
                name == "cel" ? "ceil" : name == "ron" ? "round" : name;
            llvm::Function* function = runtimeFunction(mathName);
            std::vector<llvm::Value*> args;
            bool floatingArgument = false;
            bool wideIntegerArgument = false;
            for (const Node& argument : node.args) {
                llvm::Value* value = genExpr(argument);
                floatingArgument |= value->getType()->isFloatingPointTy();
                wideIntegerArgument |= value->getType()->isIntegerTy(64);
                if (value->getType()->isIntegerTy())
                    value = isUnsignedExpr(argument)
                        ? builder.CreateUIToFP(value, builder.getDoubleTy())
                        : builder.CreateSIToFP(value, builder.getDoubleTy());
                else if (value->getType()->isFloatTy())
                    value = builder.CreateFPExt(value, builder.getDoubleTy());
                args.push_back(value);
            }
            llvm::Value* result = builder.CreateCall(function, args);
            if (floatingArgument) return result;
            return wideIntegerArgument
                ? builder.CreateFPToSI(result, builder.getInt64Ty())
                : builder.CreateFPToSI(result, builder.getInt32Ty());
        }
        if (name == "abs") {
            llvm::Value* value = genExpr(node.args[0]);
            if (value->getType()->isFloatingPointTy()) {
                llvm::Value* negated = builder.CreateFNeg(value);
                llvm::Value* negative = builder.CreateFCmpOLT(
                    value, llvm::ConstantFP::get(value->getType(), 0.0));
                return builder.CreateSelect(negative, negated, value);
            }
            llvm::Value* negated = builder.CreateNeg(value);
            llvm::Value* negative = builder.CreateICmpSLT(
                value, llvm::ConstantInt::get(value->getType(), 0));
            return builder.CreateSelect(negative, negated, value);
        }
        if (name == "max" || name == "min") {
            llvm::Value* left = genExpr(node.args[0]);
            llvm::Value* right = genExpr(node.args[1]);
            llvm::Type* common = left->getType();
            if (left->getType()->isFloatingPointTy() || right->getType()->isFloatingPointTy()) {
                common = left->getType()->isDoubleTy() || right->getType()->isDoubleTy()
                    ? builder.getDoubleTy() : builder.getFloatTy();
                left = castTo(left, common, isUnsignedExpr(node.args[0]));
                right = castTo(right, common, isUnsignedExpr(node.args[1]));
                llvm::Value* condition = name == "max"
                    ? builder.CreateFCmpOGT(left, right)
                    : builder.CreateFCmpOLT(left, right);
                return builder.CreateSelect(condition, left, right);
            }
            if (left->getType() != right->getType()) {
                common = left->getType()->getIntegerBitWidth() >= right->getType()->getIntegerBitWidth()
                    ? left->getType() : right->getType();
                left = castTo(left, common, isUnsignedExpr(node.args[0]));
                right = castTo(right, common, isUnsignedExpr(node.args[1]));
            }
            const bool unsignedValues = isUnsignedExpr(node.args[0]) || isUnsignedExpr(node.args[1]);
            llvm::Value* condition = name == "max"
                ? (unsignedValues ? builder.CreateICmpUGT(left, right) : builder.CreateICmpSGT(left, right))
                : (unsignedValues ? builder.CreateICmpULT(left, right) : builder.CreateICmpSLT(left, right));
            return builder.CreateSelect(condition, left, right);
        }
        if (name == "str") {
            llvm::Function* snprintfF = runtimeFunction("snprintf");
            llvm::Value* val = genExpr(node.args[0]);
            llvm::Value* fmt;
            if (val->getType()->isFloatingPointTy()) {
                if (val->getType()->isFloatTy())
                    val = builder.CreateFPExt(val, builder.getDoubleTy());
                fmt = getStringPtr("%f");
            } else if (val->getType()->isIntegerTy(64)) {
                fmt = getStringPtr(isUnsignedExpr(node.args[0]) ? "%llu" : "%lld");
            } else {
                fmt = getStringPtr(isUnsignedExpr(node.args[0]) ? "%u" : "%d");
            }
            // Точный размер: snprintf(NULL, 0, ...) возвращает длину без '\0'.
            llvm::Value* length = builder.CreateCall(snprintfF,
                {llvm::ConstantPointerNull::get(builder.getPtrTy()), builder.getInt64(0), fmt, val});
            llvm::Value* bytes = builder.CreateAdd(
                builder.CreateSExt(length, builder.getInt64Ty()), builder.getInt64(1));
            llvm::Value* bufPtr = allocateStringBuffer(bytes, node);
            builder.CreateCall(snprintfF, {bufPtr, bytes, fmt, val});
            return bufPtr;
        }
        if (name == "ser") {
            llvm::Function* strcmpF = runtimeFunction("strcmp");
            llvm::Value* s1 = genExpr(node.args[0]);
            llvm::Value* s2 = genExpr(node.args[1]);
            llvm::Value* res = builder.CreateCall(strcmpF, {s1, s2});
            return builder.CreateICmpEQ(res, builder.getInt32(0));
        }
        if (name == "rep" && node.args.size() == 3) {
            llvm::Function* strlenF = runtimeFunction("strlen");
            llvm::Function* strstrF = runtimeFunction("strstr");
            llvm::Function* memcpyF = runtimeFunction("memcpy");
            llvm::Function* mallocF = runtimeFunction("malloc");
            llvm::Function* abortF = runtimeFunction("abort");

            llvm::Value* source = genExpr(node.args[0]);
            llvm::Value* needle = genExpr(node.args[1]);
            llvm::Value* replacement = genExpr(node.args[2]);
            llvm::Value* sourceLength = builder.CreateCall(strlenF, {source});
            llvm::Value* replacementLength = builder.CreateCall(strlenF, {replacement});
            llvm::Value* needleLength = builder.CreateCall(strlenF, {needle});
            llvm::Value* match = builder.CreateCall(strstrF, {source, needle});
            llvm::Value* found = builder.CreateICmpNE(
                match, llvm::ConstantPointerNull::get(builder.getPtrTy()));

            llvm::Function* function = builder.GetInsertBlock()->getParent();
            auto checkedAdd = [&](llvm::Value* left, llvm::Value* right) -> llvm::Value* {
                llvm::Value* sum = builder.CreateAdd(left, right);
                llvm::Value* overflow = builder.CreateICmpULT(sum, left);
                llvm::BasicBlock* trap = llvm::BasicBlock::Create(context, "rep_size_overflow", function);
                llvm::BasicBlock* next = llvm::BasicBlock::Create(context, "rep_size_ok", function);
                builder.CreateCondBr(overflow, trap, next);
                builder.SetInsertPoint(trap);
                builder.CreateCall(abortF, {});
                builder.CreateUnreachable();
                builder.SetInsertPoint(next);
                return sum;
            };
            auto allocate = [&](llvm::Value* byteCount) -> llvm::Value* {
                llvm::Value* pointer = builder.CreateCall(mallocF, {byteCount});
                llvm::Value* failed = builder.CreateICmpEQ(
                    pointer, llvm::ConstantPointerNull::get(builder.getPtrTy()));
                llvm::BasicBlock* trap = llvm::BasicBlock::Create(context, "rep_oom", function);
                llvm::BasicBlock* next = llvm::BasicBlock::Create(context, "rep_allocated", function);
                builder.CreateCondBr(failed, trap, next);
                builder.SetInsertPoint(trap);
                builder.CreateCall(abortF, {});
                builder.CreateUnreachable();
                builder.SetInsertPoint(next);
                return pointer;
            };

            llvm::BasicBlock* replaceBlock = llvm::BasicBlock::Create(context, "rep_found", function);
            llvm::BasicBlock* copyBlock = llvm::BasicBlock::Create(context, "rep_missing", function);
            llvm::BasicBlock* mergeBlock = llvm::BasicBlock::Create(context, "rep_merge", function);
            builder.CreateCondBr(found, replaceBlock, copyBlock);

            builder.SetInsertPoint(replaceBlock);
            llvm::Value* prefixLength = builder.CreatePtrDiff(builder.getInt8Ty(), match, source);
            llvm::Value* tailOffset = checkedAdd(prefixLength, needleLength);
            llvm::Value* tailLength = builder.CreateSub(sourceLength, tailOffset);
            llvm::Value* resultLength = checkedAdd(
                checkedAdd(prefixLength, replacementLength), tailLength);
            llvm::Value* resultBytes = checkedAdd(resultLength, builder.getInt64(1));
            llvm::Value* result = allocate(resultBytes);
            builder.CreateCall(memcpyF, {result, source, prefixLength});
            llvm::Value* replacementDst = builder.CreateInBoundsGEP(
                builder.getInt8Ty(), result, {prefixLength});
            builder.CreateCall(memcpyF, {replacementDst, replacement, replacementLength});
            llvm::Value* tailDst = builder.CreateInBoundsGEP(
                builder.getInt8Ty(), replacementDst, {replacementLength});
            llvm::Value* tailSrc = builder.CreateInBoundsGEP(
                builder.getInt8Ty(), match, {needleLength});
            builder.CreateCall(memcpyF, {tailDst, tailSrc, tailLength});
            llvm::Value* terminator = builder.CreateInBoundsGEP(
                builder.getInt8Ty(), result, {resultLength});
            builder.CreateStore(builder.getInt8(0), terminator);
            llvm::BasicBlock* foundEnd = builder.GetInsertBlock();
            builder.CreateBr(mergeBlock);

            builder.SetInsertPoint(copyBlock);
            llvm::Value* copyBytes = checkedAdd(sourceLength, builder.getInt64(1));
            llvm::Value* copyResult = allocate(copyBytes);
            builder.CreateCall(memcpyF, {copyResult, source, sourceLength});
            llvm::Value* copyTerminator = builder.CreateInBoundsGEP(
                builder.getInt8Ty(), copyResult, {sourceLength});
            builder.CreateStore(builder.getInt8(0), copyTerminator);
            llvm::BasicBlock* copyEnd = builder.GetInsertBlock();
            builder.CreateBr(mergeBlock);

            builder.SetInsertPoint(mergeBlock);
            llvm::PHINode* resultPhi = builder.CreatePHI(builder.getPtrTy(), 2, "rep_result");
            resultPhi->addIncoming(result, foundEnd);
            resultPhi->addIncoming(copyResult, copyEnd);
            return resultPhi;
        }
        if (name == "flo" || name == "cel" || name == "ron") {
            std::string cname = name == "flo" ? "floor" : name == "cel" ? "ceil" : "round";
            llvm::Function* f = runtimeFunction(cname);
            llvm::Value* arg = genExpr(node.args[0]);
            if (arg->getType()->isIntegerTy())
                arg = builder.CreateSIToFP(arg, builder.getDoubleTy());
            llvm::Value* res = builder.CreateCall(f, {arg});
            return builder.CreateFPToSI(res, builder.getInt32Ty());
        }
        if (name == "ran") {
            llvm::Function* f = runtimeFunction("rand");
            return builder.CreateCall(f, {});
        }
        if (name == "ar") {
            llvm::Function* strlenF = runtimeFunction("strlen");
            llvm::Value* str = genExpr(node.args[0]);
            return builder.CreateTrunc(builder.CreateCall(strlenF, {str}), builder.getInt32Ty());
        }
        // пользовательские функции
        if (funcs.count(name)) {
            std::vector<llvm::Value*> argVals;
            llvm::Function* fn = funcs[name];
            const auto& ptypes = funcParamTypes[name];
            for (size_t i = 0; i < node.args.size(); i++) {
                llvm::Value* v = genExpr(node.args[i]);
                if (i < ptypes.size())
                    v = castTo(v, svType(ptypes[i]));
                argVals.push_back(v);
            }
            llvm::Value* ret = builder.CreateCall(fn, argVals);
            std::string rt = funcReturnTypes.count(name)
                ? funcReturnTypes[name] : "rox";
            return castTo(ret, svType(rt));
        }
        codegenFailure(node, "неизвестная функция '" + name + "' при генерации LLVM");
    }
    codegenFailure(node, "неподдерживаемый узел выражения");
}

void CodeGen::genNode(const Node& node) {
    // Тела функций генерируются после завершённого main, поэтому для них
    // проверка «блок уже завершён» не применяется.
    if (node.type != NODE_FUNC_DEF && blockHasTerminator(builder.GetInsertBlock()))
        return;
    if (node.type == NODE_IMPORT) {
        // Imports are handled during tokenization (main.cpp::appendModuleTokens)
        // No codegen needed - they were expanded into the token stream
        return;
    }
    if (node.type == NODE_SLOV) {
        if (node.left && node.left->type == NODE_STRING) {
            llvm::Value* str = getStringPtr(node.left->value);
            builder.CreateCall(putsFunc, {str});
        } else if (node.left) {
            llvm::Value* val = genExpr(*node.left);
            if (val->getType()->isPointerTy()) {
                // строка — выводим через puts
                builder.CreateCall(putsFunc, {val});
            } else if (val->getType()->isFloatTy()) {
                llvm::Value* promoted = builder.CreateFPExt(val, builder.getDoubleTy());
                llvm::Value* fmt = getStringPtr("%f\n");
                builder.CreateCall(printfFunc, {fmt, promoted});
            } else if (val->getType()->isDoubleTy()) {
                llvm::Value* fmt = getStringPtr("%f\n");
                builder.CreateCall(printfFunc, {fmt, val});
            } else if (node.left->type == NODE_IDENTIFIER &&
                       charVars.count(node.left->value) && charVars[node.left->value]) {
                llvm::Value* promoted = builder.CreateZExt(val, builder.getInt32Ty());
                llvm::Value* fmt = getStringPtr("%c\n");
                builder.CreateCall(printfFunc, {fmt, promoted});
            } else if (val->getType()->isIntegerTy() &&
                       val->getType()->getIntegerBitWidth() < 32) {
                const bool isUnsigned = node.left->type == NODE_IDENTIFIER &&
                    unsignedVars.count(node.left->value) && unsignedVars[node.left->value];
                val = isUnsigned ? builder.CreateZExt(val, builder.getInt32Ty())
                                 : builder.CreateSExt(val, builder.getInt32Ty());
                llvm::Value* fmt = getStringPtr("%d\n");
                builder.CreateCall(printfFunc, {fmt, val});
            } else if (val->getType()->isIntegerTy(64)) {
                const bool isUnsigned = node.left->type == NODE_IDENTIFIER &&
                    unsignedVars.count(node.left->value) && unsignedVars[node.left->value];
                llvm::Value* fmt = getStringPtr(isUnsigned ? "%llu\n" : "%lld\n");
                builder.CreateCall(printfFunc, {fmt, val});
            } else {
                const bool isUnsigned = node.left->type == NODE_IDENTIFIER &&
                    unsignedVars.count(node.left->value) && unsignedVars[node.left->value];
                llvm::Value* fmt = getStringPtr(isUnsigned ? "%u\n" : "%d\n");
                builder.CreateCall(printfFunc, {fmt, val});
            }
        } else {
            llvm::Value* str = getStringPtr(node.value);
            builder.CreateCall(putsFunc, {str});
        }
    }
    else if (node.type == NODE_VAR_DECL) {
        // Dynamic array: allocate a descriptor {ptr data; i64 length; i64 capacity}
        if (node.left->type == NODE_DYN_ARRAY) {
            const Node& alloc = *node.left;
            llvm::Type* elementType = svType(alloc.varType);
            llvm::Value* size = genExpr(*alloc.left);
            size = builder.CreateSExt(size, builder.getInt64Ty());
            // nw T[n]: длина n, элементы обнулены (как new T[n]() в C++ и make([]T, n) в Go).
            // calloc(0) может вернуть NULL, поэтому выделяется минимум один элемент.
            llvm::Value* count = builder.CreateSelect(
                builder.CreateICmpEQ(size, builder.getInt64(0)), builder.getInt64(1), size);
            llvm::Value* data = builder.CreateCall(runtimeFunction("calloc"), {count,
                builder.getInt64(elementType->getPrimitiveSizeInBits() / 8)});
            llvm::Function* function = builder.GetInsertBlock()->getParent();
            llvm::BasicBlock* trap = llvm::BasicBlock::Create(context, "dyn_oom", function);
            llvm::BasicBlock* next = llvm::BasicBlock::Create(context, "dyn_alloc", function);
            builder.CreateCondBr(builder.CreateICmpEQ(
                data, llvm::ConstantPointerNull::get(builder.getPtrTy())), trap, next);
            builder.SetInsertPoint(trap);
            builder.CreateCall(abortFunc, {});
            builder.CreateUnreachable();
            builder.SetInsertPoint(next);
            llvm::StructType* descType = dynArrayType();
            llvm::Value* desc = createStorage(descType, node.varName);
            builder.CreateStore(data,
                builder.CreateStructGEP(descType, desc, 0));
            builder.CreateStore(size,
                builder.CreateStructGEP(descType, desc, 1)); // length = n
            builder.CreateStore(size,
                builder.CreateStructGEP(descType, desc, 2)); // capacity = size
            dynArrays[node.varName] = desc;
            dynArrayElementTypes[node.varName] = elementType;
            vars[node.varName] = desc;
            constVars[node.varName] = node.isConst;
            return;
        }
        // ptr p = &x: запомнить тип x, чтобы *p читал и писал значение нужного типа.
        pointeeTypes.erase(node.varName);
        if (node.left->type == NODE_ADDR && vars.count(node.left->varName) && vars[node.left->varName])
            pointeeTypes[node.varName] = storageType(vars[node.left->varName]);
        llvm::Value* val = genExpr(*node.left);
        llvm::Type* ty = node.varType.empty()
            ? val->getType() : svType(node.varType);
        llvm::Value* alloca = createStorage(ty, node.varName);
        val = castTo(val, ty, isUnsignedExpr(*node.left));
        builder.CreateStore(val, alloca);
        vars[node.varName] = alloca;
        constVars[node.varName] = node.isConst;
        unsignedVars[node.varName] = node.varType == "u8" || node.varType == "u16" ||
            node.varType == "u32" || node.varType == "u64" || node.varType == "char";
        charVars[node.varName] = node.varType == "char";
        integerWidths[node.varName] = ty->isIntegerTy() ? ty->getIntegerBitWidth() : 32;
    }
    else if (node.type == NODE_VAR_DECL && node.left && node.left->type == NODE_STRUCT_LITERAL) {
        // Handle struct literal initialization for typed variable declarations
        if (!structDefs.count(node.varType)) {
            codegenFailure(node, "неизвестная структура: " + node.varType);
            return;
        }
        llvm::StructType* aggregateType = llvmStructTypes[node.varType];
        llvm::Value* object = createStorage(aggregateType, node.varName);
        vars[node.varName] = object;
        structTypesByVariable[node.varName] = node.varType;
        initStructLiteral(object, aggregateType, node.varType, *node.left);
        constVars[node.varName] = node.isConst;
        unsignedVars[node.varName] = false;
        charVars[node.varName] = false;
        integerWidths[node.varName] = 0;
    }
    else if (node.type == NODE_ASSIGN) {
        llvm::Value* alloca = vars[node.varName];
        if (alloca && !constVars[node.varName]) {
            llvm::Value* val = genExpr(*node.left);
            llvm::Type* ty = storageType(alloca);
            val = castTo(val, ty, isUnsignedExpr(*node.left));
            builder.CreateStore(val, alloca);
        }
    }
    else if (node.type == NODE_STRUCT_FIELD_ASSIGN) {
        auto object = vars.find(node.varName);
        auto structName = structTypesByVariable.find(node.varName);
        if (object == vars.end() || structName == structTypesByVariable.end())
            codegenFailure(node, "нет aggregate storage структуры '" + node.varName + "'");
        auto fieldIndex = structFieldIndices[structName->second].find(node.op);
        if (fieldIndex == structFieldIndices[structName->second].end())
            codegenFailure(node, "неизвестное поле структуры '" + node.varName + "." + node.op + "'");
        llvm::Type* aggregateType = storageType(object->second);
        const unsigned index = fieldIndex->second;
        llvm::Type* fieldType = aggregateType->getStructElementType(index);
        llvm::Value* value = genExpr(*node.left);
        value = castTo(value, fieldType, isUnsignedExpr(*node.left));
        llvm::Value* address = builder.CreateStructGEP(aggregateType, object->second, index);
        builder.CreateStore(value, address);
    }
    else if (node.type == NODE_ARRAY_ASSIGN) {
        if (node.left && node.right) {
            // динамический массив с описателем
            if (dynArrays.count(node.varName)) {
                llvm::Value* desc = dynArrays[node.varName];
                llvm::StructType* descType = dynArrayType();
                llvm::Value* dataPtr = builder.CreateLoad(builder.getPtrTy(),
                    builder.CreateStructGEP(descType, desc, 0));
                llvm::Value* length = builder.CreateLoad(builder.getInt64Ty(),
                    builder.CreateStructGEP(descType, desc, 1));
                llvm::Value* idx = genExpr(*node.left);
                emitArrayBoundsCheck(idx, length, node);
                llvm::Type* elementType = dynArrayElementTypes[node.varName];
                if (!elementType) elementType = builder.getInt32Ty();
                llvm::Value* gep = builder.CreateGEP(elementType, dataPtr, {idx});
                builder.CreateStore(genExpr(*node.right), gep);
            }
            // raw dynamic array (ptr variable)
            else if (vars.count(node.varName) &&
                storageType(vars[node.varName])->isPointerTy()) {
                llvm::Value* ptr = builder.CreateLoad(
                    builder.getPtrTy(), vars[node.varName]);
                llvm::Value* idx = genExpr(*node.left);
                llvm::Value* gep = builder.CreateGEP(
                    builder.getInt32Ty(), ptr, {idx});
                builder.CreateStore(genExpr(*node.right), gep);
            } else if (arrays.count(node.varName)) {
                llvm::Value* arr = arrays[node.varName];
                unsigned n = arrayLengths[node.varName];
                llvm::Type* elementType = arrayElementTypes[node.varName];
                if (!elementType) elementType = builder.getInt32Ty();
                llvm::ArrayType* at = llvm::ArrayType::get(elementType, n);
                llvm::Value* idx = genExpr(*node.left);
                emitArrayBoundsCheck(idx, builder.getInt64(n), node);
                llvm::Value* gep = builder.CreateInBoundsGEP(
                    at, arr, {builder.getInt32(0), idx});
                builder.CreateStore(genExpr(*node.right), gep);
            }
        }
    }
    else if (node.type == NODE_DEREF_ASSIGN) {
        llvm::Value* alloca = vars[node.varName];
        if (alloca) {
            llvm::Value* ptr = builder.CreateLoad(builder.getPtrTy(), alloca);
            auto pointee = pointeeTypes.find(node.varName);
            llvm::Value* value = genExpr(*node.left);
            if (pointee != pointeeTypes.end())
                value = castTo(value, pointee->second, isUnsignedExpr(*node.left));
            builder.CreateStore(value, ptr);
        }
    }
    else if (node.type == NODE_RETURN) {
        llvm::Function* cur = builder.GetInsertBlock()->getParent();
        llvm::Type* retTy = cur->getReturnType();
        if (retTy->isVoidTy()) {
            builder.CreateRetVoid();
        } else if (node.left) {
            llvm::Value* v = genExpr(*node.left);
            builder.CreateRet(castTo(v, retTy, isUnsignedExpr(*node.left)));
        }
    }
    else if (node.type == NODE_BREAK) {
        if (breakBlock) builder.CreateBr(breakBlock);
    }
    else if (node.type == NODE_CONTINUE) {
        if (continueBlock) builder.CreateBr(continueBlock);
    }
    else if (node.type == NODE_ARRAY_DECL) {
        unsigned n = (unsigned)node.args.size();
        llvm::Type* elementType = svType(node.varType);
        llvm::ArrayType* at = llvm::ArrayType::get(elementType, n);
        llvm::Value* arr = createStorage(at, node.varName);
        arrays[node.varName] = arr;
        arrayLengths[node.varName] = n;
        arrayElementTypes[node.varName] = elementType;
        for (unsigned i = 0; i < n; i++) {
            llvm::Value* gep = builder.CreateInBoundsGEP(
                at, arr, {builder.getInt32(0), builder.getInt32(i)});
            llvm::Value* val = genExpr(node.args[i]);
            val = castTo(val, elementType, isUnsignedExpr(node.args[i]));
            builder.CreateStore(val, gep);
        }
    }
    else if (node.type == NODE_ARRAY_POP) {
        auto found = dynArrays.find(node.varName);
        if (found == dynArrays.end())
            codegenFailure(node, "неизвестный динамический массив '" + node.varName + "'");
        llvm::StructType* descType = dynArrayType();
        llvm::Value* desc = found->second;
        llvm::Value* length = builder.CreateLoad(builder.getInt64Ty(),
            builder.CreateStructGEP(descType, desc, 1));
        llvm::Value* isNonEmpty = builder.CreateICmpNE(length, builder.getInt64(0));
        llvm::Function* function = builder.GetInsertBlock()->getParent();
        llvm::BasicBlock* hasElem = llvm::BasicBlock::Create(context, "pop_has", function);
        llvm::BasicBlock* done = llvm::BasicBlock::Create(context, "pop_done", function);
        builder.CreateCondBr(isNonEmpty, hasElem, done);
        builder.SetInsertPoint(hasElem);
        llvm::Value* lastIdx = builder.CreateSub(length, builder.getInt64(1));
        builder.CreateStore(lastIdx,
            builder.CreateStructGEP(descType, desc, 1));
        builder.CreateBr(done);
        builder.SetInsertPoint(done);
    }
    else if (node.type == NODE_ARRAY_PUSH) {
        auto found = dynArrays.find(node.varName);
        if (found == dynArrays.end())
            codegenFailure(node, "неизвестный динамический массив '" + node.varName + "'");
        llvm::StructType* descType = dynArrayType();
        llvm::Value* desc = found->second;
        llvm::Type* elementType = dynArrayElementTypes[node.varName];
        if (!elementType) elementType = builder.getInt32Ty();

        llvm::Value* length = builder.CreateLoad(builder.getInt64Ty(),
            builder.CreateStructGEP(descType, desc, 1));
        llvm::Value* capacity = builder.CreateLoad(builder.getInt64Ty(),
            builder.CreateStructGEP(descType, desc, 2));

        // Grow if length == capacity (double capacity, minimum 1)
        llvm::Function* function = builder.GetInsertBlock()->getParent();
        llvm::BasicBlock* growBlock = llvm::BasicBlock::Create(context, "push_grow", function);
        llvm::BasicBlock* noGrowBlock = llvm::BasicBlock::Create(context, "push_nogrow", function);
        llvm::BasicBlock* afterGrowBlock = llvm::BasicBlock::Create(context, "push_after_grow", function);

        llvm::Value* needsGrow = builder.CreateICmpEQ(length, capacity);
        builder.CreateCondBr(needsGrow, growBlock, noGrowBlock);

        // Grow path
        builder.SetInsertPoint(growBlock);
        llvm::Value* newCapacity = builder.CreateSelect(
            builder.CreateICmpEQ(capacity, builder.getInt64(0)),
            builder.getInt64(1),
            builder.CreateMul(capacity, builder.getInt64(2))
        );
        growDynArray(node.varName, newCapacity);
        builder.CreateBr(afterGrowBlock);

        // No grow path
        builder.SetInsertPoint(noGrowBlock);
        builder.CreateBr(afterGrowBlock);

        // After grow - store element and increment length
        builder.SetInsertPoint(afterGrowBlock);
        llvm::Value* currentLength = builder.CreateLoad(builder.getInt64Ty(),
            builder.CreateStructGEP(descType, desc, 1));
        llvm::Value* currentData = builder.CreateLoad(builder.getPtrTy(),
            builder.CreateStructGEP(descType, desc, 0));
        llvm::Value* value = genExpr(*node.left);
        value = castTo(value, elementType, isUnsignedExpr(*node.left));
        llvm::Value* gep = builder.CreateGEP(elementType, currentData, {currentLength});
        builder.CreateStore(value, gep);
        llvm::Value* newLength = builder.CreateAdd(currentLength, builder.getInt64(1));
        builder.CreateStore(newLength, builder.CreateStructGEP(descType, desc, 1));
    }
    else if (node.type == NODE_ARRAY_RESIZE) {
        auto found = dynArrays.find(node.varName);
        if (found == dynArrays.end())
            codegenFailure(node, "неизвестный динамический массив '" + node.varName + "'");
        llvm::StructType* descType = dynArrayType();
        llvm::Value* desc = found->second;

        llvm::Value* newSize = genExpr(*node.left);
        newSize = builder.CreateSExt(newSize, builder.getInt64Ty());

        llvm::Value* capacity = builder.CreateLoad(builder.getInt64Ty(),
            builder.CreateStructGEP(descType, desc, 2));

        // Grow if newSize > capacity
        llvm::Function* function = builder.GetInsertBlock()->getParent();
        llvm::BasicBlock* growBlock = llvm::BasicBlock::Create(context, "resize_grow", function);
        llvm::BasicBlock* noGrowBlock = llvm::BasicBlock::Create(context, "resize_nogrow", function);

        llvm::Value* needsGrow = builder.CreateICmpUGT(newSize, capacity);
        builder.CreateCondBr(needsGrow, growBlock, noGrowBlock);

        builder.SetInsertPoint(growBlock);
        // Grow to at least newSize (round up to next power of 2 for efficiency)
        growDynArray(node.varName, newSize);
        builder.CreateBr(noGrowBlock);

        builder.SetInsertPoint(noGrowBlock);
        builder.CreateStore(newSize, builder.CreateStructGEP(descType, desc, 1));
    }
    else if (node.type == NODE_STRUCT_DEF) {
        std::vector<StructField> fields;
        for (const Node& field : node.children) {
            StructField sf;
            sf.type = field.varType;
            sf.name = field.varName;
            if (field.left) {
                sf.defaultVal = *field.left;
            } else {
                // Field has no default value: use a zero literal as a neutral
                // placeholder so initStructLiteral can still fill it in.
                sf.defaultVal.type = NODE_NUMBER;
                sf.defaultVal.value = "0";
            }
            fields.push_back(sf);
        }
        structDefs[node.varName] = fields;
        std::vector<llvm::Type*> fieldTypes;
        std::map<std::string, unsigned> fieldIndices;
        for (size_t index = 0; index < fields.size(); ++index) {
            fieldTypes.push_back(svType(fields[index].type));
            fieldIndices[fields[index].name] = static_cast<unsigned>(index);
        }
        llvm::StructType* llvmType = llvm::StructType::create(context, node.varName);
        llvmType->setBody(fieldTypes, false);
        llvmStructTypes[node.varName] = llvmType;
        structFieldIndices[node.varName] = std::move(fieldIndices);
    }
    else if (node.type == NODE_STRUCT_INSTANCE) {
        if (!structDefs.count(node.varType)) {
            std::cerr << "Неизвестная структура: " << node.varType << std::endl;
            exit(1);
        }
        llvm::StructType* aggregateType = llvmStructTypes[node.varType];
        llvm::Value* object = createStorage(aggregateType, node.varName);
        vars[node.varName] = object;
        structTypesByVariable[node.varName] = node.varType;
        initStructLiteral(object, aggregateType, node.varType, node);
    }
    else if (node.type == NODE_FUNC_DEF) {
        std::vector<std::string> ptypes = funcParamTypes[node.varName];
        llvm::Type* retTy = svType(funcReturnTypes[node.varName]);
        llvm::Function* func = funcs[node.varName];
        llvm::BasicBlock* block =
            llvm::BasicBlock::Create(context, "entry", func);
        llvm::BasicBlock* savedBlock = builder.GetInsertBlock();
        auto savedVars = vars;
        auto savedUnsignedVars = unsignedVars;
        auto savedCharVars = charVars;
        auto savedConstVars = constVars;
        auto savedIntegerWidths = integerWidths;
        auto savedArrays = arrays;
        auto savedArrayLengths = arrayLengths;
        auto savedStructTypesByVariable = structTypesByVariable;
        builder.SetInsertPoint(block);
        int idx = 0;
        for (auto& arg : func->args()) {
            std::string entry = node.params[idx];
            std::string ptype = entry.substr(0, entry.find(':'));
            std::string pname = entry.substr(entry.find(':') + 1);
            idx++;
            arg.setName(pname);
            llvm::Type* ty = svType(ptype);
            llvm::AllocaInst* alloca =
                builder.CreateAlloca(ty, nullptr, pname);
            builder.CreateStore(&arg, alloca);
            vars[pname] = alloca;
            unsignedVars[pname] = ptype == "u8" || ptype == "u16" ||
                ptype == "u32" || ptype == "u64" || ptype == "char";
            charVars[pname] = ptype == "char";
            constVars[pname] = false;
            integerWidths[pname] = ty->isIntegerTy() ? ty->getIntegerBitWidth() : 32;
        }
        bool hasRet = false;
        for (const Node& n : node.children) {
            genNode(n);
            if (n.type == NODE_RETURN) { hasRet = true; break; }
        }
        if (!hasRet && !retTy->isVoidTy()) {
            if (retTy->isDoubleTy())
                builder.CreateRet(llvm::ConstantFP::get(builder.getDoubleTy(), 0.0));
            else if (retTy->isPointerTy())
                builder.CreateRet(llvm::ConstantPointerNull::get(builder.getPtrTy()));
            else
                builder.CreateRet(llvm::ConstantInt::get(retTy, 0));
        } else if (!hasRet && retTy->isVoidTy()) {
            builder.CreateRetVoid();
        }
        builder.SetInsertPoint(savedBlock);
        vars = savedVars;
        unsignedVars = savedUnsignedVars;
        charVars = savedCharVars;
        constVars = savedConstVars;
        integerWidths = savedIntegerWidths;
        arrays = savedArrays;
        arrayLengths = savedArrayLengths;
        structTypesByVariable = savedStructTypesByVariable;
        funcs[node.varName] = func;
    }
    else if (node.type == NODE_FUNC_CALL) {
        if (funcs.count(node.varName)) {
            std::vector<llvm::Value*> argVals;
            const auto& ptypes = funcParamTypes[node.varName];
            for (size_t i = 0; i < node.args.size(); i++) {
                llvm::Value* v = genExpr(node.args[i]);
                if (i < ptypes.size())
                    v = castTo(v, svType(ptypes[i]));
                argVals.push_back(v);
            }
            builder.CreateCall(funcs[node.varName], argVals);
        } else {
            genExpr(node);
        }
    }
    else if (node.type == NODE_DELETE) {
        llvm::Function* freeF = runtimeFunction("free");
        llvm::Value* alloca = vars[node.varName];
        if (alloca) {
            llvm::Value* ptr = builder.CreateLoad(builder.getPtrTy(), alloca);
            builder.CreateCall(freeF, {ptr});
        }
    }
    else if (node.type == NODE_FILE_WRITE) {
        llvm::Function* fputsF = runtimeFunction("fputs");
        llvm::Value* fileAlloca = vars[node.varName];
        if (fileAlloca) {
            llvm::Value* filePtr =
                builder.CreateLoad(builder.getPtrTy(), fileAlloca);
            llvm::Value* str = genExpr(*node.left);
            builder.CreateCall(fputsF, {str, filePtr});
        }
    }
    else if (node.type == NODE_INPUT) {
        genExpr(node);
    }
    else if (node.type == NODE_FILE_CLOSE) {
        llvm::Function* fcloseF = runtimeFunction("fclose");
        llvm::Value* fileAlloca = vars[node.varName];
        if (fileAlloca) {
            llvm::Value* filePtr =
                builder.CreateLoad(builder.getPtrTy(), fileAlloca);
            builder.CreateCall(fcloseF, {filePtr});
        }
    }
    else if (node.type == NODE_IF) {
        llvm::Value* cond = toBool(genExpr(*node.left));
        llvm::Function* func = builder.GetInsertBlock()->getParent();
        llvm::BasicBlock* thenBlock =
            llvm::BasicBlock::Create(context, "then", func);
        llvm::BasicBlock* elseBlock =
            llvm::BasicBlock::Create(context, "else", func);
        llvm::BasicBlock* mergeBlock =
            llvm::BasicBlock::Create(context, "merge", func);
        builder.CreateCondBr(cond, thenBlock, elseBlock);
        const ScopeSnapshot parentScope = saveScope();
        builder.SetInsertPoint(thenBlock);
        for (const Node& n : node.children) genNode(n);
        if (!blockHasTerminator(builder.GetInsertBlock()))
            builder.CreateBr(mergeBlock);
        restoreScope(parentScope);
        builder.SetInsertPoint(elseBlock);
        for (const Node& n : node.elseChildren) genNode(n);
        if ((builder.GetInsertBlock()->empty() || !builder.GetInsertBlock()->back().isTerminator()))
            builder.CreateBr(mergeBlock);
        restoreScope(parentScope);
        builder.SetInsertPoint(mergeBlock);
    }
    else if (node.type == NODE_LOOP_N) {
        llvm::Value* count = genExpr(*node.left);
        llvm::Function* func = builder.GetInsertBlock()->getParent();
        llvm::AllocaInst* i =
            builder.CreateAlloca(builder.getInt32Ty(), nullptr, "i");
        builder.CreateStore(builder.getInt32(0), i);
        llvm::BasicBlock* loopBlock =
            llvm::BasicBlock::Create(context, "loop", func);
        llvm::BasicBlock* afterBlock =
            llvm::BasicBlock::Create(context, "after", func);
        llvm::BasicBlock* bodyBlock =
            llvm::BasicBlock::Create(context, "body", func);
        llvm::BasicBlock* stepBlock =
            llvm::BasicBlock::Create(context, "step", func);
        auto savedBreak = breakBlock;
        auto savedContinue = continueBlock;
        breakBlock = afterBlock;
        continueBlock = stepBlock;
        builder.CreateBr(loopBlock);
        builder.SetInsertPoint(loopBlock);
        llvm::Value* cur = builder.CreateLoad(builder.getInt32Ty(), i);
        llvm::Value* cond = builder.CreateICmpSLT(cur, count);
        builder.CreateCondBr(cond, bodyBlock, afterBlock);
        builder.SetInsertPoint(bodyBlock);
        const ScopeSnapshot parentScope = saveScope();
        for (const Node& n : node.children) genNode(n);
        if ((builder.GetInsertBlock()->empty() || !builder.GetInsertBlock()->back().isTerminator()))
            builder.CreateBr(stepBlock);
        restoreScope(parentScope);
        builder.SetInsertPoint(stepBlock);
        llvm::Value* next = builder.CreateAdd(cur, builder.getInt32(1));
        builder.CreateStore(next, i);
        builder.CreateBr(loopBlock);
        builder.SetInsertPoint(afterBlock);
        breakBlock = savedBreak;
        continueBlock = savedContinue;
    }
    else if (node.type == NODE_FOR_EACH) {
        if (!arrays.count(node.left->value) || !arrayLengths.count(node.left->value))
            return;
        llvm::Value* array = arrays[node.left->value];
        const unsigned length = arrayLengths[node.left->value];
        llvm::ArrayType* arrayType = llvm::dyn_cast<llvm::ArrayType>(storageType(array));
        if (!arrayType) return;
        llvm::Type* elementType = arrayType->getElementType();
        llvm::Function* function = builder.GetInsertBlock()->getParent();
        llvm::AllocaInst* index = builder.CreateAlloca(builder.getInt32Ty(), nullptr, "foreach_index");
        builder.CreateStore(builder.getInt32(0), index);
        llvm::AllocaInst* element = builder.CreateAlloca(elementType, nullptr, node.varName);
        llvm::BasicBlock* conditionBlock = llvm::BasicBlock::Create(context, "foreach_cond", function);
        llvm::BasicBlock* bodyBlock = llvm::BasicBlock::Create(context, "foreach_body", function);
        llvm::BasicBlock* stepBlock = llvm::BasicBlock::Create(context, "foreach_step", function);
        llvm::BasicBlock* afterBlock = llvm::BasicBlock::Create(context, "foreach_after", function);
        const ScopeSnapshot parentScope = saveScope();
        const auto savedBreak = breakBlock;
        const auto savedContinue = continueBlock;
        vars[node.varName] = element;
        unsignedVars[node.varName] = false;
        charVars[node.varName] = false;
        integerWidths[node.varName] = elementType->isIntegerTy() ? elementType->getIntegerBitWidth() : 32;
        breakBlock = afterBlock;
        continueBlock = stepBlock;
        builder.CreateBr(conditionBlock);
        builder.SetInsertPoint(conditionBlock);
        llvm::Value* current = builder.CreateLoad(builder.getInt32Ty(), index);
        builder.CreateCondBr(builder.CreateICmpULT(current, builder.getInt32(length)), bodyBlock, afterBlock);
        builder.SetInsertPoint(bodyBlock);
        llvm::Value* address = builder.CreateInBoundsGEP(
            arrayType, array, {builder.getInt32(0), current});
        llvm::Value* value = builder.CreateLoad(elementType, address);
        builder.CreateStore(value, element);
        for (const Node& child : node.children) genNode(child);
        if ((builder.GetInsertBlock()->empty() || !builder.GetInsertBlock()->back().isTerminator())) builder.CreateBr(stepBlock);
        builder.SetInsertPoint(stepBlock);
        current = builder.CreateLoad(builder.getInt32Ty(), index);
        builder.CreateStore(builder.CreateAdd(current, builder.getInt32(1)), index);
        builder.CreateBr(conditionBlock);
        builder.SetInsertPoint(afterBlock);
        restoreScope(parentScope);
        breakBlock = savedBreak;
        continueBlock = savedContinue;
    }
    else if (node.type == NODE_FOR_RANGE) {
        llvm::Value* start = genExpr(*node.left);
        llvm::Value* end = genExpr(*node.right);
        llvm::Type* iteratorType = start->getType();
        if (iteratorType != end->getType()) {
            llvm::Type* common = iteratorType->getIntegerBitWidth() >= end->getType()->getIntegerBitWidth()
                ? iteratorType : end->getType();
            start = castTo(start, common, isUnsignedExpr(*node.left));
            end = castTo(end, common, isUnsignedExpr(*node.right));
            iteratorType = common;
        }
        llvm::AllocaInst* iterator = builder.CreateAlloca(iteratorType, nullptr, node.varName);
        builder.CreateStore(start, iterator);
        llvm::Function* function = builder.GetInsertBlock()->getParent();
        llvm::BasicBlock* conditionBlock = llvm::BasicBlock::Create(context, "for_cond", function);
        llvm::BasicBlock* bodyBlock = llvm::BasicBlock::Create(context, "for_body", function);
        llvm::BasicBlock* stepBlock = llvm::BasicBlock::Create(context, "for_step", function);
        llvm::BasicBlock* afterBlock = llvm::BasicBlock::Create(context, "for_after", function);
        const ScopeSnapshot parentScope = saveScope();
        const auto savedBreak = breakBlock;
        const auto savedContinue = continueBlock;
        vars[node.varName] = iterator;
        unsignedVars[node.varName] = isUnsignedExpr(*node.left);
        integerWidths[node.varName] = iteratorType->getIntegerBitWidth();
        breakBlock = afterBlock;
        continueBlock = stepBlock;
        builder.CreateBr(conditionBlock);
        builder.SetInsertPoint(conditionBlock);
        llvm::Value* current = builder.CreateLoad(iteratorType, iterator);
        llvm::Value* condition;
        if (isUnsignedExpr(*node.left))
            condition = node.inclusiveRange ? builder.CreateICmpULE(current, end)
                                            : builder.CreateICmpULT(current, end);
        else
            condition = node.inclusiveRange ? builder.CreateICmpSLE(current, end)
                                            : builder.CreateICmpSLT(current, end);
        builder.CreateCondBr(condition, bodyBlock, afterBlock);
        builder.SetInsertPoint(bodyBlock);
        for (const Node& child : node.children) genNode(child);
        if ((builder.GetInsertBlock()->empty() || !builder.GetInsertBlock()->back().isTerminator())) builder.CreateBr(stepBlock);
        builder.SetInsertPoint(stepBlock);
        current = builder.CreateLoad(iteratorType, iterator);
        llvm::Value* next = builder.CreateAdd(current, llvm::ConstantInt::get(iteratorType, 1));
        builder.CreateStore(next, iterator);
        builder.CreateBr(conditionBlock);
        builder.SetInsertPoint(afterBlock);
        restoreScope(parentScope);
        breakBlock = savedBreak;
        continueBlock = savedContinue;
    }
    else if (node.type == NODE_LOOP) {
        llvm::Function* function = builder.GetInsertBlock()->getParent();
        llvm::BasicBlock* headerBlock = llvm::BasicBlock::Create(context, "loop_header", function);
        llvm::BasicBlock* bodyBlock = llvm::BasicBlock::Create(context, "loop_body", function);
        llvm::BasicBlock* afterBlock = llvm::BasicBlock::Create(context, "loop_after", function);
        const auto savedBreak = breakBlock;
        const auto savedContinue = continueBlock;
        breakBlock = afterBlock;
        continueBlock = headerBlock;
        const ScopeSnapshot parentScope = saveScope();
        builder.CreateBr(headerBlock);
        builder.SetInsertPoint(headerBlock);
        builder.CreateBr(bodyBlock);
        builder.SetInsertPoint(bodyBlock);
        for (const Node& child : node.children) genNode(child);
        if ((builder.GetInsertBlock()->empty() || !builder.GetInsertBlock()->back().isTerminator())) builder.CreateBr(headerBlock);
        restoreScope(parentScope);
        builder.SetInsertPoint(afterBlock);
        breakBlock = savedBreak;
        continueBlock = savedContinue;
    }
    else if (node.type == NODE_LOOP_W || node.type == NODE_WHILE) {
        llvm::Function* func = builder.GetInsertBlock()->getParent();
        llvm::BasicBlock* condBlock =
            llvm::BasicBlock::Create(context, "cond", func);
        llvm::BasicBlock* bodyBlock =
            llvm::BasicBlock::Create(context, "body", func);
        llvm::BasicBlock* afterBlock =
            llvm::BasicBlock::Create(context, "after", func);
        auto savedBreak = breakBlock;
        auto savedContinue = continueBlock;
        breakBlock = afterBlock;
        continueBlock = condBlock;
        builder.CreateBr(condBlock);
        builder.SetInsertPoint(condBlock);
        llvm::Value* cond = toBool(genExpr(*node.left));
        builder.CreateCondBr(cond, bodyBlock, afterBlock);
        builder.SetInsertPoint(bodyBlock);
        const ScopeSnapshot parentScope = saveScope();
        for (const Node& n : node.children) genNode(n);
        if ((builder.GetInsertBlock()->empty() || !builder.GetInsertBlock()->back().isTerminator()))
            builder.CreateBr(condBlock);
        restoreScope(parentScope);
        builder.SetInsertPoint(afterBlock);
        breakBlock = savedBreak;
        continueBlock = savedContinue;
    }
}

void CodeGen::generate(const Node& program) {
    putsFunc = runtimeFunction("puts");
    abortFunc = runtimeFunction("abort");
    printfFunc = runtimeFunction("printf");

    for (const Node& node : program.children) {
        if (node.type != NODE_FUNC_DEF || node.isForwardDecl) continue;
        std::vector<llvm::Type*> parameterTypes;
        std::vector<std::string> parameterNames;
        for (const std::string& parameter : node.params) {
            const size_t separator = parameter.find(':');
            const std::string typeName = parameter.substr(0, separator);
            const std::string name = separator == std::string::npos
                ? std::string() : parameter.substr(separator + 1);
            parameterTypes.push_back(svType(typeName));
            parameterNames.push_back(typeName);
        }
        const std::string returnType = node.varType.empty() ? "rox" : node.varType;
        funcParamTypes[node.varName] = parameterNames;
        funcReturnTypes[node.varName] = returnType;
        llvm::FunctionType* functionType = llvm::FunctionType::get(
            svType(returnType), parameterTypes, false);
        // Префикс "sc.fn.": пользовательские функции не конфликтуют с main и рантаймом C.
        funcs[node.varName] = llvm::Function::Create(
            functionType, llvm::Function::InternalLinkage, "sc.fn." + node.varName, module);
    }

    // Сначала код верхнего уровня (main): он регистрирует структуры и создаёт
    // глобальные переменные, которые затем видят тела функций.
    llvm::FunctionType* funcType =
        llvm::FunctionType::get(builder.getInt32Ty(), false);
    llvm::Function* mainFunc =
        llvm::Function::Create(funcType,
            llvm::Function::ExternalLinkage, "main", module);
    llvm::BasicBlock* block =
        llvm::BasicBlock::Create(context, "entry", mainFunc);
    builder.SetInsertPoint(block);

    globalStorage = true;
    for (const Node& node : program.children)
        if (node.type != NODE_FUNC_DEF) genNode(node);
    globalStorage = false;

    builder.CreateRet(builder.getInt32(0));

    for (const Node& node : program.children)
        if (node.type == NODE_FUNC_DEF && !node.isForwardDecl) genNode(node);
}

bool CodeGen::verify(std::string& diagnostic) const {
    llvm::raw_string_ostream output(diagnostic);
    const bool invalid = llvm::verifyModule(module, &output);
    output.flush();
    return !invalid;
}

void CodeGen::print() {
    module.print(llvm::outs(), nullptr);
}
