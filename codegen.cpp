#include "diagnostics.h"
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/Intrinsics.h>
#include "codegen.h"
#include "type.h"
#include <llvm/Support/raw_ostream.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IR/GlobalVariable.h>
#include <algorithm>
#include <map>
#include <string_view>
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

// Служебный слот в начале текущей функции: alloca внутри цикла растила бы стек
// на каждой итерации.
llvm::AllocaInst* CodeGen::entryAlloca(llvm::Type* type, const char* name) {
    llvm::BasicBlock& entry = builder.GetInsertBlock()->getParent()->getEntryBlock();
    llvm::IRBuilder<> at(&entry, entry.getFirstInsertionPt());
    return at.CreateAlloca(type, nullptr, name);
}

// Единая таблица функций C-рантайма: одно имя — одна сигнатура.
namespace {
// Сигнатуры функций C, которые вызывает сгенерированный код. Коды типов:
// i — i32, l — i64, p — ptr, d — double, v — void. Таблица constexpr: раньше
// std::map из 29 записей строился заново при каждом вызове runtimeFunction.
struct RuntimeSignature {
    std::string_view name;
    char result;
    std::string_view params;
    bool variadic;
};
constexpr RuntimeSignature kRuntimeFunctions[] = {
    {"puts", 'i', "p", false},       {"printf", 'i', "p", true},
    {"dprintf", 'i', "ip", true},    {"scanf", 'i', "p", true},
    {"abort", 'v', "", false},       {"exit", 'v', "i", false},
    {"malloc", 'p', "l", false},     {"calloc", 'p', "ll", false},
    {"snprintf", 'i', "plp", true},  {"free", 'v', "p", false},
    {"memcpy", 'p', "ppl", false},   {"memset", 'p', "pil", false},
    {"strlen", 'l', "p", false},     {"strcmp", 'i', "pp", false},
    {"strstr", 'p', "pp", false},    {"fopen", 'p', "pp", false},
    {"fgets", 'p', "pip", false},    {"fgetc", 'i', "p", false},
    {"getchar", 'i', "", false},     {"realloc", 'p', "pl", false},
    {"strtoll", 'l', "ppi", false},  {"fputs", 'i', "pp", false},
    {"fclose", 'i', "p", false},     {"rand", 'i', "", false},
    {"pow", 'd', "dd", false},       {"sqrt", 'd', "d", false},
    {"floor", 'd', "d", false},      {"ceil", 'd', "d", false},
    {"round", 'd', "d", false},
};
} // namespace

llvm::Function* CodeGen::runtimeFunction(std::string_view name) {
    if (llvm::Function* existing = module.getFunction(llvm::StringRef(name))) return existing;
    const auto* entry = std::ranges::find(kRuntimeFunctions, name, &RuntimeSignature::name);
    if (entry == std::ranges::end(kRuntimeFunctions))
        throw std::logic_error("внутренняя ошибка codegen: функция рантайма '" +
                               std::string(name) + "' отсутствует в таблице");
    auto llvmTy = [&](char code) -> llvm::Type* {
        switch (code) {
        case 'i': return builder.getInt32Ty();
        case 'l': return builder.getInt64Ty();
        case 'p': return builder.getPtrTy();
        case 'd': return builder.getDoubleTy();
        default:  return builder.getVoidTy();
        }
    };
    std::vector<llvm::Type*> params;
    for (char code : entry->params) params.push_back(llvmTy(code));
    llvm::FunctionType* type = llvm::FunctionType::get(llvmTy(entry->result), params, entry->variadic);
    return llvm::Function::Create(type, llvm::Function::ExternalLinkage, llvm::StringRef(name), module);
}

CodeGen::ScopeSnapshot CodeGen::saveScope() const {
    return {vars, unsignedVars, charVars, constVars, integerWidths, arrays, arrayLengths,
            arrayElementTypes, structTypesByVariable, dynArrays, dynArrayElementTypes, pointeeTypes};
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
    dynArrays = scope.dynArrays;
    dynArrayElementTypes = scope.dynArrayElementTypes;
    pointeeTypes = scope.pointeeTypes;
}

// Keep registrations when emitting a return path: sibling CFG paths still need them.
void CodeGen::emitDeferred(size_t base) {
    const ScopeSnapshot current = saveScope();
    for (size_t i = deferred_.size(); i > base; --i) {
        restoreScope(deferred_[i - 1].bindings);
        genNode(*deferred_[i - 1].statement);
    }
    restoreScope(current);
}

void CodeGen::genScope(const std::vector<Node>& statements) {
    const size_t base = deferred_.size();
    for (const Node& statement : statements) {
        if (blockHasTerminator(builder.GetInsertBlock())) break;
        genNode(statement);
    }
    if (!blockHasTerminator(builder.GetInsertBlock())) emitDeferred(base);
    deferred_.resize(base);
}

llvm::Value* CodeGen::getStringPtr(llvm::StringRef str) {
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
        return std::max(cachedWidth(node.left), cachedWidth(node.right));
    return 32;
}

unsigned CodeGen::cachedWidth(const std::shared_ptr<Node>& child) const {
    if (auto found = widthCache_.find(child.get()); found != widthCache_.end()) return found->second;
    const unsigned width = integerWidth(*child);
    widthCache_.emplace(child.get(), width);
    return width;
}

bool CodeGen::cachedUnsigned(const std::shared_ptr<Node>& child) const {
    if (auto found = unsignedCache_.find(child.get()); found != unsignedCache_.end()) return found->second;
    const bool isUnsigned = isUnsignedExpr(*child);
    unsignedCache_.emplace(child.get(), isUnsigned);
    return isUnsigned;
}

// Ошибка выполнения: если failed истинно, печатает "SC runtime error: <message> at line L, column C"
// и завершает программу с кодом 1. detail (строка C) подставляется вместо %s в message.
void CodeGen::emitRuntimeFailureIf(llvm::Value* failed, std::string_view message,
                                   const Node& location, llvm::Value* detail) {
    llvm::Function* function = builder.GetInsertBlock()->getParent();
    llvm::BasicBlock* fail = llvm::BasicBlock::Create(context, "runtime_error", function);
    llvm::BasicBlock* ok = llvm::BasicBlock::Create(context, "runtime_ok", function);
    builder.CreateCondBr(failed, fail, ok);
    builder.SetInsertPoint(fail);
    std::string text = "SC runtime error: ";
    text.append(message).append(" at line ").append(std::to_string(location.line))
        .append(", column ").append(std::to_string(location.column));
    // Диагностика — в stderr (D-007), stdout программы остаётся чистым.
    llvm::Function* report = runtimeFunction("dprintf");
    llvm::Value* stderrFd = builder.getInt32(2);
    if (detail)
        builder.CreateCall(report, {stderrFd, getStringPtr(text + "\n"), detail});
    else
        builder.CreateCall(report, {stderrFd, getStringPtr("%s\n"), getStringPtr(text)});
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

// sc.read_line(file, status): строка любой длины без '\n' (и '\r' перед ним) в
// буфере из malloc. file == null — стандартный ввод. status: 0 — прочитано,
// 1 — конец ввода до первого символа, 2 — не хватило памяти.
llvm::Function* CodeGen::readLineFunction() {
    if (llvm::Function* existing = module.getFunction("sc.read_line")) return existing;
    llvm::Type* ptrTy = builder.getPtrTy();
    llvm::Type* i64 = builder.getInt64Ty();
    llvm::Function* function = llvm::Function::Create(
        llvm::FunctionType::get(ptrTy, {ptrTy, ptrTy}, false),
        llvm::Function::InternalLinkage, "sc.read_line", module);
    llvm::Value* file = function->getArg(0);
    llvm::Value* status = function->getArg(1);
    llvm::IRBuilderBase::InsertPointGuard guard(builder);
    auto block = [&](const char* name) { return llvm::BasicBlock::Create(context, name, function); };
    llvm::BasicBlock* entry = block("entry");
    llvm::BasicBlock* loop = block("loop");
    llvm::BasicBlock* store = block("store");
    llvm::BasicBlock* grow = block("grow");
    llvm::BasicBlock* put = block("put");
    llvm::BasicBlock* done = block("done");
    llvm::BasicBlock* eof = block("eof");
    llvm::BasicBlock* finish = block("finish");
    llvm::BasicBlock* oom = block("oom");
    builder.SetInsertPoint(entry);
    llvm::Value* cap = builder.CreateAlloca(i64);
    llvm::Value* len = builder.CreateAlloca(i64);
    llvm::Value* buf = builder.CreateAlloca(ptrTy);
    builder.CreateStore(builder.getInt64(64), cap);
    builder.CreateStore(builder.getInt64(0), len);
    llvm::Value* first = builder.CreateCall(runtimeFunction("malloc"), {builder.getInt64(64)});
    builder.CreateStore(first, buf);
    builder.CreateCondBr(builder.CreateIsNull(first), oom, loop);

    builder.SetInsertPoint(loop);
    llvm::BasicBlock* readFile = block("read_file");
    llvm::BasicBlock* readStdin = block("read_stdin");
    llvm::BasicBlock* gotChar = block("got_char");
    builder.CreateCondBr(builder.CreateIsNull(file), readStdin, readFile);
    builder.SetInsertPoint(readFile);
    llvm::Value* fromFile = builder.CreateCall(runtimeFunction("fgetc"), {file});
    builder.CreateBr(gotChar);
    builder.SetInsertPoint(readStdin);
    llvm::Value* fromStdin = builder.CreateCall(runtimeFunction("getchar"), {});
    builder.CreateBr(gotChar);
    builder.SetInsertPoint(gotChar);
    llvm::PHINode* c = builder.CreatePHI(builder.getInt32Ty(), 2);
    c->addIncoming(fromFile, readFile);
    c->addIncoming(fromStdin, readStdin);
    llvm::Value* atEnd = builder.CreateICmpEQ(c, builder.getInt32(-1));
    llvm::Value* newline = builder.CreateICmpEQ(c, builder.getInt32('\n'));
    builder.CreateCondBr(builder.CreateOr(atEnd, newline), done, store);

    builder.SetInsertPoint(store);
    llvm::Value* used = builder.CreateLoad(i64, len);
    llvm::Value* capacity = builder.CreateLoad(i64, cap);
    builder.CreateCondBr(builder.CreateICmpUGE(builder.CreateAdd(used, builder.getInt64(1)), capacity),
                         grow, put);

    builder.SetInsertPoint(grow);
    llvm::Value* doubled = builder.CreateMul(builder.CreateLoad(i64, cap), builder.getInt64(2));
    llvm::Value* old = builder.CreateLoad(ptrTy, buf);
    llvm::Value* bigger = builder.CreateCall(runtimeFunction("realloc"), {old, doubled});
    llvm::BasicBlock* grown = block("grown");
    builder.CreateCondBr(builder.CreateIsNull(bigger), oom, grown);
    builder.SetInsertPoint(grown);
    builder.CreateStore(bigger, buf);
    builder.CreateStore(doubled, cap);
    builder.CreateBr(put);

    builder.SetInsertPoint(put);
    llvm::Value* at = builder.CreateLoad(i64, len);
    builder.CreateStore(builder.CreateTrunc(c, builder.getInt8Ty()),
                        builder.CreateGEP(builder.getInt8Ty(), builder.CreateLoad(ptrTy, buf), at));
    builder.CreateStore(builder.CreateAdd(at, builder.getInt64(1)), len);
    builder.CreateBr(loop);

    builder.SetInsertPoint(done);
    llvm::Value* count = builder.CreateLoad(i64, len);
    builder.CreateCondBr(builder.CreateAnd(atEnd, builder.CreateICmpEQ(count, builder.getInt64(0))),
                         eof, finish);
    builder.SetInsertPoint(eof);
    builder.CreateStore(builder.getInt32(1), status);
    builder.CreateStore(builder.getInt8(0), builder.CreateLoad(ptrTy, buf));
    builder.CreateRet(builder.CreateLoad(ptrTy, buf));

    builder.SetInsertPoint(finish);
    llvm::Value* text = builder.CreateLoad(ptrTy, buf);
    llvm::Value* hasChars = builder.CreateICmpUGT(count, builder.getInt64(0));
    llvm::Value* lastIndex = builder.CreateSelect(hasChars, builder.CreateSub(count, builder.getInt64(1)),
                                                  builder.getInt64(0));
    llvm::Value* last = builder.CreateLoad(builder.getInt8Ty(),
                                           builder.CreateGEP(builder.getInt8Ty(), text, lastIndex));
    llvm::Value* carriage = builder.CreateAnd(hasChars, builder.CreateICmpEQ(last, builder.getInt8('\r')));
    llvm::Value* end = builder.CreateSelect(carriage, lastIndex, count);
    builder.CreateStore(builder.getInt8(0), builder.CreateGEP(builder.getInt8Ty(), text, end));
    builder.CreateStore(builder.getInt32(0), status);
    builder.CreateRet(text);

    builder.SetInsertPoint(oom);
    builder.CreateCall(runtimeFunction("free"), {builder.CreateLoad(ptrTy, buf)});
    builder.CreateStore(builder.getInt32(2), status);
    builder.CreateRet(llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptrTy)));
    return function;
}

llvm::Value* CodeGen::emitReadLine(llvm::Value* file, const Node& location, bool eofIsError) {
    llvm::Value* status = entryAlloca(builder.getInt32Ty(), "read_status");
    llvm::Value* line = builder.CreateCall(readLineFunction(), {file, status});
    llvm::Value* code = builder.CreateLoad(builder.getInt32Ty(), status);
    emitRuntimeFailureIf(builder.CreateICmpEQ(code, builder.getInt32(2)), "out of memory", location);
    if (eofIsError)
        emitRuntimeFailureIf(builder.CreateICmpEQ(code, builder.getInt32(1)),
                             "unexpected end of input", location);
    return line;
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
    emitRuntimeFailureIf(builder.CreateNot(inBounds), "array index out of bounds", location);
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

// Размер для new T[n] / resize(n): в i64 с учётом знака; отрицательный — ошибка выполнения.
llvm::Value* CodeGen::emitArraySize(const Node& sizeExpr, const Node& location) {
    llvm::Value* size = genExpr(sizeExpr);
    const bool isUnsigned = isUnsignedExpr(sizeExpr);
    if (size->getType()->getIntegerBitWidth() < 64)
        size = isUnsigned ? builder.CreateZExt(size, builder.getInt64Ty())
                          : builder.CreateSExt(size, builder.getInt64Ty());
    // u64 больше INT64_MAX тоже не может быть длиной: длина хранится как i64.
    emitRuntimeFailureIf(builder.CreateICmpSLT(size, builder.getInt64(0)),
                         isUnsigned ? "array size too large" : "negative array size", location);
    return size;
}

void CodeGen::growDynArray(const std::string& varName, llvm::Value* newCapacity,
                           const Node& location) {
    auto found = dynArrays.find(varName);
    if (found == dynArrays.end()) return;
    llvm::StructType* descType = dynArrayType();
    llvm::Value* desc = found->second;
    llvm::Type* elementType = dynArrayElementTypes[varName];
    if (!elementType) elementType = builder.getInt32Ty();
    llvm::Value* dataPtr = builder.CreateLoad(builder.getPtrTy(),
        builder.CreateStructGEP(descType, desc, 0));
    llvm::Function* mallocF = runtimeFunction("malloc");
    llvm::Value* product = builder.CreateBinaryIntrinsic(llvm::Intrinsic::umul_with_overflow,
        newCapacity, builder.getInt64(elementType->getPrimitiveSizeInBits() / 8));
    emitRuntimeFailureIf(builder.CreateExtractValue(product, 1), "out of memory", location);
    llvm::Value* bytes = builder.CreateExtractValue(product, 0);
    llvm::Value* newData = builder.CreateCall(mallocF, {bytes});
    emitRuntimeFailureIf(builder.CreateIsNull(newData), "out of memory", location);
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

bool CodeGen::fieldIsUnsigned(const std::string& variable, std::string_view path) const {
    auto structName = structTypesByVariable.find(variable);
    if (structName == structTypesByVariable.end()) return false;
    // Обход пути `a.b.c` по string_view и указателям на имена типов — без
    // копии каждого сегмента и каждого имени типа.
    const std::string* current = &structName->second;
    const std::string* type = nullptr;
    for (std::string_view rest = path;;) {
        const size_t dot = rest.find('.');
        const std::string_view name = rest.substr(0, dot);
        auto def = structDefs.find(*current);
        if (def == structDefs.end()) return false;
        type = nullptr;
        for (const auto& field : def->second) if (field.name == name) type = &field.type;
        if (!type || type->empty()) return false;
        if (dot == std::string_view::npos) break;
        current = type;
        rest.remove_prefix(dot + 1);
    }
    const sc::Type t = sc::Type::from(*type);
    return t.isUnsigned() || t.isChar();
}

void CodeGen::checkNarrowStore(llvm::Value* value, const Node& valueNode, llvm::Type* target,
                               bool targetUnsigned, const Node& location) {
    if (!value->getType()->isIntegerTy() || !target->isIntegerTy()) return;
    const unsigned sourceWidth = value->getType()->getIntegerBitWidth();
    const unsigned targetWidth = target->getIntegerBitWidth();
    if (sourceWidth <= targetWidth || targetWidth == 1) return;
    const bool sourceUnsigned = isUnsignedExpr(valueNode);
    llvm::Type* wideType = builder.getIntNTy(sourceWidth + 1);
    llvm::Value* v = sourceUnsigned ? builder.CreateZExt(value, wideType) : builder.CreateSExt(value, wideType);
    const unsigned wide = sourceWidth + 1;
    llvm::APInt low = targetUnsigned ? llvm::APInt(wide, 0)
        : llvm::APInt::getSignedMinValue(targetWidth).sext(wide);
    llvm::APInt high = targetUnsigned ? llvm::APInt::getMaxValue(targetWidth).zext(wide)
        : llvm::APInt::getSignedMaxValue(targetWidth).sext(wide);
    llvm::Value* inRange = builder.CreateAnd(builder.CreateICmpSGE(v, builder.getInt(low)),
                                             builder.CreateICmpSLE(v, builder.getInt(high)));
    emitRuntimeFailureIf(builder.CreateNot(inRange), "integer overflow", location);
}

// Беззнаковый ли тип SC по имени (char в арифметике — как u8).
static bool unsignedTypeName(std::string_view type) {
    const sc::Type t = sc::Type::from(type);
    return t.isUnsigned() || t.isChar();
}

llvm::Value* CodeGen::nullChecked(llvm::Value* pointer, const Node& location) {
    emitRuntimeFailureIf(builder.CreateICmpEQ(pointer,
        llvm::ConstantPointerNull::get(builder.getPtrTy())), "null pointer dereference", location);
    return pointer;
}

bool CodeGen::isUnsignedExpr(const Node& node) const {
    if (node.type == NODE_NUMBER) return false;
    if (node.type == NODE_IDENTIFIER) {
        auto isChar = charVars.find(node.value);
        if (isChar != charVars.end() && isChar->second) return true;
    }
    if (node.type == NODE_STRUCT_FIELD) return fieldIsUnsigned(node.varName, node.op);
    if (node.type == NODE_ARRAY_ACCESS) {
        auto found = unsignedElements.find(node.varName);
        return found != unsignedElements.end() && found->second;
    }
    if (node.type == NODE_FUNC_CALL) {
        auto found = funcReturnTypes.find(node.varName);
        if (found == funcReturnTypes.end()) return false;
        const sc::Type result = sc::Type::from(found->second);
        return result.isUnsigned();
    }
    if (node.type == NODE_CAST) {
        const sc::Type target = sc::Type::from(node.varType);
        return target.isUnsigned() || target.isChar();
    }
    if (node.type == NODE_IDENTIFIER) {
        auto found = unsignedVars.find(node.value);
        return found != unsignedVars.end() && found->second;
    }
    if (node.type == NODE_BINOP && node.left && node.right) {
        if (node.op == ">" || node.op == "<" || node.op == "==" || node.op == "!=")
            return cachedUnsigned(node.left);
        return cachedUnsigned(node.left) || cachedUnsigned(node.right);
    }
    return false;
}

void CodeGen::initStructLiteral(llvm::Value* storage, llvm::StructType* aggregateType,
                                const std::string& structName, const Node& literal) {
    if (!structDefs.contains(structName)) {
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
        if (named.contains(field.name)) {
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
            checkNarrowStore(value, *initializer, fieldType, unsignedTypeName(field.type), *initializer);
            value = castTo(value, fieldType, isUnsignedExpr(*initializer));
            builder.CreateStore(value, address);
        }
    }
}

llvm::Value* CodeGen::genExpr(const Node& node) {
    if (!precomputed_.empty()) {
        auto ready = precomputed_.find(&node);
        if (ready != precomputed_.end()) return ready->second;
    }
    if (node.type == NODE_BINOP) {
        // Левый операнд генерируется здесь, а не в genBinary: её кадр большой.
        llvm::Value* left = genExpr(*node.left);
        return genBinary(node, left);
    }
    return genOtherExpr(node);
}

llvm::Value* CodeGen::genOtherExpr(const Node& node) {
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
                throw sc::CompileFailure{1};
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
            throw sc::CompileFailure{1};
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
            // Final field: load and return the value (или адрес — для присваивания)
            if (structFieldAddressOnly_) { lastFieldType_ = fieldType; return currentPtr; }
            return builder.CreateLoad(fieldType, currentPtr);
        }
        pos = dotPos + 1;
    }
    return nullptr;
    }
    if (node.type == NODE_IDENTIFIER && node.value == "null" && !vars.contains(node.value))
        return llvm::ConstantPointerNull::get(builder.getPtrTy());
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
        if (dynArrays.contains(node.varName)) {
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
        if (vars.contains(node.varName) &&
            storageType(vars[node.varName])->isPointerTy()) {
            llvm::Value* ptr = nullChecked(builder.CreateLoad(
                builder.getPtrTy(), vars[node.varName]), node);
            llvm::Value* idx = genExpr(*node.left);
            auto pointee = pointeeTypes.find(node.varName);  // p[i] читает тип x из ptr p = &x
            llvm::Type* elementType = pointee != pointeeTypes.end() ? pointee->second : builder.getInt32Ty();
            llvm::Value* gep = builder.CreateGEP(elementType, ptr, {idx});
            return builder.CreateLoad(elementType, gep);
        }
        if (arrays.contains(node.varName)) {
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
            return builder.CreateLoad(type, nullChecked(ptr, node));
        }
        codegenFailure(node, "неизвестный указатель '" + node.varName + "'");
    }
    if (node.type == NODE_ARRAY_LEN || node.type == NODE_ARRAY_CAP) {
        auto found = dynArrays.find(node.varName);
        // Статический массив: длина и ёмкость — его фиксированная длина.
        if (found == dynArrays.end() && arrayLengths.contains(node.varName))
            return builder.getInt64(arrayLengths[node.varName]);
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
        const sc::Type targetType = sc::Type::from(node.varType);
        const bool targetUnsigned = targetType.isUnsigned() || targetType.isChar();
        if (val->getType()->isFloatingPointTy() && target->isIntegerTy()) {
            // NaN, бесконечность и значения вне диапазона — ошибка выполнения.
            const unsigned width = target->getIntegerBitWidth();
            llvm::Value* x = val->getType()->isDoubleTy() ? val : builder.CreateFPExt(val, builder.getDoubleTy());
            const double high = std::ldexp(1.0, static_cast<int>(targetUnsigned ? width : width - 1));
            llvm::Value* aboveLow = targetUnsigned
                ? builder.CreateFCmpOGT(x, llvm::ConstantFP::get(builder.getDoubleTy(), -1.0))
                : builder.CreateFCmpOGE(x, llvm::ConstantFP::get(builder.getDoubleTy(), -high));
            llvm::Value* inRange = builder.CreateAnd(aboveLow,
                builder.CreateFCmpOLT(x, llvm::ConstantFP::get(builder.getDoubleTy(), high)));
            emitRuntimeFailureIf(builder.CreateNot(inRange),
                                 "value out of range for type '" + node.varType + "'", node);
            return targetUnsigned ? builder.CreateFPToUI(x, target) : builder.CreateFPToSI(x, target);
        }
        if (val->getType()->isIntegerTy() && target->isFloatingPointTy()) {
            llvm::Value* converted = isUnsignedExpr(*node.left)
                ? builder.CreateUIToFP(val, target) : builder.CreateSIToFP(val, target);
            return converted;
        }
        if (val->getType()->isIntegerTy() && target->isIntegerTy()) {
            // Значение должно помещаться в целевой тип: проверка в общем типе шире обоих на бит.
            const bool sourceUnsigned = isUnsignedExpr(*node.left);
            const unsigned sourceWidth = val->getType()->getIntegerBitWidth();
            const unsigned targetWidth = target->getIntegerBitWidth();
            const bool lossless = (sourceUnsigned == targetUnsigned && targetWidth >= sourceWidth) ||
                                  (sourceUnsigned && !targetUnsigned && targetWidth > sourceWidth);
            if (!lossless) {
                const unsigned wide = std::max(sourceWidth, targetWidth) + 1;
                llvm::Type* wideType = builder.getIntNTy(wide);
                llvm::Value* v = sourceUnsigned ? builder.CreateZExt(val, wideType) : builder.CreateSExt(val, wideType);
                llvm::APInt low = targetUnsigned ? llvm::APInt(wide, 0)
                    : llvm::APInt::getSignedMinValue(targetWidth).sext(wide);
                llvm::APInt high = targetUnsigned ? llvm::APInt::getMaxValue(targetWidth).zext(wide)
                    : llvm::APInt::getSignedMaxValue(targetWidth).sext(wide);
                llvm::Value* inRange = builder.CreateAnd(
                    builder.CreateICmpSGE(v, builder.getInt(low)),
                    builder.CreateICmpSLE(v, builder.getInt(high)));
                emitRuntimeFailureIf(builder.CreateNot(inRange),
                                     "value out of range for type '" + node.varType + "'", node);
            }
            if (sourceWidth == targetWidth) return val;
            if (val->getType()->getIntegerBitWidth() > target->getIntegerBitWidth())
                return builder.CreateTrunc(val, target);
            return sourceUnsigned
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
        return emitReadLine(fp, node, false);
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
    if (node.type == NODE_INPUT && node.varType == "str") {
        llvm::Function* printfF = runtimeFunction("printf");
        llvm::Value* prompt = getStringPtr(node.value);
        builder.CreateCall(printfF, {prompt});
        return emitReadLine(llvm::ConstantPointerNull::get(builder.getPtrTy()), node, false);
    }
    if (node.type == NODE_INPUT) {
        llvm::Function* printfF = runtimeFunction("printf");
        llvm::Value* prompt = getStringPtr(node.value);
        builder.CreateCall(printfF, {prompt});
        // Одна строка ввода — одно целое: пробелы вокруг допустимы, остальное — ошибка.
        llvm::Value* line = emitReadLine(llvm::ConstantPointerNull::get(builder.getPtrTy()), node, true);
        llvm::Value* endSlot = entryAlloca(builder.getPtrTy(), "input_end");
        llvm::Value* value = builder.CreateCall(runtimeFunction("strtoll"),
                                                {line, endSlot, builder.getInt32(10)});
        llvm::Value* end = builder.CreateLoad(builder.getPtrTy(), endSlot);
        llvm::Function* function = builder.GetInsertBlock()->getParent();
        llvm::BasicBlock* skip = llvm::BasicBlock::Create(context, "input_skip", function);
        llvm::BasicBlock* next = llvm::BasicBlock::Create(context, "input_next", function);
        llvm::BasicBlock* checked = llvm::BasicBlock::Create(context, "input_checked", function);
        llvm::BasicBlock* before = builder.GetInsertBlock();
        builder.CreateBr(skip);
        builder.SetInsertPoint(skip);
        llvm::PHINode* cursor = builder.CreatePHI(builder.getPtrTy(), 2, "input_cursor");
        cursor->addIncoming(end, before);
        llvm::Value* ch = builder.CreateLoad(builder.getInt8Ty(), cursor);
        llvm::Value* space = builder.CreateOr(builder.CreateICmpEQ(ch, builder.getInt8(' ')),
                                              builder.CreateICmpEQ(ch, builder.getInt8('\t')));
        builder.CreateCondBr(space, next, checked);
        builder.SetInsertPoint(next);
        cursor->addIncoming(builder.CreateGEP(builder.getInt8Ty(), cursor, builder.getInt64(1)), next);
        builder.CreateBr(skip);
        builder.SetInsertPoint(checked);
        llvm::Value* noDigits = builder.CreateICmpEQ(end, line);
        llvm::Value* trailing = builder.CreateICmpNE(ch, builder.getInt8(0));
        emitRuntimeFailureIf(builder.CreateOr(noDigits, trailing), "invalid integer input", node);
        llvm::Value* tooSmall = builder.CreateICmpSLT(value, builder.getInt64(INT32_MIN));
        llvm::Value* tooLarge = builder.CreateICmpSGT(value, builder.getInt64(INT32_MAX));
        emitRuntimeFailureIf(builder.CreateOr(tooSmall, tooLarge), "integer input out of range", node);
        return builder.CreateTrunc(value, builder.getInt32Ty());
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
            // Приведение — в конце своей ветви, до перехода в merge: PHI ждёт
            // значения, вычисленные в предшествующих блоках.
            builder.SetInsertPoint(trueEnd->getTerminator());
            trueValue = castTo(trueValue, resultType, isUnsignedExpr(*node.right));
            builder.SetInsertPoint(falseEnd->getTerminator());
            falseValue = castTo(falseValue, resultType, isUnsignedExpr(*node.extra));
        }
        builder.SetInsertPoint(mergeBlock);
        llvm::PHINode* result = builder.CreatePHI(resultType, 2, "ternary_result");
        result->addIncoming(trueValue, trueEnd);
        result->addIncoming(falseValue, falseEnd);
        return result;
    }
    return genCallExpr(node);
}

// Бинарные операторы отдельно от остальных узлов: цепочка `a + b + …` рекурсивно
// рекурсивно проходит только через маленький кадр genExpr.
llvm::Value* CodeGen::genBinary(const Node& node, llvm::Value* L) {
    if (node.type == NODE_BINOP) {
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
            {
                const unsigned width = integerResult->getIntegerBitWidth();
                const double low = unsignedResult ? 0.0 : -std::ldexp(1.0, static_cast<int>(width) - 1);
                const double high = std::ldexp(1.0, static_cast<int>(unsignedResult ? width : width - 1));
                llvm::Value* inRange = builder.CreateAnd(
                    builder.CreateFCmpOGE(result, llvm::ConstantFP::get(builder.getDoubleTy(), low)),
                    builder.CreateFCmpOLT(result, llvm::ConstantFP::get(builder.getDoubleTy(), high)));
                emitRuntimeFailureIf(builder.CreateNot(inRange), "integer overflow", node);
            }
            return unsignedResult ? builder.CreateFPToUI(result, integerResult)
                                  : builder.CreateFPToSI(result, integerResult);
        }
        if (node.varType == "string-compare") {  // a == b / a != b для строк
            llvm::Value* order = builder.CreateCall(runtimeFunction("strcmp"), {L, R});
            return node.op == "==" ? builder.CreateICmpEQ(order, builder.getInt32(0))
                                   : builder.CreateICmpNE(order, builder.getInt32(0));
        }
        if (L->getType()->isPointerTy() || R->getType()->isPointerTy()) {
            if (node.op == "==") return builder.CreateICmpEQ(L, R);
            if (node.op == "!=") return builder.CreateICmpNE(L, R);
            codegenFailure(node, "недопустимая операция над указателями");
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
            if (node.op == "%") return builder.CreateFRem(L, R);  // как fmod в C: знак делимого
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
            emitRuntimeFailureIf(builder.CreateNot(valid), "shift amount out of range", node);
            if (node.op == "<<") return builder.CreateShl(L, R);
            return isUnsignedExpr(*node.left) ? builder.CreateLShr(L, R) : builder.CreateAShr(L, R);
        }
        // Целые: переполнение, деление на ноль и MIN / -1 — ошибка выполнения, не порча значения.
        const bool unsignedArith = isUnsignedExpr(*node.left) || isUnsignedExpr(*node.right);
        if (node.op == "+" || node.op == "-" || node.op == "*") {
            llvm::Intrinsic::ID id =
                node.op == "+" ? (unsignedArith ? llvm::Intrinsic::uadd_with_overflow : llvm::Intrinsic::sadd_with_overflow) :
                node.op == "-" ? (unsignedArith ? llvm::Intrinsic::usub_with_overflow : llvm::Intrinsic::ssub_with_overflow) :
                                 (unsignedArith ? llvm::Intrinsic::umul_with_overflow : llvm::Intrinsic::smul_with_overflow);
#if LLVM_VERSION_MAJOR >= 20
            llvm::Function* checked = llvm::Intrinsic::getOrInsertDeclaration(&module, id, {common});
#else
            llvm::Function* checked = llvm::Intrinsic::getDeclaration(&module, id, {common});
#endif
            llvm::Value* pair = builder.CreateCall(checked, {L, R});
            emitRuntimeFailureIf(builder.CreateExtractValue(pair, 1), "integer overflow", node);
            return builder.CreateExtractValue(pair, 0);
        }
        if (node.op == "/" || node.op == "//" || node.op == "%") {
            const unsigned width = common->getIntegerBitWidth();
            emitRuntimeFailureIf(builder.CreateICmpEQ(R, builder.getIntN(width, 0)),
                                 "division by zero", node);
            if (!unsignedArith) {
                llvm::Value* minimum = builder.CreateICmpEQ(L,
                    builder.getInt(llvm::APInt::getSignedMinValue(width)));
                llvm::Value* minusOne = builder.CreateICmpEQ(R, llvm::ConstantInt::getSigned(common, -1));
                emitRuntimeFailureIf(builder.CreateAnd(minimum, minusOne), "integer overflow", node);
            }
            if (node.op == "%") return unsignedArith ? builder.CreateURem(L, R) : builder.CreateSRem(L, R);
            return unsignedArith ? builder.CreateUDiv(L, R) : builder.CreateSDiv(L, R);
        }
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
    codegenFailure(node, "неподдерживаемый бинарный узел");
}

llvm::Value* CodeGen::genCallExpr(const Node& node) {
    if (node.type == NODE_FUNC_CALL) {
        std::string name = node.varName;
        // математика
        if (name == "pow" && node.varType == "int-pow" && node.args.size() == 2) {
            // pow двух целых — то же, что `a ** b`: целое с проверкой переполнения (D-009).
            Node power = node;
            power.type = NODE_BINOP;
            power.op = "**";
            power.left = std::make_shared<Node>(node.args[0]);
            power.right = std::make_shared<Node>(node.args[1]);
            power.args.clear();
            return genExpr(power);
        }
        if (name == "sqrt" || name == "pow" || name == "floor" ||
            name == "ceil" || name == "round") {
            const std::string mathName = name;
            llvm::Function* function = runtimeFunction(mathName);
            std::vector<llvm::Value*> args;
            for (const Node& argument : node.args) {
                llvm::Value* value = genExpr(argument);
                if (value->getType()->isIntegerTy())
                    value = isUnsignedExpr(argument)
                        ? builder.CreateUIToFP(value, builder.getDoubleTy())
                        : builder.CreateSIToFP(value, builder.getDoubleTy());
                else if (value->getType()->isFloatTy())
                    value = builder.CreateFPExt(value, builder.getDoubleTy());
                args.push_back(value);
            }
            return builder.CreateCall(function, args);  // f64 и для целых аргументов (D-009)
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
        if (name == "replace" && node.args.size() == 3) {
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
        if (name == "floor" || name == "ceil" || name == "round") {
            std::string cname = name;
            llvm::Function* f = runtimeFunction(cname);
            llvm::Value* arg = genExpr(node.args[0]);
            if (arg->getType()->isIntegerTy())
                arg = builder.CreateSIToFP(arg, builder.getDoubleTy());
            llvm::Value* res = builder.CreateCall(f, {arg});
            return builder.CreateFPToSI(res, builder.getInt32Ty());
        }
        if (name == "rand") {
            llvm::Function* f = runtimeFunction("rand");
            return builder.CreateCall(f, {});
        }
        if (name == "len") {
            llvm::Function* strlenF = runtimeFunction("strlen");
            llvm::Value* str = genExpr(node.args[0]);
            return builder.CreateTrunc(builder.CreateCall(strlenF, {str}), builder.getInt32Ty());
        }
        // пользовательские функции
        if (funcs.contains(name)) {
            std::vector<llvm::Value*> argVals;
            llvm::Function* fn = funcs[name];
            const auto& ptypes = funcParamTypes[name];
            for (size_t i = 0; i < node.args.size(); i++) {
                llvm::Value* v = genExpr(node.args[i]);
                if (i < ptypes.size()) {
                    checkNarrowStore(v, node.args[i], svType(ptypes[i]), unsignedTypeName(ptypes[i]),
                                     node.args[i]);
                    v = castTo(v, svType(ptypes[i]), isUnsignedExpr(node.args[i]));
                }
                argVals.push_back(v);
            }
            llvm::Value* ret = builder.CreateCall(fn, argVals);
            std::string rt = funcReturnTypes.contains(name)
                ? funcReturnTypes[name] : "i32";
            return castTo(ret, svType(rt));
        }
        codegenFailure(node, "неизвестная функция '" + name + "' при генерации LLVM");
    }
    codegenFailure(node, "неподдерживаемый узел выражения");
}

void CodeGen::genNode(const Node& node) {
    if (!unsignedCache_.empty()) unsignedCache_.clear();
    if (!widthCache_.empty()) widthCache_.clear();
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
                       charVars.contains(node.left->value) && charVars[node.left->value]) {
                llvm::Value* promoted = builder.CreateZExt(val, builder.getInt32Ty());
                llvm::Value* fmt = getStringPtr("%c\n");
                builder.CreateCall(printfFunc, {fmt, promoted});
            } else if (val->getType()->isIntegerTy() &&
                       val->getType()->getIntegerBitWidth() < 32) {
                const bool isUnsigned = isUnsignedExpr(*node.left) ||
                    val->getType()->getIntegerBitWidth() == 1;  // bool: 1, не -1
                val = isUnsigned ? builder.CreateZExt(val, builder.getInt32Ty())
                                 : builder.CreateSExt(val, builder.getInt32Ty());
                llvm::Value* fmt = getStringPtr("%d\n");
                builder.CreateCall(printfFunc, {fmt, val});
            } else if (val->getType()->isIntegerTy(64)) {
                const bool isUnsigned = isUnsignedExpr(*node.left);
                llvm::Value* fmt = getStringPtr(isUnsigned ? "%llu\n" : "%lld\n");
                builder.CreateCall(printfFunc, {fmt, val});
            } else {
                const bool isUnsigned = isUnsignedExpr(*node.left);
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
            llvm::Value* size = emitArraySize(*alloc.left, alloc);
            // nw T[n]: длина n, элементы обнулены (как new T[n]() в C++ и make([]T, n) в Go).
            // calloc(0) может вернуть NULL, поэтому выделяется минимум один элемент.
            llvm::Value* count = builder.CreateSelect(
                builder.CreateICmpEQ(size, builder.getInt64(0)), builder.getInt64(1), size);
            llvm::Value* data = builder.CreateCall(runtimeFunction("calloc"), {count,
                builder.getInt64(elementType->getPrimitiveSizeInBits() / 8)});
            emitRuntimeFailureIf(builder.CreateIsNull(data), "out of memory", alloc);
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
            unsignedElements[node.varName] = sc::Type::from(alloc.varType).isUnsigned();
            vars[node.varName] = desc;
            constVars[node.varName] = node.isConst;
            return;
        }
        // ptr p = &x: запомнить тип x, чтобы *p читал и писал значение нужного типа.
        pointeeTypes.erase(node.varName);
        if (node.left->type == NODE_ADDR && vars.contains(node.left->varName) && vars[node.left->varName])
            pointeeTypes[node.varName] = storageType(vars[node.left->varName]);
        llvm::Value* val = genExpr(*node.left);
        llvm::Type* ty = node.varType.empty()
            ? val->getType() : svType(node.varType);
        llvm::Value* alloca = createStorage(ty, node.varName);
        checkNarrowStore(val, *node.left, ty, unsignedTypeName(node.varType), node);
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
        if (!structDefs.contains(node.varType)) {
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
            checkNarrowStore(val, *node.left, ty, unsignedVars[node.varName], node);
            val = castTo(val, ty, isUnsignedExpr(*node.left));
            builder.CreateStore(val, alloca);
        }
    }
    else if (node.type == NODE_STRUCT_FIELD_ASSIGN && node.op.find('.') != std::string::npos) {
        Node field;
        field.type = NODE_STRUCT_FIELD;
        field.varName = node.varName;
        field.op = node.op;
        field.line = node.line; field.column = node.column; field.file = node.file;
        structFieldAddressOnly_ = true;
        llvm::Value* address = genExpr(field);
        structFieldAddressOnly_ = false;
        llvm::Type* fieldType = lastFieldType_;
        llvm::Value* value = genExpr(*node.left);
        checkNarrowStore(value, *node.left, fieldType, fieldIsUnsigned(node.varName, node.op), node);
        builder.CreateStore(castTo(value, fieldType, isUnsignedExpr(*node.left)), address);
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
        checkNarrowStore(value, *node.left, fieldType, fieldIsUnsigned(node.varName, node.op), node);
        value = castTo(value, fieldType, isUnsignedExpr(*node.left));
        llvm::Value* address = builder.CreateStructGEP(aggregateType, object->second, index);
        builder.CreateStore(value, address);
    }
    else if (node.type == NODE_ARRAY_ASSIGN) {
        if (node.left && node.right) {
            // динамический массив с описателем
            if (dynArrays.contains(node.varName)) {
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
                if (node.op == "compound") precomputed_[node.right->left->left.get()] = idx;
                llvm::Value* value = genExpr(*node.right);
                precomputed_.clear();
                checkNarrowStore(value, *node.right, elementType, unsignedElements[node.varName], node);
                builder.CreateStore(castTo(value, elementType, isUnsignedExpr(*node.right)), gep);
            }
            // raw dynamic array (ptr variable)
            else if (vars.contains(node.varName) &&
                storageType(vars[node.varName])->isPointerTy()) {
                llvm::Value* ptr = nullChecked(builder.CreateLoad(
                    builder.getPtrTy(), vars[node.varName]), node);
                llvm::Value* idx = genExpr(*node.left);
                auto pointee = pointeeTypes.find(node.varName);
                llvm::Type* elementType = pointee != pointeeTypes.end() ? pointee->second : builder.getInt32Ty();
                llvm::Value* gep = builder.CreateGEP(elementType, ptr, {idx});
                if (node.op == "compound") precomputed_[node.right->left->left.get()] = idx;
                llvm::Value* value = genExpr(*node.right);
                precomputed_.clear();
                builder.CreateStore(castTo(value, elementType, isUnsignedExpr(*node.right)), gep);
            } else if (arrays.contains(node.varName)) {
                llvm::Value* arr = arrays[node.varName];
                unsigned n = arrayLengths[node.varName];
                llvm::Type* elementType = arrayElementTypes[node.varName];
                if (!elementType) elementType = builder.getInt32Ty();
                llvm::ArrayType* at = llvm::ArrayType::get(elementType, n);
                llvm::Value* idx = genExpr(*node.left);
                emitArrayBoundsCheck(idx, builder.getInt64(n), node);
                llvm::Value* gep = builder.CreateInBoundsGEP(
                    at, arr, {builder.getInt32(0), idx});
                if (node.op == "compound") precomputed_[node.right->left->left.get()] = idx;
                llvm::Value* value = genExpr(*node.right);
                precomputed_.clear();
                checkNarrowStore(value, *node.right, elementType, unsignedElements[node.varName], node);
                builder.CreateStore(castTo(value, elementType, isUnsignedExpr(*node.right)), gep);
            }
        }
    }
    else if (node.type == NODE_DEREF_ASSIGN) {
        llvm::Value* alloca = vars[node.varName];
        if (alloca) {
            llvm::Value* ptr = nullChecked(builder.CreateLoad(builder.getPtrTy(), alloca), node);
            auto pointee = pointeeTypes.find(node.varName);
            llvm::Value* value = genExpr(*node.left);
            value = castTo(value, pointee != pointeeTypes.end() ? pointee->second : builder.getInt32Ty(),
                           isUnsignedExpr(*node.left));
            builder.CreateStore(value, ptr);
        }
    }
    else if (node.type == NODE_RETURN) {
        // Как в Go/Zig/Swift: сначала значение return, затем defer, затем выход.
        llvm::Function* cur = builder.GetInsertBlock()->getParent();
        llvm::Type* retTy = cur->getReturnType();
        if (retTy->isVoidTy()) {
            emitDeferred(0);
            builder.CreateRetVoid();
        } else if (node.left) {
            llvm::Value* v = genExpr(*node.left);
            // Имя функции в модуле — "sc.fn." + имя SC (generate()).
            const llvm::StringRef name = cur->getName();
            if (name.starts_with("sc.fn.")) {
                const auto returnType = funcReturnTypes.find(name.drop_front(6).str());
                if (returnType != funcReturnTypes.end())
                    checkNarrowStore(v, *node.left, retTy, unsignedTypeName(returnType->second), node);
            }
            v = castTo(v, retTy, isUnsignedExpr(*node.left));
            emitDeferred(0);
            builder.CreateRet(v);
        } else {
            emitDeferred(0);
        }
    }
    else if (node.type == NODE_BREAK) {
        if (breakBlock) { emitDeferred(loopDeferredBase_); builder.CreateBr(breakBlock); }
    }
    else if (node.type == NODE_CONTINUE) {
        if (continueBlock) { emitDeferred(loopDeferredBase_); builder.CreateBr(continueBlock); }
    }
    else if (node.type == NODE_ARRAY_DECL) {
        unsigned n = (unsigned)node.args.size();
        llvm::Type* elementType = svType(node.varType);
        llvm::ArrayType* at = llvm::ArrayType::get(elementType, n);
        llvm::Value* arr = createStorage(at, node.varName);
        arrays[node.varName] = arr;
        arrayLengths[node.varName] = n;
        arrayElementTypes[node.varName] = elementType;
        unsignedElements[node.varName] = sc::Type::from(node.varType).isUnsigned();
        for (unsigned i = 0; i < n; i++) {
            llvm::Value* gep = builder.CreateInBoundsGEP(
                at, arr, {builder.getInt32(0), builder.getInt32(i)});
            llvm::Value* val = genExpr(node.args[i]);
            checkNarrowStore(val, node.args[i], elementType, unsignedElements[node.varName], node.args[i]);
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
        growDynArray(node.varName, newCapacity, node);
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
        checkNarrowStore(value, *node.left, elementType, unsignedElements[node.varName], *node.left);
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

        llvm::Value* newSize = emitArraySize(*node.left, node);
        llvm::Value* oldLength = builder.CreateLoad(builder.getInt64Ty(),
            builder.CreateStructGEP(descType, desc, 1));

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
        growDynArray(node.varName, newSize, node);
        builder.CreateBr(noGrowBlock);

        // Новые элементы — нули, как у new T[n]; иначе после resize читался бы мусор.
        builder.SetInsertPoint(noGrowBlock);
        llvm::BasicBlock* zeroBlock = llvm::BasicBlock::Create(context, "resize_zero", function);
        llvm::BasicBlock* doneBlock = llvm::BasicBlock::Create(context, "resize_done", function);
        builder.CreateCondBr(builder.CreateICmpUGT(newSize, oldLength), zeroBlock, doneBlock);
        builder.SetInsertPoint(zeroBlock);
        llvm::Type* elementType = dynArrayElementTypes[node.varName];
        if (!elementType) elementType = builder.getInt32Ty();
        llvm::Value* data = builder.CreateLoad(builder.getPtrTy(), builder.CreateStructGEP(descType, desc, 0));
        llvm::Value* elementBytes = builder.getInt64(elementType->getPrimitiveSizeInBits() / 8);
        builder.CreateCall(runtimeFunction("memset"),
            {builder.CreateGEP(elementType, data, {oldLength}), builder.getInt32(0),
             builder.CreateMul(builder.CreateSub(newSize, oldLength), elementBytes)});
        builder.CreateBr(doneBlock);
        builder.SetInsertPoint(doneBlock);
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
            } else if (sc::Type::from(field.varType).isStruct()) {
                // Вложенная структура без значения: пустой литерал — поля получают
                // свои значения по умолчанию (раньше весь вложенный объект был нулями).
                sf.defaultVal.type = NODE_STRUCT_LITERAL;
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
        if (!structDefs.contains(node.varType)) {
            std::cerr << "Неизвестная структура: " << node.varType << std::endl;
            throw sc::CompileFailure{1};
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
        const ScopeSnapshot parentScope = saveScope();
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
            if (ty->isStructTy()) structTypesByVariable[pname] = ptype;
            unsignedVars[pname] = ptype == "u8" || ptype == "u16" ||
                ptype == "u32" || ptype == "u64" || ptype == "char";
            charVars[pname] = ptype == "char";
            constVars[pname] = false;
            integerWidths[pname] = ty->isIntegerTy() ? ty->getIntegerBitWidth() : 32;
        }
        genScope(node.children);
        const bool hasRet = blockHasTerminator(builder.GetInsertBlock());
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
        restoreScope(parentScope);
        funcs[node.varName] = func;
    }
    else if (node.type == NODE_FUNC_CALL) {
        if (funcs.contains(node.varName)) {
            std::vector<llvm::Value*> argVals;
            const auto& ptypes = funcParamTypes[node.varName];
            for (size_t i = 0; i < node.args.size(); i++) {
                llvm::Value* v = genExpr(node.args[i]);
                if (i < ptypes.size()) {
                    checkNarrowStore(v, node.args[i], svType(ptypes[i]), unsignedTypeName(ptypes[i]),
                                     node.args[i]);
                    v = castTo(v, svType(ptypes[i]), isUnsignedExpr(node.args[i]));
                }
                argVals.push_back(v);
            }
            builder.CreateCall(funcs[node.varName], argVals);
        } else {
            genExpr(node);
        }
    }
    else if (node.type == NODE_DEFER) {
        deferred_.push_back({node.left.get(), saveScope()});
    }
    else if (node.type == NODE_DROP) {
        // Семантика гарантирует: динамический массив или file, освобождается один раз.
        if (dynArrays.contains(node.varName)) {
            llvm::Value* desc = dynArrays[node.varName];
            llvm::Value* dataSlot = builder.CreateStructGEP(dynArrayType(), desc, 0);
            builder.CreateCall(runtimeFunction("free"),
                {builder.CreateLoad(builder.getPtrTy(), dataSlot)});
            builder.CreateStore(llvm::ConstantPointerNull::get(builder.getPtrTy()), dataSlot);
            builder.CreateStore(builder.getInt64(0), builder.CreateStructGEP(dynArrayType(), desc, 1));
            builder.CreateStore(builder.getInt64(0), builder.CreateStructGEP(dynArrayType(), desc, 2));
        } else if (llvm::Value* slot = vars[node.varName]) {
            llvm::Function* fcloseF = runtimeFunction("fclose");
            builder.CreateCall(fcloseF, {builder.CreateLoad(builder.getPtrTy(), slot)});
            builder.CreateStore(llvm::ConstantPointerNull::get(builder.getPtrTy()), slot);
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
        genScope(node.children);
        if (!blockHasTerminator(builder.GetInsertBlock()))
            builder.CreateBr(mergeBlock);
        restoreScope(parentScope);
        builder.SetInsertPoint(elseBlock);
        genScope(node.elseChildren);
        if ((builder.GetInsertBlock()->empty() || !builder.GetInsertBlock()->back().isTerminator()))
            builder.CreateBr(mergeBlock);
        restoreScope(parentScope);
        builder.SetInsertPoint(mergeBlock);
    }
    else if (node.type == NODE_LOOP_N) {
        // Счётчик i64: число повторов любой целой ширины (u8 … u64) сравнивается
        // в одном типе; раньше счётчик i32 против i64/i8 давал неверный IR.
        llvm::Value* count = genExpr(*node.left);
        if (count->getType() != builder.getInt64Ty())
            count = isUnsignedExpr(*node.left) ? builder.CreateZExt(count, builder.getInt64Ty())
                                               : builder.CreateSExt(count, builder.getInt64Ty());
        llvm::Function* func = builder.GetInsertBlock()->getParent();
        llvm::Value* i = entryAlloca(builder.getInt64Ty(), "i");
        builder.CreateStore(builder.getInt64(0), i);
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
        const size_t savedLoopBase = loopDeferredBase_;
        loopDeferredBase_ = deferred_.size();
        breakBlock = afterBlock;
        continueBlock = stepBlock;
        builder.CreateBr(loopBlock);
        builder.SetInsertPoint(loopBlock);
        llvm::Value* cur = builder.CreateLoad(builder.getInt64Ty(), i);
        llvm::Value* cond = isUnsignedExpr(*node.left) ? builder.CreateICmpULT(cur, count)
                                                       : builder.CreateICmpSLT(cur, count);
        builder.CreateCondBr(cond, bodyBlock, afterBlock);
        builder.SetInsertPoint(bodyBlock);
        const ScopeSnapshot parentScope = saveScope();
        genScope(node.children);
        if ((builder.GetInsertBlock()->empty() || !builder.GetInsertBlock()->back().isTerminator()))
            builder.CreateBr(stepBlock);
        restoreScope(parentScope);
        builder.SetInsertPoint(stepBlock);
        llvm::Value* next = builder.CreateAdd(cur, builder.getInt64(1));
        builder.CreateStore(next, i);
        builder.CreateBr(loopBlock);
        builder.SetInsertPoint(afterBlock);
        breakBlock = savedBreak;
        continueBlock = savedContinue;
        loopDeferredBase_ = savedLoopBase;
    }
    else if (node.type == NODE_FOR_EACH) {
        if (!arrays.contains(node.left->value) || !arrayLengths.contains(node.left->value))
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
        const size_t savedLoopBase = loopDeferredBase_;
        loopDeferredBase_ = deferred_.size();
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
        genScope(node.children);
        if ((builder.GetInsertBlock()->empty() || !builder.GetInsertBlock()->back().isTerminator())) builder.CreateBr(stepBlock);
        builder.SetInsertPoint(stepBlock);
        current = builder.CreateLoad(builder.getInt32Ty(), index);
        builder.CreateStore(builder.CreateAdd(current, builder.getInt32(1)), index);
        builder.CreateBr(conditionBlock);
        builder.SetInsertPoint(afterBlock);
        restoreScope(parentScope);
        breakBlock = savedBreak;
        continueBlock = savedContinue;
        loopDeferredBase_ = savedLoopBase;
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
        const size_t savedLoopBase = loopDeferredBase_;
        loopDeferredBase_ = deferred_.size();
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
        genScope(node.children);
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
        loopDeferredBase_ = savedLoopBase;
    }
    else if (node.type == NODE_LOOP) {
        llvm::Function* function = builder.GetInsertBlock()->getParent();
        llvm::BasicBlock* headerBlock = llvm::BasicBlock::Create(context, "loop_header", function);
        llvm::BasicBlock* bodyBlock = llvm::BasicBlock::Create(context, "loop_body", function);
        llvm::BasicBlock* afterBlock = llvm::BasicBlock::Create(context, "loop_after", function);
        const auto savedBreak = breakBlock;
        const auto savedContinue = continueBlock;
        const size_t savedLoopBase = loopDeferredBase_;
        loopDeferredBase_ = deferred_.size();
        breakBlock = afterBlock;
        continueBlock = headerBlock;
        const ScopeSnapshot parentScope = saveScope();
        builder.CreateBr(headerBlock);
        builder.SetInsertPoint(headerBlock);
        builder.CreateBr(bodyBlock);
        builder.SetInsertPoint(bodyBlock);
        genScope(node.children);
        if ((builder.GetInsertBlock()->empty() || !builder.GetInsertBlock()->back().isTerminator())) builder.CreateBr(headerBlock);
        restoreScope(parentScope);
        builder.SetInsertPoint(afterBlock);
        breakBlock = savedBreak;
        continueBlock = savedContinue;
        loopDeferredBase_ = savedLoopBase;
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
        const size_t savedLoopBase = loopDeferredBase_;
        loopDeferredBase_ = deferred_.size();
        breakBlock = afterBlock;
        continueBlock = condBlock;
        builder.CreateBr(condBlock);
        builder.SetInsertPoint(condBlock);
        llvm::Value* cond = toBool(genExpr(*node.left));
        builder.CreateCondBr(cond, bodyBlock, afterBlock);
        builder.SetInsertPoint(bodyBlock);
        const ScopeSnapshot parentScope = saveScope();
        genScope(node.children);
        if ((builder.GetInsertBlock()->empty() || !builder.GetInsertBlock()->back().isTerminator()))
            builder.CreateBr(condBlock);
        restoreScope(parentScope);
        builder.SetInsertPoint(afterBlock);
        breakBlock = savedBreak;
        continueBlock = savedContinue;
        loopDeferredBase_ = savedLoopBase;
    }
}

void CodeGen::generate(const Node& program) {
    putsFunc = runtimeFunction("puts");
    abortFunc = runtimeFunction("abort");
    printfFunc = runtimeFunction("printf");

    // Aggregate parameter types must exist before function signatures are built.
    for (const Node& node : program.children)
        if (node.type == NODE_STRUCT_DEF) genNode(node);

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
        const std::string returnType = node.varType.empty() ? "i32" : node.varType;
        funcParamTypes[node.varName] = parameterNames;
        funcReturnTypes[node.varName] = returnType;
        llvm::FunctionType* functionType = llvm::FunctionType::get(
            svType(returnType), parameterTypes, false);
        // Префикс "sc.fn.": пользовательские функции не конфликтуют с main и рантаймом C.
        funcs[node.varName] = llvm::Function::Create(
            functionType, llvm::Function::InternalLinkage, "sc.fn." + node.varName, module);
    }

    // Сначала main создаёт глобальные переменные, которые затем видят тела функций.
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
        if (node.type != NODE_FUNC_DEF && node.type != NODE_STRUCT_DEF) genNode(node);
    emitDeferred(0);
    deferred_.clear();
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
