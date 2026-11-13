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
    {"round", 'd', "d", false},      {"strdup", 'p', "p", false},
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
        if (llvm::Value* slot = deferred_[i - 1].freeSlot; slot && deferred_[i - 1].elements) {
            dropElements(slot, deferred_[i - 1].structName);  // массив структур: строки элементов, затем буфер
        } else if (slot && !deferred_[i - 1].structName.empty()) {
            dropStruct(slot, deferred_[i - 1].structName);
            continue;
        }
        if (llvm::Value* slot = deferred_[i - 1].freeSlot) {  // free(NULL) — ничего не делает
            builder.CreateCall(runtimeFunction("free"), {builder.CreateLoad(builder.getPtrTy(), slot)});
            builder.CreateStore(llvm::ConstantPointerNull::get(builder.getPtrTy()), slot);
            continue;
        }
        restoreScope(deferred_[i - 1].bindings);
        genStatement(*deferred_[i - 1].statement);
    }
    restoreScope(current);
}

bool CodeGen::isOwnedString(const std::string& name) const {
    const auto found = vars.find(name);
    return found != vars.end() && ownedStringSlots_.contains(found->second);
}

// Новая строка в куче, которой ещё никто не владеет: результат str(), replace,
// ввода, read() и функции, возвращающей string.
bool CodeGen::isFreshString(const Node& expr) const {
    if (expr.type == NODE_FILE_READ) return true;
    if (expr.type == NODE_BINOP) return expr.varType == "string-concat";  // a + b (D-024)
    if (expr.type == NODE_INPUT) return sc::Type::from(expr.varType).isStr();
    if (expr.type != NODE_FUNC_CALL) return false;
    if (expr.varName == "move")  // move(структура) — не строка (freshStruct), move(массив) — isFreshArray
        return expr.args.size() == 1 && !structTypesByVariable.contains(expr.args[0].value) &&
               !dynArrays.contains(expr.args[0].value);
    if (expr.varName == "str" || expr.varName == "replace") return true;
    const auto result = funcReturnTypes.find(expr.varName);
    return funcs.contains(expr.varName) && result != funcReturnTypes.end() &&
           sc::Type::from(result->second).isStr();
}

// Копия строки в куче (как std::string(s)); NULL (поле string без значения) — "".
llvm::Value* CodeGen::copyString(llvm::Value* value) {
    llvm::Value* isNull = builder.CreateICmpEQ(value, llvm::ConstantPointerNull::get(builder.getPtrTy()));
    llvm::Value* source = builder.CreateSelect(isNull, getStringPtr(""), value);
    llvm::Value* copy = builder.CreateCall(runtimeFunction("strdup"), {source});
    llvm::Function* abortF = runtimeFunction("abort");
    llvm::Function* function = builder.GetInsertBlock()->getParent();
    llvm::BasicBlock* failed = llvm::BasicBlock::Create(context, "strdup_failed", function);
    llvm::BasicBlock* done = llvm::BasicBlock::Create(context, "strdup_done", function);
    builder.CreateCondBr(builder.CreateICmpEQ(copy, llvm::ConstantPointerNull::get(builder.getPtrTy())),
                         failed, done);
    builder.SetInsertPoint(failed);
    builder.CreateCall(abortF, {});
    builder.CreateUnreachable();
    builder.SetInsertPoint(done);
    return copy;
}

// Значение для string-переменной: свежую строку забрать, остальное (литерал,
// другая переменная, поле) скопировать — `string b = a` копирует (D-014).
llvm::Value* CodeGen::ownString(const Node& expr, llvm::Value* value) {
    if (!isFreshString(expr)) return copyString(value);
    consumeTemporary(expr);
    return value;
}

// Запись строки в поле, элемент массива: такие места строку не освобождают, поэтому
// чужую string-переменную (её освободит конец блока) копировать, иначе — висячий адрес.
llvm::Value* CodeGen::stableString(const Node& expr, llvm::Value* value) {
    const bool borrowed = (expr.type == NODE_IDENTIFIER && isOwnedString(expr.value)) ||
        (expr.type == NODE_TERNARY && value->getType()->isPointerTy());
    if (isFreshString(expr)) consumeTemporary(expr);
    return borrowed ? copyString(value) : value;
}

void CodeGen::ownStringSlot(llvm::Value* slot) {
    ownedStringSlots_.insert(slot);
    deferred_.push_back({nullptr, {}, slot});
}

// Есть ли в структуре (или во вложенной) поле string — тогда она владеет памятью.
bool CodeGen::structOwnsMemory(const std::string& structName, int depth) const {
    auto def = structDefs.find(structName);
    if (def == structDefs.end() || depth > 256) return false;
    for (const StructField& field : def->second) {
        const sc::Type type = sc::Type::from(field.type);
        if (type.isStr() || type.isDynArray() ||
            (type.isStruct() && structOwnsMemory(type.structName, depth + 1))) return true;
    }
    return false;
}

// Освободить строки полей структуры по адресу (и во вложенных); поле обнуляется,
// поэтому повторное освобождение (выход из блока после return) ничего не делает.
void CodeGen::dropStruct(llvm::Value* address, const std::string& structName) {
    llvm::StructType* type = llvmStructTypes[structName];
    const std::vector<StructField>& fields = structDefs[structName];
    for (unsigned i = 0; i < fields.size(); ++i) {
        const sc::Type fieldType = sc::Type::from(fields[i].type);
        llvm::Value* field = builder.CreateStructGEP(type, address, i);
        if (fieldType.isStr()) {
            builder.CreateCall(runtimeFunction("free"), {builder.CreateLoad(builder.getPtrTy(), field)});
            builder.CreateStore(llvm::ConstantPointerNull::get(builder.getPtrTy()), field);
        } else if (fieldType.isDynArray()) {  // поле-массив: буфер и пустой описатель (D-016)
            builder.CreateCall(runtimeFunction("free"), {builder.CreateLoad(builder.getPtrTy(),
                builder.CreateStructGEP(dynArrayType(), field, 0))});
            builder.CreateStore(llvm::Constant::getNullValue(dynArrayType()), field);
        } else if (fieldType.isStruct() && structOwnsMemory(fieldType.structName)) {
            dropStruct(field, fieldType.structName);
        }
    }
}

// Структура по адресу — побитовая копия чужой: дать ей свои копии строк (как в C++).
void CodeGen::copyStructStrings(llvm::Value* address, const std::string& structName) {
    llvm::StructType* type = llvmStructTypes[structName];
    const std::vector<StructField>& fields = structDefs[structName];
    for (unsigned i = 0; i < fields.size(); ++i) {
        const sc::Type fieldType = sc::Type::from(fields[i].type);
        llvm::Value* field = builder.CreateStructGEP(type, address, i);
        if (fieldType.isStr())
            builder.CreateStore(copyString(builder.CreateLoad(builder.getPtrTy(), field)), field);
        else if (fieldType.isDynArray()) {  // своя копия буфера: ёмкость (минимум 1) и len элементов
            llvm::Type* element = svType(fieldType.element->toString());
            llvm::Value* size = elementSize(element);
            llvm::Value* capacity = builder.CreateLoad(builder.getInt64Ty(), builder.CreateStructGEP(dynArrayType(), field, 2));
            capacity = builder.CreateSelect(builder.CreateICmpEQ(capacity, builder.getInt64(0)), builder.getInt64(1), capacity);
            llvm::Value* data = builder.CreateCall(runtimeFunction("malloc"), {builder.CreateMul(capacity, size)});
            emitRuntimeFailureIf(builder.CreateIsNull(data), "out of memory", Node{});
            llvm::Value* dataSlot = builder.CreateStructGEP(dynArrayType(), field, 0);
            llvm::Value* length = builder.CreateLoad(builder.getInt64Ty(), builder.CreateStructGEP(dynArrayType(), field, 1));
            builder.CreateCall(runtimeFunction("memcpy"), {data, builder.CreateLoad(builder.getPtrTy(), dataSlot),
                builder.CreateMul(length, size)});
            builder.CreateStore(data, dataSlot);
        }
        else if (fieldType.isStruct() && structOwnsMemory(fieldType.structName))
            copyStructStrings(field, fieldType.structName);
    }
}

// Новый растущий массив, которым никто не владеет: результат `fn int[] f()` или move(имя).
bool CodeGen::isFreshArray(const Node& expr) const {
    if (expr.type != NODE_FUNC_CALL) return false;
    if (expr.varName == "move")
        return expr.args.size() == 1 && expr.args[0].type == NODE_IDENTIFIER && dynArrays.contains(expr.args[0].value);
    const auto result = funcReturnTypes.find(expr.varName);
    return funcs.contains(expr.varName) && result != funcReturnTypes.end() && result->second.starts_with("dyn<");
}

// Описатель {data, len, cap} со своей копией буфера (ёмкость минимум 1, как у copyStructStrings).
llvm::Value* CodeGen::copyDynArray(llvm::Value* data, llvm::Value* length, llvm::Value* capacity,
                                   llvm::Type* element, const std::string& structName) {
    llvm::Value* size = elementSize(element);
    capacity = builder.CreateSelect(builder.CreateICmpEQ(capacity, builder.getInt64(0)), builder.getInt64(1), capacity);
    llvm::Value* copy = builder.CreateCall(runtimeFunction("malloc"), {builder.CreateMul(capacity, size)});
    emitRuntimeFailureIf(builder.CreateIsNull(copy), "out of memory", Node{});
    builder.CreateCall(runtimeFunction("memcpy"), {copy, data, builder.CreateMul(length, size)});
    if (!structName.empty())  // элементы со строками/массивами: свои копии (как копия структуры, D-016)
        forEachElement(copy, element, builder.getInt64(0), length,
                       [&](llvm::Value* item) { copyStructStrings(item, structName); });
    llvm::Value* desc = llvm::UndefValue::get(dynArrayType());
    desc = builder.CreateInsertValue(desc, copy, {0});
    desc = builder.CreateInsertValue(desc, length, {1});
    return builder.CreateInsertValue(desc, capacity, {2});
}

// Значение для параметра/переменной/return типа T[] (D-018): f() и move(a) забрать,
// имя растущего или обычного массива — скопировать (как vector в C++).
llvm::Value* CodeGen::arrayValue(const Node& expr) {
    if (isFreshArray(expr)) {
        llvm::Value* value = genExpr(expr);
        consumeTemporary(expr);
        return value;
    }
    if (auto found = dynArrays.find(expr.value); found != dynArrays.end()) {
        llvm::Value* desc = found->second;
        auto field = [&](unsigned index, llvm::Type* type) {
            return builder.CreateLoad(type, builder.CreateStructGEP(dynArrayType(), desc, index));
        };
        return copyDynArray(field(0, builder.getPtrTy()), field(1, builder.getInt64Ty()),
                            field(2, builder.getInt64Ty()), dynArrayElementTypes[expr.value],
                            arrayStructNames.contains(expr.value) ? arrayStruct(arrayStructNames[expr.value]) : "");
    }
    llvm::Value* length = builder.getInt64(arrayLengths[expr.value]);
    return copyDynArray(arrays[expr.value], length, length, arrayElementTypes[expr.value],
                        arrayStructNames.contains(expr.value) ? arrayStruct(arrayStructNames[expr.value]) : "");
}

// Окно для параметра `T[..]` (D-018): {data + начало, длина, длина} без копии; v[a..b] —
// конец не входит, slice(v, a, n) — начало и количество; выход за края — ошибка выполнения.
llvm::Value* CodeGen::viewValue(const Node& expr) {
    const bool slice = expr.type == NODE_FUNC_CALL && (expr.varName == ".slice" || expr.varName == "slice");
    const std::string& name = slice ? expr.args[0].value : expr.value;
    llvm::Value* data = nullptr;
    llvm::Value* length = nullptr;
    llvm::Type* element = nullptr;
    if (auto found = dynArrays.find(name); found != dynArrays.end()) {
        data = builder.CreateLoad(builder.getPtrTy(), builder.CreateStructGEP(dynArrayType(), found->second, 0));
        length = builder.CreateLoad(builder.getInt64Ty(), builder.CreateStructGEP(dynArrayType(), found->second, 1));
        element = dynArrayElementTypes[name];
    } else {
        data = arrays[name];
        length = builder.getInt64(arrayLengths[name]);
        element = arrayElementTypes[name];
    }
    llvm::Value* start = builder.getInt64(0);
    llvm::Value* end = length;
    if (slice) {
        start = castTo(genExpr(expr.args[1]), builder.getInt64Ty(), isUnsignedExpr(expr.args[1]));
        end = castTo(genExpr(expr.args[2]), builder.getInt64Ty(), isUnsignedExpr(expr.args[2]));
        if (expr.varName == "slice") end = builder.CreateAdd(start, end);
        llvm::Value* outside = builder.CreateOr(builder.CreateICmpSLT(start, builder.getInt64(0)),
            builder.CreateOr(builder.CreateICmpSGT(start, end), builder.CreateICmpSGT(end, length)));
        emitRuntimeFailureIf(outside, "slice out of bounds", expr);
        data = builder.CreateGEP(element, data, {start});
    }
    llvm::Value* count = builder.CreateSub(end, start);
    llvm::Value* desc = llvm::UndefValue::get(dynArrayType());
    desc = builder.CreateInsertValue(desc, data, {0});
    desc = builder.CreateInsertValue(desc, count, {1});
    return builder.CreateInsertValue(desc, count, {2});
}

// Имя растущего массива: описатель desc, освобождается в конце блока (1.3); окно — нет.
void CodeGen::bindDynArray(const std::string& name, llvm::Value* desc, const std::string& elementType,
                           bool owned) {
    dynArrays[name] = desc;
    dynArrayElementTypes[name] = svType(elementType);
    unsignedElements[name] = sc::Type::from(elementType).isUnsigned();
    vars[name] = desc;
    if (structDefs.contains(elementType)) arrayStructNames[name] = elementType;
    if (!owned) return;
    deferred_.push_back({nullptr, {}, desc, nullptr, arrayStruct(elementType)});
    deferred_.back().elements = !deferred_.back().structName.empty();
}

// Имя структуры элемента, если её надо освобождать/копировать по полям; иначе пусто.
std::string CodeGen::arrayStruct(const std::string& elementType) const {
    return structDefs.contains(elementType) && structOwnsMemory(elementType) ? elementType : "";
}

llvm::Value* CodeGen::elementSize(llvm::Type* element) {  // bool — 1 байт, структура — с выравниванием
    return builder.getInt64(module.getDataLayout().getTypeAllocSize(element).getFixedValue());
}

// Цикл по элементам [from, to) буфера data: body получает адрес элемента.
void CodeGen::forEachElement(llvm::Value* data, llvm::Type* element, llvm::Value* from, llvm::Value* to,
                             const std::function<void(llvm::Value*)>& body) {
    llvm::Function* function = builder.GetInsertBlock()->getParent();
    llvm::BasicBlock* before = builder.GetInsertBlock();
    llvm::BasicBlock* head = llvm::BasicBlock::Create(context, "each_head", function);
    llvm::BasicBlock* step = llvm::BasicBlock::Create(context, "each_body", function);
    llvm::BasicBlock* done = llvm::BasicBlock::Create(context, "each_done", function);
    builder.CreateBr(head);
    builder.SetInsertPoint(head);
    llvm::PHINode* index = builder.CreatePHI(builder.getInt64Ty(), 2);
    index->addIncoming(from, before);
    builder.CreateCondBr(builder.CreateICmpSLT(index, to), step, done);
    builder.SetInsertPoint(step);
    body(builder.CreateGEP(element, data, {index}));
    index->addIncoming(builder.CreateAdd(index, builder.getInt64(1)), builder.GetInsertBlock());
    builder.CreateBr(head);
    builder.SetInsertPoint(done);
}

// Строки/массивы полей элементов [from, len) массива структур по описателю desc.
void CodeGen::dropElements(llvm::Value* desc, const std::string& structName, llvm::Value* from) {
    if (structName.empty()) return;
    llvm::Value* data = builder.CreateLoad(builder.getPtrTy(), builder.CreateStructGEP(dynArrayType(), desc, 0));
    llvm::Value* length = builder.CreateLoad(builder.getInt64Ty(), builder.CreateStructGEP(dynArrayType(), desc, 1));
    forEachElement(data, llvmStructTypes[structName], from ? from : builder.getInt64(0), length,
                   [&](llvm::Value* item) { dropStruct(item, structName); });
}

// Переменная-структура со строками освобождает их в конце своего блока (как string, D-014).
void CodeGen::ownStructSlot(llvm::Value* slot, const std::string& structName) {
    if (structOwnsMemory(structName)) deferred_.push_back({nullptr, {}, slot, nullptr, structName});
}

// p.name = v / p.inner = q: строка поля — своя копия, старая освобождается (D-016).
bool CodeGen::storeOwnedField(const Node& assign, llvm::Value* address, llvm::Type* fieldType) {
    std::string fieldTypeName;
    auto root = structTypesByVariable.find(assign.varName);
    if (assign.right) root = arrayStructNames.find(assign.varName);  // a[i].name = "x"
    if (root != structTypesByVariable.end() && root != arrayStructNames.end()) {
        fieldTypeName = root->second;
        for (size_t pos = 0; pos <= assign.op.size();) {
            const size_t dot = std::min(assign.op.find('.', pos), assign.op.size());
            const std::string name = assign.op.substr(pos, dot - pos);
            std::string next;
            for (const StructField& field : structDefs[fieldTypeName]) if (field.name == name) next = field.type;
            fieldTypeName = next;
            pos = dot + 1;
        }
    }
    const sc::Type type = sc::Type::from(fieldTypeName);
    if (type.isStr()) {
        llvm::Value* value = ownString(*assign.left, genExpr(*assign.left));
        llvm::Value* old = builder.CreateLoad(builder.getPtrTy(), address);
        builder.CreateStore(value, address);
        builder.CreateCall(runtimeFunction("free"), {old});
        return true;
    }
    if (type.isStruct() && fieldType->isStructTy() && structOwnsMemory(type.structName)) {
        replaceStruct(address, type.structName, genExpr(*assign.left), *assign.left);
        return true;
    }
    return false;
}

// Новая структура со строками, которой ещё никто не владеет: результат функции или
// move(имя). Возвращает имя структуры, пусто — не такая.
std::string CodeGen::freshStruct(const Node& expr) const {
    if (expr.type != NODE_FUNC_CALL) return "";
    std::string structName;
    if (expr.varName == "move" && expr.args.size() == 1) {
        auto variable = structTypesByVariable.find(expr.args[0].value);
        if (expr.args[0].type == NODE_IDENTIFIER && variable != structTypesByVariable.end())
            structName = variable->second;
    } else if (auto result = funcReturnTypes.find(expr.varName);
               funcs.contains(expr.varName) && result != funcReturnTypes.end()) {
        structName = result->second;
    }
    return !structName.empty() && structOwnsMemory(structName) ? structName : "";
}

// Значение-структура для нового места: свежую забрать (без копии), чужую — скопировать (D-016).
llvm::Value* CodeGen::takeStruct(const Node& expr, llvm::Value* value, const std::string& structName) {
    if (!structOwnsMemory(structName)) return value;
    if (!freshStruct(expr).empty()) {
        consumeTemporary(expr);
        return value;
    }
    llvm::AllocaInst* copy = entryAlloca(value->getType(), "sc.struct.copy");
    builder.CreateStore(value, copy);
    copyStructStrings(copy, structName);
    return builder.CreateLoad(value->getType(), copy);
}

// x = y / x.inner = y для структуры со строками: сначала копия нового значения, затем
// освобождение старого (x = x тоже верно).
void CodeGen::replaceStruct(llvm::Value* address, const std::string& structName, llvm::Value* value,
                            const Node& source) {
    value = takeStruct(source, value, structName);
    dropStruct(address, structName);
    builder.CreateStore(value, address);
}

// Временная строка (результат str()/replace/ввода/функции, не положенный в
// переменную или поле) живёт до конца оператора: слот в начале функции (NULL), на
// выходах из блока (return/break/continue) освобождается вместе с переменными.
void CodeGen::registerTemporary(const Node& expr, llvm::Value* value) {
    llvm::BasicBlock& entry = builder.GetInsertBlock()->getParent()->getEntryBlock();
    llvm::IRBuilder<> at(&entry, entry.getFirstInsertionPt());
    llvm::AllocaInst* slot = at.CreateAlloca(value->getType(), nullptr, "sc.tmp");
    at.CreateStore(llvm::Constant::getNullValue(value->getType()), slot);  // путь без значения: free(NULL)
    builder.CreateStore(value, slot);
    deferred_.push_back({nullptr, {}, slot, &expr, freshStruct(expr)});
    if (isFreshArray(expr)) {  // f() -> P[] / move(a): элементы-структуры освобождаются с массивом
        const std::string type = expr.varName == "move" ? "" : funcReturnTypes[expr.varName];
        deferred_.back().structName = expr.varName == "move"
            ? (arrayStructNames.contains(expr.args[0].value) ? arrayStructNames[expr.args[0].value] : "")
            : arrayStruct(sc::Type::from(type).element->toString());
        deferred_.back().elements = !deferred_.back().structName.empty();
    }
}

// Значение забрала переменная, поле или return: временной строкой оно больше не считается.
void CodeGen::consumeTemporary(const Node& expr) {
    for (size_t i = deferred_.size(); i > 0; --i)
        if (deferred_[i - 1].temporary == &expr) {
            deferred_.erase(deferred_.begin() + static_cast<std::ptrdiff_t>(i - 1));
            return;
        }
}

// Конец оператора: освободить его временные строки (после return/break их уже освободил выход).
void CodeGen::releaseTemporaries(size_t mark) {
    const bool reachable = !blockHasTerminator(builder.GetInsertBlock());
    for (size_t i = deferred_.size(); i > mark; --i) {
        if (!deferred_[i - 1].temporary) continue;
        if (reachable && deferred_[i - 1].elements)
            dropElements(deferred_[i - 1].freeSlot, deferred_[i - 1].structName);
        if (reachable && !deferred_[i - 1].structName.empty() && !deferred_[i - 1].elements) {
            dropStruct(deferred_[i - 1].freeSlot, deferred_[i - 1].structName);
        } else if (reachable) {
            llvm::Value* slot = deferred_[i - 1].freeSlot;
            builder.CreateCall(runtimeFunction("free"), {builder.CreateLoad(builder.getPtrTy(), slot)});
            builder.CreateStore(llvm::ConstantPointerNull::get(builder.getPtrTy()), slot);
        }
        deferred_.erase(deferred_.begin() + static_cast<std::ptrdiff_t>(i - 1));
    }
}

void CodeGen::genStatement(const Node& statement) {
    const size_t mark = deferred_.size();
    genNode(statement);
    releaseTemporaries(mark);
}

void CodeGen::genScope(const std::vector<Node>& statements) {
    const size_t base = deferred_.size();
    for (const Node& statement : statements) {
        if (blockHasTerminator(builder.GetInsertBlock())) break;
        genStatement(statement);
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
    case TypeKind::Char:    return llvm::Type::getInt32Ty(ctx);  // буква Юникода (D-021)
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
    if (name.starts_with("dyn<") || name.starts_with("view<")) return dynArrayType();  // int[], int[..] (D-018)
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
// sc.utf8_encode(letter, out): буква в UTF-8 (1–4 байта) и завершающий 0 в out[5].
llvm::Function* CodeGen::utf8EncodeFunction() {
    if (llvm::Function* existing = module.getFunction("sc.utf8_encode")) return existing;
    llvm::Type* i32 = builder.getInt32Ty();
    llvm::Type* i8 = builder.getInt8Ty();
    llvm::Function* function = llvm::Function::Create(
        llvm::FunctionType::get(builder.getInt64Ty(), {i32, builder.getPtrTy()}, false),
        llvm::Function::InternalLinkage, "sc.utf8_encode", module);
    llvm::IRBuilderBase::InsertPointGuard guard(builder);
    builder.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", function));
    llvm::Value* c = function->getArg(0);
    llvm::Value* out = function->getArg(1);
    auto below = [&](uint32_t limit) { return builder.CreateICmpULT(c, builder.getInt32(limit)); };
    auto pick = [&](uint32_t one, uint32_t two, uint32_t three, uint32_t four) {
        return builder.CreateSelect(below(0x80), builder.getInt32(one), builder.CreateSelect(below(0x800),
            builder.getInt32(two), builder.CreateSelect(below(0x10000), builder.getInt32(three), builder.getInt32(four))));
    };
    llvm::Value* length = pick(1, 2, 3, 4);
    llvm::Value* lead = builder.CreateOr(pick(0, 0xC0, 0xE0, 0xF0),
                                         builder.CreateLShr(c, builder.CreateMul(builder.CreateSub(length, builder.getInt32(1)), builder.getInt32(6))));
    builder.CreateStore(builder.CreateTrunc(lead, i8), out);
    for (unsigned k = 1; k < 4; ++k) {  // байт k: 10xxxxxx из бит c >> 6*(length-1-k)
        llvm::Value* shift = builder.CreateMul(builder.CreateSub(length, builder.getInt32(k + 1)), builder.getInt32(6));
        llvm::Value* safeShift = builder.CreateSelect(builder.CreateICmpSGE(shift, builder.getInt32(0)), shift, builder.getInt32(0));
        llvm::Value* bits = builder.CreateOr(builder.getInt32(0x80),
            builder.CreateAnd(builder.CreateLShr(c, safeShift), builder.getInt32(0x3F)));
        builder.CreateStore(builder.CreateTrunc(bits, i8), builder.CreateInBoundsGEP(i8, out, builder.getInt64(k)));
    }
    llvm::Value* length64 = builder.CreateZExt(length, builder.getInt64Ty());
    builder.CreateStore(builder.getInt8(0), builder.CreateInBoundsGEP(i8, out, length64));
    builder.CreateRet(length64);
    return function;
}

// sc.utf8_decode(at, width): буква UTF-8 с адреса at и её длина в байтах. Не UTF-8
// (обрыв, лишний байт, длинная запись, суррогат) — U+FFFD на один байт, как
// String::from_utf8_lossy в Rust (D-022). Дальше завершающего 0 не читает.
llvm::Function* CodeGen::utf8DecodeFunction() {
    if (llvm::Function* existing = module.getFunction("sc.utf8_decode")) return existing;
    llvm::Type* i32 = builder.getInt32Ty();
    llvm::Type* i8 = builder.getInt8Ty();
    llvm::Function* function = llvm::Function::Create(
        llvm::FunctionType::get(i32, {builder.getPtrTy(), builder.getPtrTy()}, false),
        llvm::Function::InternalLinkage, "sc.utf8_decode", module);
    llvm::IRBuilderBase::InsertPointGuard guard(builder);
    auto block = [&](const char* name) { return llvm::BasicBlock::Create(context, name, function); };
    llvm::Value* at = function->getArg(0);
    llvm::Value* width = function->getArg(1);
    builder.SetInsertPoint(block("entry"));
    llvm::AllocaInst* code = builder.CreateAlloca(i32);
    llvm::Value* lead = builder.CreateZExt(builder.CreateLoad(i8, at), i32);
    auto test = [&](unsigned shift, unsigned value) {
        return builder.CreateICmpEQ(builder.CreateLShr(lead, builder.getInt32(shift)), builder.getInt32(value));
    };
    // Сколько байт продолжения: 0 — ASCII, 1–3, 4 — не UTF-8.
    llvm::Value* extra = builder.CreateSelect(builder.CreateICmpULT(lead, builder.getInt32(0x80)), builder.getInt32(0),
        builder.CreateSelect(test(5, 0x6), builder.getInt32(1), builder.CreateSelect(test(4, 0xE), builder.getInt32(2),
        builder.CreateSelect(test(3, 0x1E), builder.getInt32(3), builder.getInt32(4)))));
    llvm::Value* mask = builder.CreateLShr(builder.getInt32(0x7F), extra);
    builder.CreateStore(builder.CreateAnd(lead, mask), code);
    llvm::BasicBlock* bad = block("bad");
    llvm::BasicBlock* check = block("check");
    llvm::BasicBlock* next = block("cont1");
    builder.CreateCondBr(builder.CreateICmpEQ(extra, builder.getInt32(4)), bad, next);
    for (unsigned k = 1; k <= 3; ++k) {
        builder.SetInsertPoint(next);
        llvm::BasicBlock* read = block("read");
        builder.CreateCondBr(builder.CreateICmpULT(extra, builder.getInt32(k)), check, read);
        builder.SetInsertPoint(read);
        llvm::Value* byte = builder.CreateZExt(builder.CreateLoad(i8, builder.CreateInBoundsGEP(i8, at, builder.getInt64(k))), i32);
        llvm::BasicBlock* take = block("take");
        builder.CreateCondBr(builder.CreateICmpEQ(builder.CreateAnd(byte, builder.getInt32(0xC0)), builder.getInt32(0x80)),
                             take, bad);
        builder.SetInsertPoint(take);
        builder.CreateStore(builder.CreateOr(builder.CreateShl(builder.CreateLoad(i32, code), builder.getInt32(6)),
                                             builder.CreateAnd(byte, builder.getInt32(0x3F))), code);
        next = k < 3 ? block("cont") : check;
        builder.CreateBr(next);
    }
    builder.SetInsertPoint(check);
    llvm::Value* c = builder.CreateLoad(i32, code);
    llvm::Value* least = builder.CreateSelect(builder.CreateICmpEQ(extra, builder.getInt32(1)), builder.getInt32(0x80),
        builder.CreateSelect(builder.CreateICmpEQ(extra, builder.getInt32(2)), builder.getInt32(0x800),
        builder.CreateSelect(builder.CreateICmpEQ(extra, builder.getInt32(3)), builder.getInt32(0x10000), builder.getInt32(0))));
    llvm::Value* invalid = builder.CreateOr(builder.CreateOr(builder.CreateICmpULT(c, least),
        builder.CreateICmpUGT(c, builder.getInt32(0x10FFFF))),
        builder.CreateICmpULT(builder.CreateSub(c, builder.getInt32(0xD800)), builder.getInt32(0x800)));
    llvm::BasicBlock* good = block("good");
    builder.CreateCondBr(invalid, bad, good);
    builder.SetInsertPoint(good);
    builder.CreateStore(builder.CreateZExt(builder.CreateAdd(extra, builder.getInt32(1)), builder.getInt64Ty()), width);
    builder.CreateRet(c);
    builder.SetInsertPoint(bad);
    builder.CreateStore(builder.getInt64(1), width);
    builder.CreateRet(builder.getInt32(0xFFFD));
    return function;
}

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
        newCapacity, elementSize(elementType));
    emitRuntimeFailureIf(builder.CreateExtractValue(product, 1), "out of memory", location);
    llvm::Value* bytes = builder.CreateExtractValue(product, 0);
    llvm::Value* newData = builder.CreateCall(mallocF, {bytes});
    emitRuntimeFailureIf(builder.CreateIsNull(newData), "out of memory", location);
    llvm::Value* oldLength = builder.CreateLoad(builder.getInt64Ty(),
        builder.CreateStructGEP(descType, desc, 1));
    // Copy existing elements into the new buffer.
    llvm::Value* copyBytes = builder.CreateMul(oldLength,
        elementSize(elementType));
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
    if (structName == structTypesByVariable.end()) {
        structName = arrayStructNames.find(variable);  // a[i].x
        if (structName == arrayStructNames.end()) return false;
    }
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
        if (node.varType == "string") return true;  // байт строки — u8
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
        if (sc::Type::from(field.type).isDynArray()) {  // int[] a; — пустой массив (D-016)
            builder.CreateStore(llvm::Constant::getNullValue(fieldType), address);
        } else if (initializer->type == NODE_STRUCT_LITERAL) {
            // Nested struct field: allocate a temp, initialize it, store the pointer
            llvm::AllocaInst* temp = builder.CreateAlloca(fieldType, nullptr, field.name);
            llvm::StructType* nestedType = llvmStructTypes[field.type];
            initStructLiteral(temp, nestedType, field.type, *initializer);
            llvm::Value* loaded = builder.CreateLoad(fieldType, temp);
            builder.CreateStore(loaded, address);
        } else if (sc::Type::from(field.type).isStr()) {  // поле владеет своей копией (D-016)
            builder.CreateStore(ownString(*initializer, genExpr(*initializer)), address);
        } else {
            llvm::Value* value = stableString(*initializer, genExpr(*initializer));
            checkNarrowStore(value, *initializer, fieldType, unsignedTypeName(field.type), *initializer);
            value = castTo(value, fieldType, isUnsignedExpr(*initializer));
            if (fieldType->isStructTy()) value = takeStruct(*initializer, value, field.type);  // { inner = q }
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
        llvm::Value* result = genBinary(node, left);
        if (isFreshString(node)) registerTemporary(node, result);  // a + b — временная строка (D-024)
        return result;
    }
    llvm::Value* value = genOtherExpr(node);
    if (isFreshString(node) || !freshStruct(node).empty() || isFreshArray(node))
        registerTemporary(node, value);  // D-014, D-016, D-018
    return value;
}

// p.scores (D-016): описатель поля-массива под именем пути — дальше код динамических массивов.
void CodeGen::bindFieldArray(const Node& node) {
    if (node.varName.find('.') == std::string::npos) return;
    switch (node.type) {
    case NODE_ARRAY_ACCESS: case NODE_ARRAY_ASSIGN: case NODE_ARRAY_PUSH: case NODE_ARRAY_POP:
    case NODE_ARRAY_RESIZE: case NODE_ARRAY_LEN: case NODE_ARRAY_CAP: case NODE_DROP: break;
    default: return;
    }
    const size_t dot = node.varName.find('.');
    Node field;
    field.type = NODE_STRUCT_FIELD;
    field.varName = node.varName.substr(0, dot);
    field.op = node.varName.substr(dot + 1);
    field.line = node.line; field.column = node.column; field.file = node.file;
    structFieldAddressOnly_ = true;
    llvm::Value* address = genOtherExpr(field);
    structFieldAddressOnly_ = false;
    std::string structName = structTypesByVariable[field.varName], type;
    for (size_t pos = 0; !structName.empty();) {
        const size_t next = field.op.find('.', pos);
        const std::string name = field.op.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
        for (const StructField& candidate : structDefs[structName]) if (candidate.name == name) type = candidate.type;
        if (next == std::string::npos) break;
        structName = type;
        pos = next + 1;
    }
    const sc::Type arrayType = sc::Type::from(type);
    dynArrays[node.varName] = address;
    dynArrayElementTypes[node.varName] = svType(arrayType.element->toString());
    unsignedElements[node.varName] = arrayType.element->isUnsigned();
}

llvm::Value* CodeGen::genOtherExpr(const Node& node) {
    bindFieldArray(node);
    if (node.type == NODE_CHAR) {
        return builder.getInt32(static_cast<uint32_t>(std::stoul(node.value)));  // буква (D-021)
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
    llvm::Value* currentPtr = nullptr;
    std::string structName;
    if (node.right && dynArrays.contains(currentVar)) {  // a[i].x растущего массива структур (1.3)
        llvm::Value* desc = dynArrays[currentVar];
        llvm::Value* data = builder.CreateLoad(builder.getPtrTy(), builder.CreateStructGEP(dynArrayType(), desc, 0));
        llvm::Value* length = builder.CreateLoad(builder.getInt64Ty(), builder.CreateStructGEP(dynArrayType(), desc, 1));
        llvm::Value* idx = genExpr(*node.right);
        emitArrayBoundsCheck(idx, length, node);
        currentPtr = builder.CreateGEP(dynArrayElementTypes[currentVar], data, {idx});
        structName = arrayStructNames[currentVar];
    } else if (node.right) {  // a[i].x: адрес элемента статического массива структур
        auto array = arrays.find(currentVar);
        auto element = arrayStructNames.find(currentVar);
        if (array == arrays.end() || element == arrayStructNames.end()) {
            codegenFailure(node, "неизвестный массив структур '" + currentVar + "'");
            return nullptr;
        }
        const unsigned n = arrayLengths[currentVar];
        llvm::Value* idx = genExpr(*node.right);
        emitArrayBoundsCheck(idx, builder.getInt64(n), node);
        currentPtr = builder.CreateInBoundsGEP(llvm::ArrayType::get(arrayElementTypes[currentVar], n),
                                               array->second, {builder.getInt32(0), idx});
        structName = element->second;
    } else {
    auto objectIt = vars.find(currentVar);
    if (objectIt == vars.end()) {
        codegenFailure(node, "неизвестная переменная '" + currentVar + "'");
        return nullptr;
    }
    currentPtr = objectIt->second;
    auto structNameIt = structTypesByVariable.find(currentVar);
    if (structNameIt == structTypesByVariable.end()) {
        codegenFailure(node, "неизвестная структура '" + currentVar + "'");
        return nullptr;
    }
    structName = structNameIt->second;
    }
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
        if (node.varType == "string") {  // D-019: s[i] — байт строки; вне [0, len(s)) — ошибка
            Node name;
            name.type = NODE_IDENTIFIER;
            name.value = node.varName;
            name.line = node.line; name.column = node.column; name.file = node.file;
            llvm::Value* str = genExpr(name);
            llvm::Value* length = builder.CreateCall(runtimeFunction("strlen"), {str});
            llvm::Value* idx = genExpr(*node.left);
            if (idx->getType()->getIntegerBitWidth() < 64)
                idx = isUnsignedExpr(*node.left) ? builder.CreateZExt(idx, builder.getInt64Ty())
                                                 : builder.CreateSExt(idx, builder.getInt64Ty());
            emitRuntimeFailureIf(builder.CreateICmpUGE(idx, length), "string index out of bounds", node);
            return builder.CreateLoad(builder.getInt8Ty(), builder.CreateGEP(builder.getInt8Ty(), str, {idx}));
        }
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
        if (targetType.isChar() && val->getType()->isIntegerTy()) {
            // (char) n: только номер буквы Юникода — 0..0x10FFFF без суррогатов (как char::from_u32 в Rust).
            llvm::Value* n = isUnsignedExpr(*node.left) ? builder.CreateZExtOrTrunc(val, builder.getInt64Ty())
                                                        : builder.CreateSExtOrTrunc(val, builder.getInt64Ty());
            llvm::Value* bad = builder.CreateOr(builder.CreateICmpUGT(n, builder.getInt64(0x10FFFF)),
                builder.CreateICmpULT(builder.CreateSub(n, builder.getInt64(0xD800)), builder.getInt64(0x800)));
            emitRuntimeFailureIf(bad, "invalid char", node);
            return builder.CreateTrunc(n, builder.getInt32Ty());
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
        // Подсказка — значение выражения; печать через "%s", а не как формат printf.
        llvm::Function* printfF = runtimeFunction("printf");
        llvm::Value* prompt = node.left ? genExpr(*node.left) : getStringPtr("");
        builder.CreateCall(printfF, {getStringPtr("%s"), prompt});
        return emitReadLine(llvm::ConstantPointerNull::get(builder.getPtrTy()), node, false);
    }
    if (node.type == NODE_INPUT) {
        // Подсказка — значение выражения; печать через "%s", а не как формат printf.
        llvm::Function* printfF = runtimeFunction("printf");
        llvm::Value* prompt = node.left ? genExpr(*node.left) : getStringPtr("");
        builder.CreateCall(printfF, {getStringPtr("%s"), prompt});
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
        if (node.varType == "string-concat") {  // a + b: malloc(len a + len b + 1), обе части копией
            llvm::Value* null = llvm::ConstantPointerNull::get(builder.getPtrTy());
            llvm::Value* left = builder.CreateSelect(builder.CreateICmpEQ(L, null), getStringPtr(""), L);
            llvm::Value* right = builder.CreateSelect(builder.CreateICmpEQ(R, null), getStringPtr(""), R);
            llvm::Value* leftLength = builder.CreateCall(runtimeFunction("strlen"), {left});
            llvm::Value* rightLength = builder.CreateAdd(builder.CreateCall(runtimeFunction("strlen"), {right}),
                                                         builder.getInt64(1));
            llvm::Value* joined = builder.CreateCall(runtimeFunction("malloc"),
                                                     {builder.CreateAdd(leftLength, rightLength)});
            llvm::Function* function = builder.GetInsertBlock()->getParent();
            llvm::BasicBlock* failed = llvm::BasicBlock::Create(context, "string_concat_failed", function);
            llvm::BasicBlock* done = llvm::BasicBlock::Create(context, "string_concat_done", function);
            builder.CreateCondBr(builder.CreateICmpEQ(joined, null), failed, done);
            builder.SetInsertPoint(failed);
            builder.CreateCall(runtimeFunction("abort"), {});
            builder.CreateUnreachable();
            builder.SetInsertPoint(done);
            builder.CreateCall(runtimeFunction("memcpy"), {joined, left, leftLength});
            builder.CreateCall(runtimeFunction("memcpy"),
                {builder.CreateInBoundsGEP(builder.getInt8Ty(), joined, leftLength), right, rightLength});
            return joined;
        }
        if (node.varType == "string-compare") {  // == != < > <= >= для строк: знак strcmp
            llvm::Value* order = builder.CreateCall(runtimeFunction("strcmp"), {L, R});
            llvm::Value* zero = builder.getInt32(0);
            if (node.op == "==") return builder.CreateICmpEQ(order, zero);
            if (node.op == "!=") return builder.CreateICmpNE(order, zero);
            if (node.op == "<") return builder.CreateICmpSLT(order, zero);
            if (node.op == ">") return builder.CreateICmpSGT(order, zero);
            if (node.op == "<=") return builder.CreateICmpSLE(order, zero);
            return builder.CreateICmpSGE(order, zero);
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
        if (node.op == "/" || node.op == "div" || node.op == "%") {
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
            if (node.op == "div" && !unsignedArith) {
                // Округление вниз (D-010): частное уменьшается на 1, если остаток ненулевой
                // и знаки операндов разные.
                llvm::Value* quotient = builder.CreateSDiv(L, R);
                llvm::Value* inexact = builder.CreateICmpNE(builder.CreateSRem(L, R), llvm::ConstantInt::get(common, 0));
                llvm::Value* signsDiffer = builder.CreateICmpSLT(builder.CreateXor(L, R), llvm::ConstantInt::get(common, 0));
                return builder.CreateSub(quotient,
                    builder.CreateZExt(builder.CreateAnd(inexact, signsDiffer), common));
            }
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
            // abs(MIN) не представим — integer overflow, как у `-x` (Rust: i32::abs паникует).
            const unsigned width = value->getType()->getIntegerBitWidth();
            emitRuntimeFailureIf(builder.CreateICmpEQ(value, builder.getInt(llvm::APInt::getSignedMinValue(width))),
                                 "integer overflow", node);
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
            } else if (val->getType()->isPointerTy()) {
                fmt = getStringPtr("%s");  // str(строка) — копия строки
            } else if (val->getType()->isIntegerTy(64)) {
                fmt = getStringPtr(isUnsignedExpr(node.args[0]) ? "%llu" : "%lld");
            } else {
                // Узкие целые в vararg-вызове расширяются до int, как в C: раньше i8 -5 давал 251.
                if (val->getType()->getIntegerBitWidth() < 32)
                    val = isUnsignedExpr(node.args[0]) || val->getType()->isIntegerTy(1)
                        ? builder.CreateZExt(val, builder.getInt32Ty())
                        : builder.CreateSExt(val, builder.getInt32Ty());
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
        if (name == "data" || name == "as_ptr")  // D-014: адрес символов строки
            return genExpr(node.args[0]);
        if (name == "move") {  // D-015/D-016: забрать строку или структуру, в старом слоте — нули
            llvm::Value* slot = vars[node.args[0].value];
            llvm::Type* type = storageType(slot);
            if (!type->isStructTy()) type = builder.getPtrTy();
            llvm::Value* value = builder.CreateLoad(type, slot);
            builder.CreateStore(llvm::Constant::getNullValue(type), slot);
            return value;
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
                if (i < ptypes.size() && ptypes[i].starts_with("view<")) {  // int[..] part — окно без копии
                    argVals.push_back(viewValue(node.args[i]));
                    continue;
                }
                if (i < ptypes.size() && ptypes[i].starts_with("dyn<")) {  // int[] v — копия или move (D-018)
                    argVals.push_back(arrayValue(node.args[i]));
                    continue;
                }
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
    bindFieldArray(node);
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
            } else if (node.varType == "char" || (node.left->type == NODE_IDENTIFIER &&
                       charVars.contains(node.left->value) && charVars[node.left->value])) {
                llvm::Value* letter = builder.CreateZExtOrTrunc(val, builder.getInt32Ty());
                llvm::Value* text = entryAlloca(llvm::ArrayType::get(builder.getInt8Ty(), 5), "sc.letter");
                builder.CreateCall(utf8EncodeFunction(), {letter, text});
                builder.CreateCall(printfFunc, {getStringPtr("%s\n"), text});
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
                elementSize(elementType)});
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
            // 1.3: буфер освобождается сам в конце блока (как поле-массив, D-017);
            // drop/del раньше обнуляют его, и free(NULL) в конце ничего не делает.
            deferred_.push_back({nullptr, {}, desc, nullptr, arrayStruct(alloc.varType)});
            deferred_.back().elements = !deferred_.back().structName.empty();
            if (structDefs.contains(alloc.varType)) {  // new P[n]: элементы — значения полей по умолчанию
                arrayStructNames[node.varName] = alloc.varType;
                Node empty;
                empty.type = NODE_STRUCT_LITERAL;
                forEachElement(data, elementType, builder.getInt64(0), size, [&](llvm::Value* item) {
                    initStructLiteral(item, llvmStructTypes[alloc.varType], alloc.varType, empty);
                });
            }
            return;
        }
        // int[] b = a / move(a) / f(); ptr b = f() / move(a) — растущий массив (D-018).
        if (node.varType.starts_with("dyn<") || (sc::Type::from(node.varType).isPtr() && isFreshArray(*node.left))) {
            const bool moved = !node.varType.starts_with("dyn<") && node.left->varName == "move";
            const std::string source = moved ? node.left->args[0].value : "";
            llvm::Type* movedElement = moved ? dynArrayElementTypes[source] : nullptr;
            const bool movedUnsigned = moved && unsignedElements[source];
            const std::string type = node.varType.starts_with("dyn<") ? node.varType
                : moved ? "dyn<i32>" : funcReturnTypes[node.left->varName];
            llvm::Value* value = arrayValue(*node.left);
            llvm::Value* desc = createStorage(dynArrayType(), node.varName);
            builder.CreateStore(value, desc);
            bindDynArray(node.varName, desc, sc::Type::from(type).element->toString());
            if (moved) { dynArrayElementTypes[node.varName] = movedElement; unsignedElements[node.varName] = movedUnsigned; }
            constVars[node.varName] = node.isConst;
            return;
        }
        // ptr p = &x: запомнить тип x, чтобы *p читал и писал значение нужного типа.
        pointeeTypes.erase(node.varName);
        if (node.left->type == NODE_ADDR && vars.contains(node.left->varName) && vars[node.left->varName])
            pointeeTypes[node.varName] = storageType(vars[node.left->varName]);
        if (node.left->type == NODE_FUNC_CALL && (node.left->varName == "data" || node.left->varName == "as_ptr"))
            pointeeTypes[node.varName] = builder.getInt8Ty();  // символы строки
        llvm::Value* val = genExpr(*node.left);
        llvm::Type* ty = node.varType.empty()
            ? val->getType() : svType(node.varType);
        llvm::Value* alloca = createStorage(ty, node.varName);
        const bool ownedString = sc::Type::from(node.varType).isStr();
        if (ownedString) val = ownString(*node.left, val);
        checkNarrowStore(val, *node.left, ty, unsignedTypeName(node.varType), node);
        val = castTo(val, ty, isUnsignedExpr(*node.left));
        builder.CreateStore(val, alloca);
        vars[node.varName] = alloca;
        if (ownedString) ownStringSlot(alloca);
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
        ownStructSlot(object, node.varType);
        constVars[node.varName] = node.isConst;
        unsignedVars[node.varName] = false;
        charVars[node.varName] = false;
        integerWidths[node.varName] = 0;
    }
    else if (node.type == NODE_ASSIGN) {
        llvm::Value* alloca = vars[node.varName];
        if (alloca && !constVars[node.varName] && dynArrays.contains(node.varName) && (isFreshArray(*node.left) ||
            (node.left->type == NODE_IDENTIFIER && (dynArrays.contains(node.left->value) || arrays.contains(node.left->value))))) {  // v = a (копия), v = move(a), v = f(): старый буфер — free
            llvm::Value* value = arrayValue(*node.left);
            if (arrayStructNames.contains(node.varName))
                dropElements(dynArrays[node.varName], arrayStruct(arrayStructNames[node.varName]));
            llvm::Value* dataSlot = builder.CreateStructGEP(dynArrayType(), dynArrays[node.varName], 0);
            builder.CreateCall(runtimeFunction("free"), {builder.CreateLoad(builder.getPtrTy(), dataSlot)});
            builder.CreateStore(value, dynArrays[node.varName]);
            return;
        }
        if (alloca && !constVars[node.varName] && dynArrays.contains(node.varName) &&
            node.left->type == NODE_DYN_ARRAY) {  // v = new T[n]: новый буфер, старый освободить (1.3)
            llvm::Value* desc = dynArrays[node.varName];
            llvm::Type* elementType = dynArrayElementTypes[node.varName];
            llvm::Value* size = emitArraySize(*node.left->left, *node.left);
            llvm::Value* count = builder.CreateSelect(
                builder.CreateICmpEQ(size, builder.getInt64(0)), builder.getInt64(1), size);
            llvm::Value* data = builder.CreateCall(runtimeFunction("calloc"), {count,
                elementSize(elementType)});
            emitRuntimeFailureIf(builder.CreateIsNull(data), "out of memory", *node.left);
            llvm::Value* dataSlot = builder.CreateStructGEP(dynArrayType(), desc, 0);
            builder.CreateCall(runtimeFunction("free"), {builder.CreateLoad(builder.getPtrTy(), dataSlot)});
            builder.CreateStore(data, dataSlot);
            builder.CreateStore(size, builder.CreateStructGEP(dynArrayType(), desc, 1));
            builder.CreateStore(size, builder.CreateStructGEP(dynArrayType(), desc, 2));
            return;
        }
        if (alloca && !constVars[node.varName]) {
            llvm::Value* val = genExpr(*node.left);
            llvm::Type* ty = storageType(alloca);
            if (ownedStringSlots_.contains(alloca)) {  // s = …: новое значение, затем free старого
                val = ownString(*node.left, val);
                llvm::Value* old = builder.CreateLoad(builder.getPtrTy(), alloca);
                builder.CreateStore(val, alloca);
                builder.CreateCall(runtimeFunction("free"), {old});
                return;
            }
            if (auto structName = structTypesByVariable.find(node.varName);
                structName != structTypesByVariable.end() && structOwnsMemory(structName->second)) {
                replaceStruct(alloca, structName->second, val, *node.left);  // w = q — копия, w = f() — без копии
                return;
            }
            checkNarrowStore(val, *node.left, ty, unsignedVars[node.varName], node);
            val = castTo(val, ty, isUnsignedExpr(*node.left));
            builder.CreateStore(val, alloca);
        }
    }
    else if (node.type == NODE_STRUCT_FIELD_ASSIGN && node.left->type == NODE_ARRAY_DECL) {
        Node target = node;  // t.scores = [1, 2]: старый буфер освобождается, новый — из списка (D-017)
        target.type = NODE_DROP;
        target.varName = node.varName + "." + node.op;
        bindFieldArray(target);
        llvm::Value* desc = dynArrays[target.varName];
        llvm::Type* element = dynArrayElementTypes[target.varName];
        llvm::Value* dataSlot = builder.CreateStructGEP(dynArrayType(), desc, 0);
        builder.CreateCall(runtimeFunction("free"), {builder.CreateLoad(builder.getPtrTy(), dataSlot)});
        const uint64_t count = node.left->args.size();
        llvm::Value* data = builder.CreateCall(runtimeFunction("calloc"), {builder.getInt64(count ? count : 1),
            elementSize(element)});
        emitRuntimeFailureIf(builder.CreateIsNull(data), "out of memory", node);
        for (uint64_t i = 0; i < count; ++i) {
            const Node& item = node.left->args[i];
            builder.CreateStore(castTo(genExpr(item), element, isUnsignedExpr(item)),
                                builder.CreateGEP(element, data, {builder.getInt64(i)}));
        }
        builder.CreateStore(data, dataSlot);
        builder.CreateStore(builder.getInt64(count), builder.CreateStructGEP(dynArrayType(), desc, 1));
        builder.CreateStore(builder.getInt64(count), builder.CreateStructGEP(dynArrayType(), desc, 2));
    }
    else if (node.type == NODE_STRUCT_FIELD_ASSIGN && (node.op.find('.') != std::string::npos || node.right)) {
        Node field;
        field.type = NODE_STRUCT_FIELD;
        field.varName = node.varName;
        field.op = node.op;
        field.right = node.right;  // a[i].x = v
        field.line = node.line; field.column = node.column; field.file = node.file;
        // a[i].x += v: индекс вычисляется один раз и для адреса, и для чтения поля.
        if (node.right) precomputed_[node.right.get()] = genExpr(*node.right);
        structFieldAddressOnly_ = true;
        llvm::Value* address = genExpr(field);
        structFieldAddressOnly_ = false;
        llvm::Type* fieldType = lastFieldType_;
        if (storeOwnedField(node, address, fieldType)) { precomputed_.clear(); return; }
        llvm::Value* value = stableString(*node.left, genExpr(*node.left));
        precomputed_.clear();
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
        if (storeOwnedField(node, builder.CreateStructGEP(aggregateType, object->second, index), fieldType))
            return;
        llvm::Value* value = stableString(*node.left, genExpr(*node.left));
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
                llvm::Value* value = stableString(*node.right, genExpr(*node.right));
                precomputed_.clear();
                if (auto structName = arrayStructNames.find(node.varName); structName != arrayStructNames.end()) {
                    if (structOwnsMemory(structName->second))  // a[i] = q: копия, старый элемент освобождается
                        replaceStruct(gep, structName->second, value, *node.right);
                    else
                        builder.CreateStore(value, gep);
                    return;
                }
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
                llvm::Value* value = stableString(*node.right, genExpr(*node.right));
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
                llvm::Value* value = stableString(*node.right, genExpr(*node.right));
                precomputed_.clear();
                if (auto structName = arrayStructNames.find(node.varName); structName != arrayStructNames.end()) {
                    if (structOwnsMemory(structName->second))  // a[i] = q: копия q, старый элемент освобождается
                        replaceStruct(gep, structName->second, value, *node.right);
                    else
                        builder.CreateStore(value, gep);
                    return;
                }
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
        } else if (node.left && retTy == dynArrayType()) {
            // return v локального массива или параметра — без копии (слот обнуляется), иначе arrayValue.
            llvm::Value* slot = node.left->type == NODE_IDENTIFIER && dynArrays.contains(node.left->value)
                ? dynArrays[node.left->value] : nullptr;
            llvm::Value* v = nullptr;
            if (slot && llvm::isa<llvm::AllocaInst>(slot) && !viewSlots_.contains(slot)) {
                v = builder.CreateLoad(dynArrayType(), slot);
                builder.CreateStore(llvm::Constant::getNullValue(dynArrayType()), slot);
            } else {
                v = arrayValue(*node.left);
            }
            emitDeferred(0);
            builder.CreateRet(v);
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
            if (name.starts_with("sc.fn.")) {  // string-результат: вызывающему — своя копия
                const auto returnType = funcReturnTypes.find(name.drop_front(6).str());
                if (returnType != funcReturnTypes.end() && sc::Type::from(returnType->second).isStr())
                    v = ownString(*node.left, v);
                else if (returnType != funcReturnTypes.end() && structOwnsMemory(returnType->second)) {
                    // return p (локальная или параметр) — отдать без копии: слот обнуляется, конец
                    // функции его не освобождает; глобальная, поле — копия; f()/move — забрать.
                    llvm::Value* slot = node.left->type == NODE_IDENTIFIER ? vars[node.left->value] : nullptr;
                    if (slot && llvm::isa<llvm::AllocaInst>(slot))
                        builder.CreateStore(llvm::Constant::getNullValue(v->getType()), slot);
                    else
                        v = takeStruct(*node.left, v, returnType->second);
                }
            }
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
        if (structDefs.contains(node.varType)) arrayStructNames[node.varName] = node.varType;
        for (unsigned i = 0; i < n; i++) {
            llvm::Value* gep = builder.CreateInBoundsGEP(
                at, arr, {builder.getInt32(0), builder.getInt32(i)});
            if (node.args[i].type == NODE_STRUCT_LITERAL) {  // P[] a = [{1, 2}]
                initStructLiteral(gep, llvmStructTypes[node.varType], node.varType, node.args[i]);
                continue;
            }
            llvm::Value* val = stableString(node.args[i], genExpr(node.args[i]));
            if (arrayStructNames.contains(node.varName)) {  // [q, f()]: копия q, результат f() забирается
                builder.CreateStore(takeStruct(node.args[i], val, node.varType), gep);
                continue;
            }
            checkNarrowStore(val, node.args[i], elementType, unsignedElements[node.varName], node.args[i]);
            val = castTo(val, elementType, isUnsignedExpr(node.args[i]));
            builder.CreateStore(val, gep);
        }
        if (arrayStructNames.contains(node.varName) && structOwnsMemory(node.varType))
            for (unsigned i = 0; i < n; i++)  // строки и массивы полей элементов освобождаются с массивом (D-016)
                ownStructSlot(builder.CreateInBoundsGEP(at, arr, {builder.getInt32(0), builder.getInt32(i)}),
                              node.varType);
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
    else if (node.type == NODE_ARRAY_PUSH && !dynArrays.contains(node.varName) && vars.contains(node.varName) &&
             ownedStringSlots_.contains(vars[node.varName])) {
        // s.push(c): строка длиннее на букву c в UTF-8 (до 4 байт и 0 в конце), D-023.
        llvm::Value* slot = vars[node.varName];
        llvm::Value* letter = genExpr(*node.left);
        llvm::Value* old = builder.CreateLoad(builder.getPtrTy(), slot);
        llvm::Value* isNull = builder.CreateICmpEQ(old, llvm::ConstantPointerNull::get(builder.getPtrTy()));
        llvm::Value* length = builder.CreateSelect(isNull, builder.getInt64(0),
            builder.CreateCall(runtimeFunction("strlen"), {builder.CreateSelect(isNull, getStringPtr(""), old)}));
        llvm::Value* grown = builder.CreateCall(runtimeFunction("realloc"),
                                                {old, builder.CreateAdd(length, builder.getInt64(5))});
        llvm::Function* function = builder.GetInsertBlock()->getParent();
        llvm::BasicBlock* failed = llvm::BasicBlock::Create(context, "string_push_failed", function);
        llvm::BasicBlock* done = llvm::BasicBlock::Create(context, "string_push_done", function);
        builder.CreateCondBr(builder.CreateICmpEQ(grown, llvm::ConstantPointerNull::get(builder.getPtrTy())),
                             failed, done);
        builder.SetInsertPoint(failed);
        builder.CreateCall(runtimeFunction("abort"), {});
        builder.CreateUnreachable();
        builder.SetInsertPoint(done);
        builder.CreateCall(utf8EncodeFunction(),
                           {letter, builder.CreateInBoundsGEP(builder.getInt8Ty(), grown, length)});
        builder.CreateStore(grown, slot);
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
        llvm::Value* value = stableString(*node.left, genExpr(*node.left));
        if (auto structName = arrayStructNames.find(node.varName); structName != arrayStructNames.end()) {
            value = takeStruct(*node.left, value, structName->second);  // push(q) — копия, push(f()) — забрать
        } else {
            checkNarrowStore(value, *node.left, elementType, unsignedElements[node.varName], *node.left);
            value = castTo(value, elementType, isUnsignedExpr(*node.left));
        }
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
        const std::string owning = arrayStructNames.contains(node.varName)
            ? arrayStruct(arrayStructNames[node.varName]) : "";
        if (!owning.empty())  // clear()/resize меньше: строки уходящих элементов освобождаются (1.3)
            dropElements(desc, owning, builder.CreateSelect(builder.CreateICmpSLT(newSize, builder.getInt64(0)),
                                                            builder.getInt64(0), newSize));
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
        llvm::Value* elementBytes = elementSize(elementType);
        builder.CreateCall(runtimeFunction("memset"),
            {builder.CreateGEP(elementType, data, {oldLength}), builder.getInt32(0),
             builder.CreateMul(builder.CreateSub(newSize, oldLength), elementBytes)});
        if (auto structName = arrayStructNames.find(node.varName); structName != arrayStructNames.end()) {
            Node empty;  // новые элементы-структуры — значения полей по умолчанию, как new P[n]
            empty.type = NODE_STRUCT_LITERAL;
            forEachElement(data, elementType, oldLength, newSize, [&](llvm::Value* item) {
                initStructLiteral(item, llvmStructTypes[structName->second], structName->second, empty);
            });
        }
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
            } else if (sc::Type::from(field.varType).isStr()) {
                sf.defaultVal.type = NODE_STRING;  // string без значения — пустая строка
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
            fieldTypes.push_back(sc::Type::from(fields[index].type).isDynArray()
                ? dynArrayType() : svType(fields[index].type));  // поле-массив — описатель внутри
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
        if (node.left) {  // P q = make(…); / P w = q; — копия значения целиком
            llvm::Value* value = genExpr(*node.left);
            vars[node.varName] = object;
            structTypesByVariable[node.varName] = node.varType;
            builder.CreateStore(takeStruct(*node.left, value, node.varType), object);  // P w = q — копия
            ownStructSlot(object, node.varType);
            return;
        }
        vars[node.varName] = object;
        structTypesByVariable[node.varName] = node.varType;
        initStructLiteral(object, aggregateType, node.varType, node);
        ownStructSlot(object, node.varType);
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
            if (ptype.starts_with("view<")) {  // int[..] part: окно на чужой массив, не освобождается (D-018)
                builder.CreateStore(&arg, alloca);
                bindDynArray(pname, alloca, sc::Type::from(ptype.substr(5, ptype.size() - 6)).toString(), false);
                viewSlots_.insert(alloca);
                constVars[pname] = false;
                continue;
            }
            if (ptype.starts_with("dyn<")) {  // int[] v: своя копия (её сделал вызывающий), free в конце (D-018)
                builder.CreateStore(&arg, alloca);
                bindDynArray(pname, alloca, sc::Type::from(ptype).element->toString());
                constVars[pname] = false;
                continue;
            }
            const bool ownedString = sc::Type::from(ptype).isStr();  // параметр — своя копия
            builder.CreateStore(ownedString ? copyString(&arg) : static_cast<llvm::Value*>(&arg), alloca);
            vars[pname] = alloca;
            if (ownedString) ownStringSlot(alloca);
            if (ty->isStructTy()) {
                structTypesByVariable[pname] = ptype;
                if (structOwnsMemory(ptype)) copyStructStrings(alloca, ptype);  // параметр — своя копия
                ownStructSlot(alloca, ptype);
            }
            unsignedVars[pname] = ptype == "u8" || ptype == "u16" ||
                ptype == "u32" || ptype == "u64" || ptype == "char";
            charVars[pname] = ptype == "char";
            constVars[pname] = false;
            integerWidths[pname] = ty->isIntegerTy() ? ty->getIntegerBitWidth() : 32;
        }
        genScope(node.children);
        if (!blockHasTerminator(builder.GetInsertBlock())) emitDeferred(0);  // параметры-строки
        deferred_.clear();
        const bool hasRet = blockHasTerminator(builder.GetInsertBlock());
        if (!hasRet && !retTy->isVoidTy()) {
            builder.CreateRet(llvm::Constant::getNullValue(retTy));  // число, ptr или структура
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
                if (i < ptypes.size() && ptypes[i].starts_with("view<")) {  // int[..] part — окно без копии
                    argVals.push_back(viewValue(node.args[i]));
                    continue;
                }
                if (i < ptypes.size() && ptypes[i].starts_with("dyn<")) {  // int[] v — копия или move (D-018)
                    argVals.push_back(arrayValue(node.args[i]));
                    continue;
                }
                llvm::Value* v = genExpr(node.args[i]);
                if (i < ptypes.size()) {
                    checkNarrowStore(v, node.args[i], svType(ptypes[i]), unsignedTypeName(ptypes[i]),
                                     node.args[i]);
                    v = castTo(v, svType(ptypes[i]), isUnsignedExpr(node.args[i]));
                }
                argVals.push_back(v);
            }
            llvm::Value* result = builder.CreateCall(funcs[node.varName], argVals);
            if (isFreshString(node) || !freshStruct(node).empty() || isFreshArray(node))
                registerTemporary(node, result);  // f(); с результатом string или структурой со строками
        } else {
            genExpr(node);
        }
    }
    else if (node.type == NODE_DEFER) {
        deferred_.push_back({node.left.get(), saveScope()});
    }
    else if (node.type == NODE_DROP || (node.type == NODE_DELETE && dynArrays.contains(node.varName))) {
        // Семантика гарантирует: динамический массив или file, освобождается один раз.
        // del динамического массива — то же, что drop (буфер обнуляется, 1.3).
        if (dynArrays.contains(node.varName)) {
            llvm::Value* desc = dynArrays[node.varName];
            if (arrayStructNames.contains(node.varName)) dropElements(desc, arrayStruct(arrayStructNames[node.varName]));
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
    else if (node.type == NODE_FOR_EACH && node.op == "letters") {
        // for c in letters(s): номер байта i, на каждом шаге — буква UTF-8 с s[i] и её длина (D-022).
        llvm::Value* text = genExpr(*node.left);
        text = builder.CreateSelect(builder.CreateICmpEQ(text, llvm::ConstantPointerNull::get(builder.getPtrTy())),
                                    getStringPtr(""), text);
        llvm::Function* function = builder.GetInsertBlock()->getParent();
        llvm::AllocaInst* index = builder.CreateAlloca(builder.getInt64Ty(), nullptr, "letters_index");
        llvm::AllocaInst* width = builder.CreateAlloca(builder.getInt64Ty(), nullptr, "letters_width");
        llvm::AllocaInst* letter = builder.CreateAlloca(builder.getInt32Ty(), nullptr, node.varName);
        builder.CreateStore(builder.getInt64(0), index);
        llvm::BasicBlock* conditionBlock = llvm::BasicBlock::Create(context, "letters_cond", function);
        llvm::BasicBlock* bodyBlock = llvm::BasicBlock::Create(context, "letters_body", function);
        llvm::BasicBlock* stepBlock = llvm::BasicBlock::Create(context, "letters_step", function);
        llvm::BasicBlock* afterBlock = llvm::BasicBlock::Create(context, "letters_after", function);
        const ScopeSnapshot parentScope = saveScope();
        const auto savedBreak = breakBlock;
        const auto savedContinue = continueBlock;
        const size_t savedLoopBase = loopDeferredBase_;
        loopDeferredBase_ = deferred_.size();
        vars[node.varName] = letter;
        unsignedVars[node.varName] = true;
        charVars[node.varName] = true;
        integerWidths[node.varName] = 32;
        breakBlock = afterBlock;
        continueBlock = stepBlock;
        builder.CreateBr(conditionBlock);
        builder.SetInsertPoint(conditionBlock);
        llvm::Value* at = builder.CreateInBoundsGEP(builder.getInt8Ty(), text,
                                                    builder.CreateLoad(builder.getInt64Ty(), index));
        builder.CreateCondBr(builder.CreateICmpNE(builder.CreateLoad(builder.getInt8Ty(), at), builder.getInt8(0)),
                             bodyBlock, afterBlock);
        builder.SetInsertPoint(bodyBlock);
        builder.CreateStore(builder.CreateCall(utf8DecodeFunction(), {at, width}), letter);
        genScope(node.children);
        if (!blockHasTerminator(builder.GetInsertBlock())) builder.CreateBr(stepBlock);
        builder.SetInsertPoint(stepBlock);
        builder.CreateStore(builder.CreateAdd(builder.CreateLoad(builder.getInt64Ty(), index),
                                              builder.CreateLoad(builder.getInt64Ty(), width)), index);
        builder.CreateBr(conditionBlock);
        builder.SetInsertPoint(afterBlock);
        restoreScope(parentScope);
        breakBlock = savedBreak;
        continueBlock = savedContinue;
        loopDeferredBase_ = savedLoopBase;
    }
    else if (node.type == NODE_FOR_EACH) {
        // Обычный массив — длина известна; растущий массив или окно — описатель {data, len, cap},
        // данные и длина читаются на каждом шаге (длину внутри цикла менять нельзя, семантика).
        llvm::Value* desc = dynArrays.contains(node.left->value) ? dynArrays[node.left->value] : nullptr;
        if (!desc && (!arrays.contains(node.left->value) || !arrayLengths.contains(node.left->value)))
            return;
        llvm::Value* array = desc ? nullptr : arrays[node.left->value];
        llvm::ArrayType* arrayType = desc ? nullptr : llvm::dyn_cast<llvm::ArrayType>(storageType(array));
        if (!desc && !arrayType) return;
        llvm::Type* elementType = desc ? dynArrayElementTypes[node.left->value] : arrayType->getElementType();
        llvm::Function* function = builder.GetInsertBlock()->getParent();
        llvm::AllocaInst* index = builder.CreateAlloca(builder.getInt64Ty(), nullptr, "foreach_index");
        builder.CreateStore(builder.getInt64(0), index);
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
        if (auto structName = arrayStructNames.find(node.left->value); structName != arrayStructNames.end())
            structTypesByVariable[node.varName] = structName->second;  // for p in people { p.age }
        unsignedVars[node.varName] = desc && unsignedElements[node.left->value];  // for b in bytes: u8
        charVars[node.varName] = false;
        integerWidths[node.varName] = elementType->isIntegerTy() ? elementType->getIntegerBitWidth() : 32;
        breakBlock = afterBlock;
        continueBlock = stepBlock;
        builder.CreateBr(conditionBlock);
        builder.SetInsertPoint(conditionBlock);
        llvm::Value* current = builder.CreateLoad(builder.getInt64Ty(), index);
        llvm::Value* length = desc
            ? builder.CreateLoad(builder.getInt64Ty(), builder.CreateStructGEP(dynArrayType(), desc, 1))
            : static_cast<llvm::Value*>(builder.getInt64(arrayLengths[node.left->value]));
        builder.CreateCondBr(builder.CreateICmpULT(current, length), bodyBlock, afterBlock);
        builder.SetInsertPoint(bodyBlock);
        llvm::Value* address = desc
            ? builder.CreateGEP(elementType, builder.CreateLoad(builder.getPtrTy(),
                  builder.CreateStructGEP(dynArrayType(), desc, 0)), {current})
            : builder.CreateInBoundsGEP(arrayType, array, {builder.getInt64(0), current});
        llvm::Value* value = builder.CreateLoad(elementType, address);
        builder.CreateStore(value, element);
        std::string ownedElement;  // for p in a: p — своя копия элемента (D-016), освобождается на каждом шаге
        if (auto structName = arrayStructNames.find(node.left->value);
            structName != arrayStructNames.end() && structOwnsMemory(structName->second)) {
            ownedElement = structName->second;
            copyStructStrings(element, ownedElement);
            ownStructSlot(element, ownedElement);
        }
        genScope(node.children);
        if (!ownedElement.empty()) {
            if (!blockHasTerminator(builder.GetInsertBlock())) dropStruct(element, ownedElement);
            deferred_.resize(loopDeferredBase_);
        }
        if ((builder.GetInsertBlock()->empty() || !builder.GetInsertBlock()->back().isTerminator())) builder.CreateBr(stepBlock);
        builder.SetInsertPoint(stepBlock);
        current = builder.CreateLoad(builder.getInt64Ty(), index);
        builder.CreateStore(builder.CreateAdd(current, builder.getInt64(1)), index);
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
        const size_t conditionMark = deferred_.size();
        llvm::Value* cond = toBool(genExpr(*node.left));
        releaseTemporaries(conditionMark);  // строки условия — на каждой проверке
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
        if (node.type != NODE_FUNC_DEF && node.type != NODE_STRUCT_DEF) genStatement(node);
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
