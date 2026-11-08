#include "codegen.h"
#include <llvm/Support/raw_ostream.h>
#include <llvm/IR/Verifier.h>
#include <llvm/ADT/ArrayRef.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>
#include <llvm/MC/TargetRegistry.h>
#include <iostream>
#include <cstdint>
#include <climits>
#include <cstdlib>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace {

[[noreturn]] void codegenFailure(const Node& node, const std::string& message) {
    throw std::logic_error(node.file + ":" + std::to_string(node.line) + ":" +
                           std::to_string(node.column) + ": внутренняя ошибка codegen: " + message);
}

bool blockHasTerminator(const llvm::BasicBlock* block) {
    return block && !block->empty() && block->back().isTerminator();
}

} // namespace

CodeGen::ScopeSnapshot CodeGen::saveScope() const {
    return {vars, unsignedVars, charVars, constVars, integerWidths, arrays, arrayLengths,
            arrayElementTypes, structTypesByVariable, deferred};
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
    deferred = scope.deferred;
}

// Emit all deferred statements in reverse order (LIFO - last deferred executes first)
void CodeGen::emitDeferred() {
    for (auto it = deferred.rbegin(); it != deferred.rend(); ++it) {
        genNode(*it);
    }
}

void CodeGen::emitAggregateInto(llvm::AllocaInst* dst, const Node& src) {
    // Plain identifier referencing another aggregate variable: a = b.
    if (src.type == NODE_IDENTIFIER) {
        if (arrays.count(src.value)) {
            llvm::AllocaInst* srcArr = arrays[src.value];
            llvm::Type* dstType = dst->getAllocatedType();
            llvm::Type* srcType = srcArr->getAllocatedType();
            if (dstType != srcType || dstType->getArrayNumElements() !=
                    srcType->getArrayNumElements())
                codegenFailure(src, "несовместимый тип массива при присваивании '" + src.value + "'");
            builder.CreateMemCpy(dst, llvm::MaybeAlign(), srcArr, llvm::MaybeAlign(),
                                 dstType->getPrimitiveSizeInBits().getFixedValue() / 8);
            return;
        }
        if (structTypesByVariable.count(src.value)) {
            llvm::AllocaInst* srcObj = vars[src.value];
            if (!srcObj) codegenFailure(src, "неизвестная переменная '" + src.value + "'");
            llvm::Type* dstType = dst->getAllocatedType();
            llvm::Type* srcType = srcObj->getAllocatedType();
            if (dstType != srcType)
                codegenFailure(src, "несовместимый тип структуры при присваивании '" + src.value + "'");
            builder.CreateMemCpy(dst, llvm::MaybeAlign(), srcObj, llvm::MaybeAlign(),
                                 dstType->getPrimitiveSizeInBits().getFixedValue() / 8);
            return;
        }
        codegenFailure(src, "невозможно присвоить aggregate значение '" + src.value + "'");
    }
    // Aggregate literal: array literal [a, b, c] or struct literal { field = v; }.
    if (src.type == NODE_ARRAY_DECL) {
        llvm::Type* dstType = dst->getAllocatedType();
        if (!dstType->isArrayTy())
            codegenFailure(src, "присваивание массива не по типу");
        llvm::ArrayType* at = llvm::cast<llvm::ArrayType>(dstType);
        unsigned n = static_cast<unsigned>(src.args.size());
        if (n != at->getNumElements())
            codegenFailure(src, "несовпадение размера массива при присваивании");
        for (unsigned i = 0; i < n; i++) {
            llvm::Value* gep = builder.CreateInBoundsGEP(
                at, dst, {builder.getInt32(0), builder.getInt32(i)});
            llvm::Value* val = genExpr(src.args[i]);
            val = castTo(val, at->getElementType(), isUnsignedExpr(src.args[i]));
            builder.CreateStore(val, gep);
        }
        return;
    }
    if (src.type == NODE_STRUCT_INSTANCE) {
        if (!structDefs.count(src.varType))
            codegenFailure(src, "неизвестная структура '" + src.varType + "'");
        const auto& fields = structDefs[src.varType];
        llvm::StructType* aggregateType = llvmStructTypes[src.varType];
        if (dst->getAllocatedType() != aggregateType) {
            codegenFailure(src, "неизвестный тип структуры '" + src.varType + "'");
        }
        std::map<std::string, const Node*> named;
        std::vector<const Node*> positional;
        for (const Node& c : src.children) {
            if (!c.varName.empty()) named[c.varName] = &c;
            else if (c.left) positional.push_back(&c);
        }
        size_t positionalIndex = 0;
        for (size_t index = 0; index < fields.size(); ++index) {
            const Node* initializer = nullptr;
            if (named.count(fields[index].name)) {
                initializer = named.at(fields[index].name)->left;
            } else if (positionalIndex < positional.size()) {
                initializer = positional[positionalIndex++]->left;
            } else {
                initializer = &fields[index].defaultVal;
            }
            llvm::Value* value = genExpr(*initializer);
            llvm::Type* fieldType = aggregateType->getStructElementType(static_cast<unsigned>(index));
            value = castTo(value, fieldType, isUnsignedExpr(*initializer));
            llvm::Value* address = builder.CreateStructGEP(aggregateType, dst,
                                                            static_cast<unsigned>(index));
            builder.CreateStore(value, address);
        }
        return;
    }
    codegenFailure(src, "невозможно присвоить aggregate значение '" +
                   (src.value.empty() ? src.varName : src.value) + "'");
}

// Returns true if the expression is a compile-time constant (literals,
// constant arrays, constant struct instances). Used to decide whether a
// global initializer can be emitted directly or must be deferred to main.
bool CodeGen::isConstantInitializer(const Node& node) {
    switch (node.type) {
        case NODE_NUMBER:
        case NODE_CHAR:
        case NODE_STRING:
            return true;
        case NODE_ARRAY_DECL:
            for (const Node& arg : node.args)
                if (!isConstantInitializer(arg)) return false;
            return true;
        case NODE_STRUCT_INSTANCE:
            for (const Node& c : node.children) {
                if (c.left && !isConstantInitializer(*c.left)) return false;
            }
            return true;
        default:
            return false;
    }
}

llvm::Value* CodeGen::getStringPtr(const std::string& str) {
    // When no function is active (global initializer), CreateGlobalString
    // crashes because it needs an insertion point. Create the global directly
    // and return a constant GEP/bitcast to i8*.
    if (!builder.GetInsertBlock()) {
        static int strCounter = 0;
        std::vector<char> chars(str.begin(), str.end());
        chars.push_back('\0');
        llvm::Constant* strConst = llvm::ConstantDataArray::get(
            context, llvm::ArrayRef<char>(chars.data(), chars.size()));
        llvm::ArrayType* arrTy = llvm::ArrayType::get(
            builder.getInt8Ty(), str.size() + 1);
        llvm::GlobalVariable* gv = new llvm::GlobalVariable(
            module, arrTy, true, llvm::GlobalVariable::PrivateLinkage,
            strConst, ".str." + std::to_string(strCounter++));
        gv->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
        return llvm::ConstantExpr::getBitCast(gv, builder.getPtrTy());
    }
    llvm::GlobalVariable* gv = builder.CreateGlobalString(str);
    return builder.CreateInBoundsGEP(gv->getValueType(), gv,
        {builder.getInt32(0), builder.getInt32(0)});
}

// Get or create the runtime panic function.
llvm::Function* CodeGen::getPanicFunction() {
    llvm::Function* panicF = module.getFunction("__sc_panic");
    if (panicF) return panicF;
    llvm::FunctionType* panicType = llvm::FunctionType::get(
        builder.getVoidTy(), {builder.getPtrTy()}, false);
    panicF = llvm::Function::Create(panicType,
        llvm::Function::ExternalLinkage, "__sc_panic", module);
    return panicF;
}

void CodeGen::emitPanic(const std::string& message) {
    llvm::Function* panicF = getPanicFunction();
    builder.CreateCall(panicF, {getStringPtr(message)});
    builder.CreateUnreachable();
}

llvm::Type* CodeGen::svType(const std::string& name) {
    if (name == "int" || name == "i32" || name == "rox" || name == "uint" || name == "u32") return builder.getInt32Ty();
    if (name == "short" || name == "i16" || name == "ushort" || name == "u16") return builder.getInt16Ty();
    if (name == "i8" || name == "u8" || name == "byte" || name == "char" || name == "chr") return builder.getInt8Ty();
    if (name == "long" || name == "i64" || name == "ulong" || name == "u64" || name == "rox64") return builder.getInt64Ty();
    if (name == "float" || name == "f32" || name == "dor") return builder.getFloatTy();
    if (name == "double" || name == "f64") return builder.getDoubleTy();
    if (name == "str" || name == "string" || name == "cos" || name == "ptr" || name == "file") return builder.getPtrTy();
    if (name == "bool" || name == "bue") return builder.getInt8Ty();
    if (name == "void") return builder.getVoidTy();
    throw std::logic_error("внутренняя ошибка codegen: неизвестный тип '" + name + "'");
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

void CodeGen::emitArrayBoundsCheck(llvm::Value* index, unsigned length,
                                   const Node& location) {
    if (index->getType()->isIntegerTy(1))
        index = builder.CreateZExt(index, builder.getInt32Ty());
    if (index->getType()->getIntegerBitWidth() < 64) {
        index = isUnsignedExpr(location) ? builder.CreateZExt(index, builder.getInt64Ty())
                                         : builder.CreateSExt(index, builder.getInt64Ty());
    }
    llvm::Value* nonNegative = isUnsignedExpr(location)
        ? builder.getTrue()
        : builder.CreateICmpSGE(index, builder.getInt64(0));
    llvm::Value* belowLength = builder.CreateICmpULT(index, builder.getInt64(length));
    llvm::Value* inBounds = builder.CreateAnd(nonNegative, belowLength);
    llvm::Function* function = builder.GetInsertBlock()->getParent();
    llvm::BasicBlock* valid = llvm::BasicBlock::Create(context, "index_valid", function);
    llvm::BasicBlock* invalid = llvm::BasicBlock::Create(context, "index_invalid", function);
    builder.CreateCondBr(inBounds, valid, invalid);

    builder.SetInsertPoint(invalid);
    const std::string message = "SC runtime error: array index out of bounds at line " +
        std::to_string(location.line) + ", column " + std::to_string(location.column);
    emitPanic(message);
    builder.SetInsertPoint(valid);
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
    if (node.type == NODE_STRUCT_FIELD) {
        const std::string key = node.varName + "." + node.op;
        auto object = vars.find(node.varName);
        auto structName = structTypesByVariable.find(node.varName);
        if (object == vars.end() || structName == structTypesByVariable.end() ||
            !structFieldIndices[structName->second].count(node.op))
            codegenFailure(node, "неизвестное поле структуры '" + key + "'");
        const unsigned fieldIndex = structFieldIndices[structName->second].at(node.op);
        llvm::Type* aggregateType = object->second->getAllocatedType();
        if (!aggregateType->isStructTy())
            codegenFailure(node, "storage структуры '" + node.varName + "' не является aggregate");
        llvm::Value* address = builder.CreateStructGEP(aggregateType, object->second, fieldIndex);
        return builder.CreateLoad(aggregateType->getStructElementType(fieldIndex), address);
    }
    if (node.type == NODE_IDENTIFIER) {
        llvm::AllocaInst* alloca = vars[node.value];
        if (alloca) {
            llvm::Type* ty = alloca->getAllocatedType();
            if (ty->isPointerTy())
                return builder.CreateLoad(builder.getPtrTy(), alloca);
            return builder.CreateLoad(ty, alloca);
        }
        // Check globals
        if (globals.count(node.value)) {
            llvm::GlobalVariable* gv = globals[node.value];
            return builder.CreateLoad(gv->getValueType(), gv);
        }
        codegenFailure(node, "неизвестная переменная '" + node.value + "'");
    }
    if (node.type == NODE_ARRAY_ACCESS) {
        // Dynamic array / pointer access (local variable)
        if (vars.count(node.varName) &&
            vars[node.varName]->getAllocatedType()->isPointerTy()) {
            llvm::Value* ptr = builder.CreateLoad(
                builder.getPtrTy(), vars[node.varName]);
            llvm::Value* idx = genExpr(*node.left);
            llvm::Value* gep = builder.CreateGEP(
                builder.getInt32Ty(), ptr, {idx});
            return builder.CreateLoad(builder.getInt32Ty(), gep);
        }
        // Dynamic array / pointer access (global variable)
        if (globals.count(node.varName) &&
            globals[node.varName]->getValueType()->isPointerTy()) {
            llvm::GlobalVariable* gv = globals[node.varName];
            llvm::Value* ptr = builder.CreateLoad(gv->getValueType(), gv);
            llvm::Value* idx = genExpr(*node.left);
            llvm::Value* gep = builder.CreateGEP(
                builder.getInt32Ty(), ptr, {idx});
            return builder.CreateLoad(builder.getInt32Ty(), gep);
        }
        // Check both local arrays and global arrays (fixed-size)
        if (arrays.count(node.varName) || globals.count(node.varName)) {
            llvm::Value* arr = nullptr;
            if (globals.count(node.varName)) {
                arr = globals[node.varName];
            } else {
                arr = arrays[node.varName];
            }
            unsigned n = arrayLengths[node.varName];
            llvm::Type* elementType = arrayElementTypes[node.varName];
            if (!elementType) elementType = builder.getInt32Ty();
            llvm::ArrayType* at = llvm::ArrayType::get(elementType, n);
            llvm::Value* idx = genExpr(*node.left);
            emitArrayBoundsCheck(idx, n, node);
            llvm::Value* gep = builder.CreateInBoundsGEP(
                at, arr, {builder.getInt32(0), idx});
            return builder.CreateLoad(elementType, gep);
        }
        codegenFailure(node, "неизвестный массив '" + node.varName + "'");
    }
    if (node.type == NODE_STR_LEN) {
        llvm::FunctionType* strlenType =
            llvm::FunctionType::get(builder.getInt32Ty(),
                {builder.getPtrTy()}, false);
        llvm::Function* strlenF = module.getFunction("strlen");
        if (!strlenF) strlenF = llvm::Function::Create(strlenType,
            llvm::Function::ExternalLinkage, "strlen", module);
        llvm::Value* str = nullptr;
        if (vars.count(node.varName)) {
            str = builder.CreateLoad(builder.getPtrTy(), vars[node.varName]);
        } else if (globals.count(node.varName)) {
            llvm::GlobalVariable* gv = globals[node.varName];
            str = builder.CreateLoad(gv->getValueType(), gv);
        }
        if (!str) codegenFailure(node, "неизвестная строка '" + node.varName + "'");
        else return builder.CreateCall(strlenF, {str});
    }
    if (node.type == NODE_STR_INDEX) {
        llvm::Value* str = nullptr;
        if (vars.count(node.varName)) {
            str = builder.CreateLoad(builder.getPtrTy(), vars[node.varName]);
        } else if (globals.count(node.varName)) {
            llvm::GlobalVariable* gv = globals[node.varName];
            str = builder.CreateLoad(gv->getValueType(), gv);
        }
        if (!str) codegenFailure(node, "неизвестная строка '" + node.varName + "'");
        llvm::Value* idx = genExpr(*node.left);
        if (idx->getType()->getIntegerBitWidth() != 32)
            idx = builder.CreateSExt(idx, builder.getInt32Ty());
        llvm::Value* gep = builder.CreateGEP(builder.getInt8Ty(), str, {idx});
        return builder.CreateLoad(builder.getInt8Ty(), gep);
    }
    if (node.type == NODE_ADDR) {
        llvm::AllocaInst* alloca = vars[node.varName];
        if (alloca) return alloca;
        // Check globals - return the global variable pointer
        if (globals.count(node.varName)) return globals[node.varName];
        codegenFailure(node, "невозможно получить адрес неизвестной переменной '" + node.varName + "'");
    }
    if (node.type == NODE_DEREF) {
        llvm::AllocaInst* alloca = vars[node.varName];
        if (alloca) {
            llvm::Value* ptr = builder.CreateLoad(builder.getPtrTy(), alloca);
            return builder.CreateLoad(builder.getInt32Ty(), ptr);
        }
        // Check globals - global pointer is already a ptr
        if (globals.count(node.varName)) {
            llvm::GlobalVariable* gv = globals[node.varName];
            return builder.CreateLoad(gv->getValueType(), gv);
        }
        codegenFailure(node, "неизвестный указатель '" + node.varName + "'");
    }
    if (node.type == NODE_DYN_ARRAY) {
        llvm::FunctionType* mallocType =
            llvm::FunctionType::get(builder.getPtrTy(),
                {builder.getInt64Ty()}, false);
        llvm::Function* mallocF = module.getFunction("malloc");
        if (!mallocF) mallocF = llvm::Function::Create(mallocType,
            llvm::Function::ExternalLinkage, "malloc", module);
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
        llvm::Value* fp = nullptr;
        // Check local variable first
        if (vars.count(node.varName)) {
            llvm::AllocaInst* fileAlloca = vars[node.varName];
            fp = builder.CreateLoad(builder.getPtrTy(), fileAlloca);
        }
        // Check global variable
        else if (globals.count(node.varName)) {
            llvm::GlobalVariable* gv = globals[node.varName];
            fp = builder.CreateLoad(gv->getValueType(), gv);
        }
        if (!fp) return getStringPtr("");
        llvm::FunctionType* fgetsType = llvm::FunctionType::get(
            builder.getPtrTy(),
            {builder.getPtrTy(), builder.getInt32Ty(), builder.getPtrTy()}, false);
        llvm::Function* fgetsF = module.getFunction("fgets");
        if (!fgetsF) fgetsF = llvm::Function::Create(fgetsType,
            llvm::Function::ExternalLinkage, "fgets", module);
        llvm::AllocaInst* buf = builder.CreateAlloca(
            llvm::ArrayType::get(builder.getInt8Ty(), 4096), nullptr, "filebuf");
        llvm::Value* bufPtr = builder.CreateInBoundsGEP(
            llvm::ArrayType::get(builder.getInt8Ty(), 4096), buf,
            {builder.getInt32(0), builder.getInt32(0)});
        builder.CreateCall(fgetsF,
            {bufPtr, builder.getInt32(4096), fp});
        return bufPtr;
    }
    if (node.type == NODE_FILE_OPEN) {
        llvm::FunctionType* fopenType =
            llvm::FunctionType::get(builder.getPtrTy(),
                {builder.getPtrTy(), builder.getPtrTy()}, false);
        llvm::Function* fopenF = module.getFunction("fopen");
        if (!fopenF) fopenF = llvm::Function::Create(fopenType,
            llvm::Function::ExternalLinkage, "fopen", module);
        llvm::Value* fname = node.left ? genExpr(*node.left) : getStringPtr("");
        llvm::Value* mode = node.right ? genExpr(*node.right) : getStringPtr("w");
        return builder.CreateCall(fopenF, {fname, mode});
    }
    if (node.type == NODE_INPUT && node.varType == "cos") {
        llvm::FunctionType* printfType =
            llvm::FunctionType::get(builder.getInt32Ty(),
                {builder.getPtrTy()}, true);
        llvm::Function* printfF = module.getFunction("printf");
        if (!printfF) printfF = llvm::Function::Create(printfType,
            llvm::Function::ExternalLinkage, "printf", module);
        llvm::FunctionType* scanfType =
            llvm::FunctionType::get(builder.getInt32Ty(),
                {builder.getPtrTy()}, true);
        llvm::Function* scanfF = module.getFunction("scanf");
        if (!scanfF) scanfF = llvm::Function::Create(scanfType,
            llvm::Function::ExternalLinkage, "scanf", module);
        llvm::Value* prompt = getStringPtr(node.value);
        builder.CreateCall(printfF, {prompt});
        llvm::AllocaInst* buf = builder.CreateAlloca(
            llvm::ArrayType::get(builder.getInt8Ty(), 256), nullptr, "strinput");
        llvm::Value* bufPtr = builder.CreateInBoundsGEP(
            llvm::ArrayType::get(builder.getInt8Ty(), 256), buf,
            {builder.getInt32(0), builder.getInt32(0)});
        llvm::Value* fmt = getStringPtr("%255s");
        builder.CreateCall(scanfF, {fmt, bufPtr});
        return bufPtr;
    }
    if (node.type == NODE_INPUT) {
        llvm::FunctionType* printfType =
            llvm::FunctionType::get(builder.getInt32Ty(),
                {builder.getPtrTy()}, true);
        llvm::Function* printfF = module.getFunction("printf");
        if (!printfF) printfF = llvm::Function::Create(printfType,
            llvm::Function::ExternalLinkage, "printf", module);
        llvm::FunctionType* scanfType =
            llvm::FunctionType::get(builder.getInt32Ty(),
                {builder.getPtrTy()}, true);
        llvm::Function* scanfF = module.getFunction("scanf");
        if (!scanfF) scanfF = llvm::Function::Create(scanfType,
            llvm::Function::ExternalLinkage, "scanf", module);
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
        if (node.op == "+" && L->getType()->isPointerTy() && R->getType()->isPointerTy()) {
            // String concatenation: both operands are str pointers.
            llvm::FunctionType* concatType = llvm::FunctionType::get(
                builder.getPtrTy(), {builder.getPtrTy(), builder.getPtrTy()}, false);
            llvm::Function* concatF = module.getFunction("__sc_concat");
            if (!concatF) concatF = llvm::Function::Create(concatType,
                llvm::Function::ExternalLinkage, "__sc_concat", module);
            return builder.CreateCall(concatF, {L, R});
        }
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
            llvm::FunctionType* functionType = llvm::FunctionType::get(
                builder.getDoubleTy(), {builder.getDoubleTy(), builder.getDoubleTy()}, false);
            llvm::Function* powerFunction = module.getFunction("pow");
            if (!powerFunction) powerFunction = llvm::Function::Create(
                functionType, llvm::Function::ExternalLinkage, "pow", module);
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
        // Struct equality: both operands must be the same struct type (checked in semantic).
        // Use byte-wise memcmp via runtime helper. Struct values are loaded as
        // aggregate SSA values; store each into a temp alloca to obtain a
        // comparable byte pointer.
        if ((node.op == "==" || node.op == "!=") &&
            L->getType()->isStructTy() && R->getType()->isStructTy() &&
            L->getType()->getStructName() == R->getType()->getStructName() &&
            llvmStructTypes.count(L->getType()->getStructName().str())) {
            llvm::Type* structTy = L->getType();
            // Get struct size in bytes
            const auto& fields = structDefs[structTy->getStructName().str()];
            unsigned totalSize = 0;
            for (const auto& f : fields) {
                totalSize += svType(f.type)->getPrimitiveSizeInBits() / 8;
            }
            if (totalSize == 0) {
                // Fallback: use DataLayout to compute size
                llvm::DataLayout DL(module.getDataLayout());
                totalSize = DL.getTypeAllocSize(structTy);
            }
            llvm::AllocaInst* aTmp = builder.CreateAlloca(structTy);
            llvm::AllocaInst* bTmp = builder.CreateAlloca(structTy);
            builder.CreateStore(L, aTmp);
            builder.CreateStore(R, bTmp);
            llvm::Value* aPtr = builder.CreateBitCast(aTmp, builder.getPtrTy());
            llvm::Value* bPtr = builder.CreateBitCast(bTmp, builder.getPtrTy());
            llvm::FunctionType* eqType = llvm::FunctionType::get(
                builder.getInt32Ty(),
                {builder.getPtrTy(), builder.getPtrTy(), builder.getInt64Ty()}, false);
            llvm::Function* eqF = module.getFunction("__sc_struct_eq");
            if (!eqF) eqF = llvm::Function::Create(eqType,
                llvm::Function::ExternalLinkage, "__sc_struct_eq", module);
            llvm::Value* res = builder.CreateCall(eqF, {aPtr, bPtr, builder.getInt64(totalSize)});
            if (node.op == "==")
                return builder.CreateICmpEQ(res, builder.getInt32(1));
            else
                return builder.CreateICmpEQ(res, builder.getInt32(0));
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
            builder.CreateCondBr(valid, validBlock, trapBlock);
            builder.SetInsertPoint(trapBlock);
            emitPanic("SC runtime error: invalid shift amount");
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
            llvm::FunctionType* type = name == "pow"
                ? llvm::FunctionType::get(builder.getDoubleTy(),
                    {builder.getDoubleTy(), builder.getDoubleTy()}, false)
                : llvm::FunctionType::get(builder.getDoubleTy(),
                    {builder.getDoubleTy()}, false);
            llvm::Function* function = module.getFunction(mathName);
            if (!function) function = llvm::Function::Create(type,
                llvm::Function::ExternalLinkage, mathName, module);
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
            llvm::FunctionType* sprintfType =
                llvm::FunctionType::get(builder.getInt32Ty(),
                    {builder.getPtrTy(), builder.getPtrTy()}, true);
            llvm::Function* sprintfF = module.getFunction("sprintf");
            if (!sprintfF) sprintfF = llvm::Function::Create(sprintfType,
                llvm::Function::ExternalLinkage, "sprintf", module);
            llvm::AllocaInst* buf = builder.CreateAlloca(
                llvm::ArrayType::get(builder.getInt8Ty(), 64), nullptr, "strbuf");
            llvm::Value* bufPtr = builder.CreateInBoundsGEP(
                llvm::ArrayType::get(builder.getInt8Ty(), 64), buf,
                {builder.getInt32(0), builder.getInt32(0)});
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
            builder.CreateCall(sprintfF, {bufPtr, fmt, val});
            return bufPtr;
        }
        if (name == "ser") {
            llvm::FunctionType* strcmpType =
                llvm::FunctionType::get(builder.getInt32Ty(),
                    {builder.getPtrTy(), builder.getPtrTy()}, false);
            llvm::Function* strcmpF = module.getFunction("strcmp");
            if (!strcmpF) strcmpF = llvm::Function::Create(strcmpType,
                llvm::Function::ExternalLinkage, "strcmp", module);
            llvm::Value* s1 = genExpr(node.args[0]);
            llvm::Value* s2 = genExpr(node.args[1]);
            llvm::Value* res = builder.CreateCall(strcmpF, {s1, s2});
            return builder.CreateICmpEQ(res, builder.getInt32(0));
        }
        if (name == "rep" && node.args.size() == 3) {
            llvm::FunctionType* strlenType = llvm::FunctionType::get(
                builder.getInt64Ty(), {builder.getPtrTy()}, false);
            llvm::Function* strlenF = module.getFunction("strlen");
            if (!strlenF) strlenF = llvm::Function::Create(strlenType,
                llvm::Function::ExternalLinkage, "strlen", module);
            llvm::FunctionType* strstrType = llvm::FunctionType::get(
                builder.getPtrTy(), {builder.getPtrTy(), builder.getPtrTy()}, false);
            llvm::Function* strstrF = module.getFunction("strstr");
            if (!strstrF) strstrF = llvm::Function::Create(strstrType,
                llvm::Function::ExternalLinkage, "strstr", module);
            llvm::FunctionType* memcpyType = llvm::FunctionType::get(
                builder.getPtrTy(), {builder.getPtrTy(), builder.getPtrTy(), builder.getInt64Ty()}, false);
            llvm::Function* memcpyF = module.getFunction("memcpy");
            if (!memcpyF) memcpyF = llvm::Function::Create(memcpyType,
                llvm::Function::ExternalLinkage, "memcpy", module);
            llvm::FunctionType* mallocType = llvm::FunctionType::get(
                builder.getPtrTy(), {builder.getInt64Ty()}, false);
            llvm::Function* mallocF = module.getFunction("malloc");
            if (!mallocF) mallocF = llvm::Function::Create(mallocType,
                llvm::Function::ExternalLinkage, "malloc", module);

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
                emitPanic("SC runtime error: size overflow in rep");
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
                emitPanic("SC runtime error: out of memory in rep");
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
            llvm::FunctionType* ft = llvm::FunctionType::get(
                builder.getDoubleTy(), {builder.getDoubleTy()}, false);
            llvm::Function* f = module.getFunction(
                name == "flo" ? "floor" : name == "cel" ? "ceil" : "round");
            std::string cname = name == "flo" ? "floor" : name == "cel" ? "ceil" : "round";
            f = module.getFunction(cname);
            if (!f) f = llvm::Function::Create(ft,
                llvm::Function::ExternalLinkage, cname, module);
            llvm::Value* arg = genExpr(node.args[0]);
            if (arg->getType()->isIntegerTy())
                arg = builder.CreateSIToFP(arg, builder.getDoubleTy());
            llvm::Value* res = builder.CreateCall(f, {arg});
            return builder.CreateFPToSI(res, builder.getInt32Ty());
        }
        if (name == "ran") {
            llvm::FunctionType* ft = llvm::FunctionType::get(
                builder.getInt32Ty(), {}, false);
            llvm::Function* f = module.getFunction("rand");
            if (!f) f = llvm::Function::Create(ft,
                llvm::Function::ExternalLinkage, "rand", module);
            return builder.CreateCall(f, {});
        }
        if (name == "ar") {
            llvm::FunctionType* strlenType =
                llvm::FunctionType::get(builder.getInt32Ty(),
                    {builder.getPtrTy()}, false);
            llvm::Function* strlenF = module.getFunction("strlen");
            if (!strlenF) strlenF = llvm::Function::Create(strlenType,
                llvm::Function::ExternalLinkage, "strlen", module);
            llvm::Value* str = genExpr(node.args[0]);
            return builder.CreateCall(strlenF, {str});
        }
        // пользовательские функции
        std::string callName = node.mangledName.empty() ? name : node.mangledName;
        if (funcs.count(callName)) {
            std::vector<llvm::Value*> argVals;
            llvm::Function* fn = funcs[callName];
            const auto& ptypes = funcParamTypes[callName];
            for (size_t i = 0; i < node.args.size(); i++) {
                llvm::Value* v = genExpr(node.args[i]);
                if (i < ptypes.size())
                    v = castTo(v, svType(ptypes[i]));
                argVals.push_back(v);
            }
            llvm::Value* ret = builder.CreateCall(fn, argVals);
            std::string rt = funcReturnTypes.count(callName)
                ? funcReturnTypes[callName] : "rox";
            return castTo(ret, svType(rt));
        }
        codegenFailure(node, "неизвестная функция '" + name + "' при генерации LLVM");
    }
    codegenFailure(node, "неподдерживаемый узел выражения");
}

void CodeGen::genNode(const Node& node) {
    if (blockHasTerminator(builder.GetInsertBlock()))
        return;
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
                // Booleans are i1 values that must be zero-extended to 1/0, not
                // sign-extended to -1/0, when printed as a decimal integer.
                if (val->getType()->getIntegerBitWidth() == 1)
                    val = builder.CreateZExt(val, builder.getInt32Ty());
                else
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
        // If we're at global scope (no current function), create a global variable.
        // NB: top-level declarations are pre-registered as globals in the first
        // pass of generate(); declarations encountered inside function bodies
        // (including main) must be local allocas so shadowing works.
        llvm::Function* curFunc = builder.GetInsertBlock() ? builder.GetInsertBlock()->getParent() : nullptr;
        llvm::Type* ty = nullptr;
        if (!curFunc) {
            // Global variable — initializer must be a compile-time constant.
            // If the initializer is not a constant (e.g. a function call), defer
            // the assignment to main's body so the instruction has a parent.
            ty = node.varType.empty() ? svType("i32") : svType(node.varType);
            bool isConstantInit = isConstantInitializer(*node.left);
            llvm::Constant* init = nullptr;
            if (isConstantInit) {
                llvm::Value* val = genExpr(*node.left);
                val = castTo(val, ty, isUnsignedExpr(*node.left));
                init = llvm::dyn_cast<llvm::Constant>(val);
                if (!init) {
                    // String literals return a GEP instruction; create a global
                    // string constant and bitcast to i8* for the initializer.
                    if (node.left->type == NODE_STRING) {
                        static int strCounter = 0;
                        std::vector<char> chars(node.left->value.begin(), node.left->value.end());
                        chars.push_back('\0');
                        llvm::Constant* data =
                            llvm::ConstantDataArray::get(
                                context, llvm::ArrayRef<char>(chars.data(), chars.size()));
                        llvm::ArrayType* arrTy = llvm::ArrayType::get(
                            builder.getInt8Ty(), node.left->value.size() + 1);
                        llvm::GlobalVariable* strGV = new llvm::GlobalVariable(
                            module, arrTy, true, llvm::GlobalVariable::PrivateLinkage,
                            data, ".str." + std::to_string(strCounter++));
                        strGV->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
                        init = llvm::ConstantExpr::getBitCast(strGV, builder.getPtrTy());
                    } else {
                        init = llvm::Constant::getNullValue(ty);
                    }
                }
            } else {
                init = llvm::Constant::getNullValue(ty);
                globalDeferred.push_back(node);
            }
            llvm::GlobalVariable* gv = new llvm::GlobalVariable(
                module, ty, node.isConst, llvm::GlobalVariable::ExternalLinkage,
                init, node.varName);
            gv->setInitializer(init);
            globals[node.varName] = gv;
            globalIsConst[node.varName] = node.isConst;
        } else {
            // Local variable
            llvm::Value* val = genExpr(*node.left);
            ty = node.varType.empty()
                ? val->getType() : svType(node.varType);
            val = castTo(val, ty, isUnsignedExpr(*node.left));
            llvm::AllocaInst* alloca =
                builder.CreateAlloca(ty, nullptr, node.varName);
            builder.CreateStore(val, alloca);
            vars[node.varName] = alloca;
            constVars[node.varName] = node.isConst;
        }
        unsignedVars[node.varName] = node.varType == "u8" || node.varType == "u16" ||
            node.varType == "u32" || node.varType == "u64" || node.varType == "char";
        charVars[node.varName] = node.varType == "char";
        integerWidths[node.varName] = ty->isIntegerTy() ? ty->getIntegerBitWidth() : 32;
    }
    else if (node.type == NODE_ASSIGN) {
        // Check const for both local and global
        bool isConst = constVars[node.varName] || globalIsConst[node.varName];
        if (isConst)
            codegenFailure(node, "нельзя присвоить значение константе '" + node.varName + "'");
        // Aggregate assignment (whole array or whole struct). Falls back to
        // element-wise copy for literals, memcpy for aggregate-to-aggregate.
        if (arrays.count(node.varName)) {
            emitAggregateInto(arrays[node.varName], *node.left);
            return;
        }
        if (structTypesByVariable.count(node.varName)) {
            llvm::AllocaInst* obj = vars[node.varName];
            if (obj) emitAggregateInto(obj, *node.left);
            else codegenFailure(node, "неизвестная переменная '" + node.varName + "'");
            return;
        }
        llvm::AllocaInst* alloca = vars[node.varName];
        if (alloca) {
            llvm::Value* val = genExpr(*node.left);
            llvm::Type* ty = alloca->getAllocatedType();
            val = castTo(val, ty, isUnsignedExpr(*node.left));
            builder.CreateStore(val, alloca);
        } else if (globals.count(node.varName)) {
            llvm::GlobalVariable* gv = globals[node.varName];
            llvm::Value* val = genExpr(*node.left);
            val = castTo(val, gv->getValueType(), isUnsignedExpr(*node.left));
            builder.CreateStore(val, gv);
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
        llvm::Type* aggregateType = object->second->getAllocatedType();
        const unsigned index = fieldIndex->second;
        llvm::Type* fieldType = aggregateType->getStructElementType(index);
        llvm::Value* value = genExpr(*node.left);
        value = castTo(value, fieldType, isUnsignedExpr(*node.left));
        llvm::Value* address = builder.CreateStructGEP(aggregateType, object->second, index);
        builder.CreateStore(value, address);
    }
    else if (node.type == NODE_ARRAY_ASSIGN) {
        if (node.left && node.right) {
            // Dynamic array / pointer access (local variable)
            if (vars.count(node.varName) &&
                vars[node.varName]->getAllocatedType()->isPointerTy()) {
                llvm::Value* ptr = builder.CreateLoad(
                    builder.getPtrTy(), vars[node.varName]);
                llvm::Value* idx = genExpr(*node.left);
                llvm::Value* gep = builder.CreateGEP(
                    builder.getInt32Ty(), ptr, {idx});
                builder.CreateStore(genExpr(*node.right), gep);
            }
            // Dynamic array / pointer access (global variable)
            else if (globals.count(node.varName) &&
                     globals[node.varName]->getValueType()->isPointerTy()) {
                llvm::GlobalVariable* gv = globals[node.varName];
                llvm::Value* ptr = builder.CreateLoad(gv->getValueType(), gv);
                llvm::Value* idx = genExpr(*node.left);
                llvm::Value* gep = builder.CreateGEP(
                    builder.getInt32Ty(), ptr, {idx});
                builder.CreateStore(genExpr(*node.right), gep);
            }
            // Fixed-size array access (local or global)
            else if (arrays.count(node.varName) || globals.count(node.varName)) {
                llvm::Value* arr = nullptr;
                if (globals.count(node.varName)) {
                    arr = globals[node.varName];
                } else {
                    arr = arrays[node.varName];
                }
                unsigned n = arrayLengths[node.varName];
                llvm::Type* elementType = arrayElementTypes[node.varName];
                if (!elementType) elementType = builder.getInt32Ty();
                llvm::ArrayType* at = llvm::ArrayType::get(elementType, n);
                llvm::Value* idx = genExpr(*node.left);
                emitArrayBoundsCheck(idx, n, node);
                llvm::Value* gep = builder.CreateInBoundsGEP(
                    at, arr, {builder.getInt32(0), idx});
                builder.CreateStore(genExpr(*node.right), gep);
            }
        }
    }
    else if (node.type == NODE_DEREF_ASSIGN) {
        llvm::AllocaInst* alloca = vars[node.varName];
        if (alloca) {
            llvm::Value* ptr = builder.CreateLoad(builder.getPtrTy(), alloca);
            builder.CreateStore(genExpr(*node.left), ptr);
        } else if (globals.count(node.varName)) {
            // Global pointer variable
            llvm::GlobalVariable* gv = globals[node.varName];
            llvm::Value* ptr = builder.CreateLoad(gv->getValueType(), gv);
            builder.CreateStore(genExpr(*node.left), ptr);
        }
    }
    else if (node.type == NODE_RETURN) {
        llvm::Function* cur = builder.GetInsertBlock()->getParent();
        llvm::Type* retTy = cur->getReturnType();
        // Run deferred statements FIRST, then evaluate the return expression.
        // This matches the expected semantics where defer mutations are
        // visible in the return value.
        emitDeferred();
        if (retTy->isVoidTy()) {
            builder.CreateRetVoid();
        } else if (node.left) {
            llvm::Value* retval = genExpr(*node.left);
            retval = castTo(retval, retTy, isUnsignedExpr(*node.left));
            builder.CreateRet(retval);
        }
    }
    else if (node.type == NODE_BREAK) {
        if (breakBlock) builder.CreateBr(breakBlock);
    }
    else if (node.type == NODE_CONTINUE) {
        if (continueBlock) builder.CreateBr(continueBlock);
    }
    else if (node.type == NODE_DEFER) {
        // Collect deferred statements to execute at function exit
        for (const Node& child : node.children) {
            deferred.push_back(child);
        }
    }
    else if (node.type == NODE_ARRAY_DECL) {
        unsigned n = (unsigned)node.args.size();
        llvm::Type* elementType = svType(node.varType);
        llvm::ArrayType* at = llvm::ArrayType::get(elementType, n);
        // If we're at global scope (no current function), create a global variable.
        // NB: top-level declarations are pre-registered as globals in the first
        // pass of generate(); declarations encountered inside function bodies
        // (including main) must be local allocas so shadowing works.
        llvm::Function* curFunc = builder.GetInsertBlock() ? builder.GetInsertBlock()->getParent() : nullptr;
        if (!curFunc) {
            // Global array - need constant initializer
            std::vector<llvm::Constant*> elements;
            bool allConstant = true;
            for (unsigned i = 0; i < n; i++) {
                if (isConstantInitializer(node.args[i])) {
                    llvm::Value* val = genExpr(node.args[i]);
                    val = castTo(val, elementType, isUnsignedExpr(node.args[i]));
                    llvm::Constant* c = llvm::dyn_cast<llvm::Constant>(val);
                    if (!c) c = llvm::Constant::getNullValue(elementType);
                    elements.push_back(c);
                } else {
                    elements.push_back(llvm::Constant::getNullValue(elementType));
                    allConstant = false;
                }
            }
            llvm::Constant* init = llvm::ConstantArray::get(at, elements);
            llvm::GlobalVariable* gv = new llvm::GlobalVariable(
                module, at, node.isConst, llvm::GlobalVariable::ExternalLinkage,
                init, node.varName);
            gv->setInitializer(init);
            globals[node.varName] = gv;
            globalIsConst[node.varName] = node.isConst;
            // Also track in arrays map for codegen (using the global as storage)
            arrayLengths[node.varName] = n;
            arrayElementTypes[node.varName] = elementType;
            if (!allConstant) globalDeferred.push_back(node);
        } else {
            // Local array
            llvm::AllocaInst* arr = builder.CreateAlloca(at, nullptr, node.varName);
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
    }
    else if (node.type == NODE_STRUCT_DEF) {
        std::vector<StructField> fields;
        for (const Node& field : node.children) {
            StructField sf;
            sf.type = field.varType;
            sf.name = field.varName;
            sf.defaultVal = *field.left;
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
            codegenFailure(node, "неизвестная структура: " + node.varType);
        }
        const auto& fields = structDefs[node.varType];
        std::map<std::string, const Node*> named;
        std::vector<const Node*> positional;
        for (const Node& c : node.children) {
            if (!c.varName.empty()) named[c.varName] = &c;
            else if (c.left) positional.push_back(&c);
        }
        llvm::StructType* aggregateType = llvmStructTypes[node.varType];
        llvm::AllocaInst* object = builder.CreateAlloca(aggregateType, nullptr, node.varName);
        vars[node.varName] = object;
        structTypesByVariable[node.varName] = node.varType;
        size_t positionalIndex = 0;
        for (size_t index = 0; index < fields.size(); ++index) {
            const StructField& field = fields[index];
            const Node* initializer;
            if (named.count(field.name)) {
                initializer = named.at(field.name)->left;
            } else if (positionalIndex < positional.size()) {
                initializer = positional[positionalIndex++]->left;
            } else {
                initializer = &field.defaultVal;
            }
            llvm::Value* value = genExpr(*initializer);
            llvm::Type* fieldType = aggregateType->getStructElementType(static_cast<unsigned>(index));
            value = castTo(value, fieldType, isUnsignedExpr(*initializer));
            llvm::Value* address = builder.CreateStructGEP(aggregateType, object,
                                                            static_cast<unsigned>(index));
            builder.CreateStore(value, address);
        }
    }
    else if (node.type == NODE_FUNC_DEF) {
        std::string mangled = node.mangledName.empty() ? node.varName : node.mangledName;
        std::vector<std::string> ptypes = funcParamTypes[mangled];
        llvm::Type* retTy = svType(funcReturnTypes[mangled]);
        llvm::Function* func = funcs[mangled];
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
        auto savedDeferred = deferred;
        // Clear deferred for this function scope
        deferred.clear();
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
        if (!hasRet) {
            // Emit deferred statements before implicit return
            emitDeferred();
            if (!retTy->isVoidTy()) {
                if (retTy->isDoubleTy())
                    builder.CreateRet(llvm::ConstantFP::get(builder.getDoubleTy(), 0.0));
                else if (retTy->isPointerTy())
                    builder.CreateRet(llvm::ConstantPointerNull::get(builder.getPtrTy()));
                else
                    builder.CreateRet(llvm::ConstantInt::get(retTy, 0));
            } else {
                builder.CreateRetVoid();
            }
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
        deferred = savedDeferred;
        funcs[mangled] = func;
    }
    else if (node.type == NODE_FUNC_CALL) {
        std::string callName = node.mangledName.empty() ? node.varName : node.mangledName;
        if (funcs.count(callName)) {
            std::vector<llvm::Value*> argVals;
            const auto& ptypes = funcParamTypes[callName];
            for (size_t i = 0; i < node.args.size(); i++) {
                llvm::Value* v = genExpr(node.args[i]);
                if (i < ptypes.size())
                    v = castTo(v, svType(ptypes[i]));
                argVals.push_back(v);
            }
            builder.CreateCall(funcs[callName], argVals);
        } else {
            genExpr(node);
        }
    }
    else if (node.type == NODE_DELETE) {
        llvm::FunctionType* freeType =
            llvm::FunctionType::get(builder.getVoidTy(),
                {builder.getPtrTy()}, false);
        llvm::Function* freeF = module.getFunction("free");
        if (!freeF) freeF = llvm::Function::Create(freeType,
            llvm::Function::ExternalLinkage, "free", module);
        llvm::AllocaInst* alloca = vars[node.varName];
        if (alloca) {
            llvm::Value* ptr = builder.CreateLoad(builder.getPtrTy(), alloca);
            builder.CreateCall(freeF, {ptr});
        }
    }
    else if (node.type == NODE_FILE_WRITE) {
        llvm::FunctionType* fputsType =
            llvm::FunctionType::get(builder.getInt32Ty(),
                {builder.getPtrTy(), builder.getPtrTy()}, false);
        llvm::Function* fputsF = module.getFunction("fputs");
        if (!fputsF) fputsF = llvm::Function::Create(fputsType,
            llvm::Function::ExternalLinkage, "fputs", module);
        llvm::Value* filePtr = nullptr;
        // Check local variable first
        if (vars.count(node.varName)) {
            llvm::AllocaInst* fileAlloca = vars[node.varName];
            filePtr = builder.CreateLoad(builder.getPtrTy(), fileAlloca);
        }
        // Check global variable
        else if (globals.count(node.varName)) {
            llvm::GlobalVariable* gv = globals[node.varName];
            filePtr = builder.CreateLoad(gv->getValueType(), gv);
        }
        if (filePtr) {
            llvm::Value* str = genExpr(*node.left);
            builder.CreateCall(fputsF, {str, filePtr});
        }
    }
    else if (node.type == NODE_INPUT) {
        genExpr(node);
    }
    else if (node.type == NODE_FILE_CLOSE) {
        llvm::FunctionType* fcloseType =
            llvm::FunctionType::get(builder.getInt32Ty(),
                {builder.getPtrTy()}, false);
        llvm::Function* fcloseF = module.getFunction("fclose");
        if (!fcloseF) fcloseF = llvm::Function::Create(fcloseType,
            llvm::Function::ExternalLinkage, "fclose", module);
        llvm::Value* filePtr = nullptr;
        // Check local variable first
        if (vars.count(node.varName)) {
            llvm::AllocaInst* fileAlloca = vars[node.varName];
            filePtr = builder.CreateLoad(builder.getPtrTy(), fileAlloca);
        }
        // Check global variable
        else if (globals.count(node.varName)) {
            llvm::GlobalVariable* gv = globals[node.varName];
            filePtr = builder.CreateLoad(gv->getValueType(), gv);
        }
        if (filePtr) {
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
    else if (node.type == NODE_SWITCH) {
        llvm::Value* value = genExpr(*node.left);
        llvm::Function* func = builder.GetInsertBlock()->getParent();
        const ScopeSnapshot parentScope = saveScope();
        const auto savedBreak = breakBlock;
        const auto savedContinue = continueBlock;
        llvm::BasicBlock* afterBlock = llvm::BasicBlock::Create(context, "switch_after", func);
        breakBlock = afterBlock;
        continueBlock = afterBlock;
        std::vector<llvm::BasicBlock*> decisionBlocks;
        std::vector<llvm::BasicBlock*> bodyBlocks;
        for (size_t i = 0; i < node.children.size(); ++i) {
            decisionBlocks.push_back(llvm::BasicBlock::Create(context, "case_cmp", func));
            bodyBlocks.push_back(llvm::BasicBlock::Create(context, "case_body", func));
        }
        llvm::BasicBlock* defaultBlock =
            llvm::BasicBlock::Create(context, "switch_default", func);
        builder.CreateBr(decisionBlocks.empty() ? defaultBlock : decisionBlocks[0]);
        for (size_t i = 0; i < decisionBlocks.size(); ++i) {
            builder.SetInsertPoint(decisionBlocks[i]);
            const Node& caseNode = node.children[i];
            if (caseNode.left) {
                llvm::Value* caseVal = genExpr(*caseNode.left);
                llvm::Value* match = builder.CreateICmpEQ(value, caseVal);
                llvm::BasicBlock* next = (i + 1 < decisionBlocks.size())
                    ? decisionBlocks[i + 1] : defaultBlock;
                builder.CreateCondBr(match, bodyBlocks[i], next);
            } else {
                builder.CreateBr(bodyBlocks[i]);
            }
        }
        builder.SetInsertPoint(defaultBlock);
        for (const Node& caseNode : node.children) {
            if (!caseNode.left) {
                for (const Node& child : caseNode.children) genNode(child);
                break;
            }
        }
        if (!blockHasTerminator(builder.GetInsertBlock()))
            builder.CreateBr(afterBlock);
        for (size_t i = 0; i < bodyBlocks.size(); ++i) {
            builder.SetInsertPoint(bodyBlocks[i]);
            const Node& caseNode = node.children[i];
            for (const Node& child : caseNode.children) genNode(child);
            if (!blockHasTerminator(builder.GetInsertBlock()))
                builder.CreateBr(afterBlock);
        }
        builder.SetInsertPoint(afterBlock);
        restoreScope(parentScope);
        breakBlock = savedBreak;
        continueBlock = savedContinue;
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
        if (!arrays.count(node.left->value) && !globals.count(node.left->value))
            codegenFailure(node, "неизвестный массив '" + node.left->value + "'");
        llvm::Value* array = globals.count(node.left->value)
            ? static_cast<llvm::Value*>(globals[node.left->value])
            : static_cast<llvm::Value*>(arrays[node.left->value]);
        const unsigned length = arrayLengths[node.left->value];
        llvm::Type* elementType = arrayElementTypes[node.left->value];
        if (!elementType) elementType = builder.getInt32Ty();
        llvm::ArrayType* arrayType = llvm::ArrayType::get(elementType, length);
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

// ---------------------------------------------------------------------------
// DWARF debug info
// ---------------------------------------------------------------------------
// Debug info is emitted only when the SC_DEBUG environment variable is set,
// so production builds pay no cost. The implementation is intentionally
// lightweight: it records a compile unit, per-function subprograms, local
// variables, and the basic SC types. It does not attempt to track every
// instruction location, so the debug info is useful for source mapping but
// not for full IDE-level stepping.
// ---------------------------------------------------------------------------

namespace {
    /// Returns a stable DWARF encoding for an SC primitive type.
    /// Returns 0 for non-primitive types (structs, arrays, pointers).
    unsigned dwarfEncoding(const std::string& scType) {
        if (scType == "bool") return 0x02;       // DW_ATE_boolean
        if (scType == "char") return 0x06;       // DW_ATE_signed_char
        if (scType == "i8" || scType == "i16" || scType == "i32" || scType == "i64")
            return 0x05;                          // DW_ATE_signed
        if (scType == "u8" || scType == "u16" || scType == "u32" || scType == "u64")
            return 0x07;                          // DW_ATE_unsigned
        if (scType == "f32" || scType == "f64")
            return 0x04;                          // DW_ATE_float
        if (scType == "str" || scType == "char8" || scType == "char16" ||
            scType == "char32")
            return 0x10;                          // DW_ATE_UTF
        return 0;
    }

    /// Bit width for an SC primitive type. Returns 0 for non-primitives.
    unsigned dwarfBitWidth(const std::string& scType) {
        if (scType == "bool") return 1;
        if (scType == "char") return 8;
        if (scType == "i8" || scType == "u8") return 8;
        if (scType == "i16" || scType == "u16") return 16;
        if (scType == "i32" || scType == "u32") return 32;
        if (scType == "i64" || scType == "u64") return 64;
        if (scType == "f32") return 32;
        if (scType == "f64") return 64;
        return 0;
    }
}

llvm::DIType* CodeGen::dwarfType(const std::string& scType) {
    if (!debugBuilder) return nullptr;
    unsigned enc = dwarfEncoding(scType);
    if (enc == 0) return nullptr;
    unsigned bits = dwarfBitWidth(scType);
    if (bits == 0) return nullptr;
    // Canonicalize type aliases to their primitive names for DWARF.
    std::string name = scType;
    if (name == "rox" || name == "int") name = "i32";
    else if (name == "dor" || name == "float") name = "f32";
    else if (name == "cos" || name == "string") name = "str";
    return debugBuilder->createBasicType(name, bits, enc);
}

void CodeGen::emitDebugInfo() {
    if (!getenv("SC_DEBUG")) return;
    debugBuilder = std::make_unique<llvm::DIBuilder>(module);

    llvm::DIFile* cuFile = debugBuilder->createFile(
        module.getName(), ".");
    llvm::DICompileUnit* cu = debugBuilder->createCompileUnit(
        llvm::DISourceLanguageName(llvm::dwarf::DW_LANG_C), cuFile,
        "SC compiler", false, "", 0);

    for (auto& [mangled, func] : funcs) {
        if (func->isDeclaration()) continue;
        std::vector<llvm::Metadata*> paramMetas;
        const auto& ptypes = funcParamTypes[mangled];
        for (size_t i = 0; i < func->arg_size() && i < ptypes.size(); ++i) {
            paramMetas.push_back(dwarfType(ptypes[i]));
        }
        llvm::DITypeArray paramArray = debugBuilder->getOrCreateTypeArray(paramMetas);
        llvm::DISubroutineType* subTy =
            debugBuilder->createSubroutineType(paramArray);
        llvm::DISubprogram* sp = debugBuilder->createFunction(
            cu, mangled, mangled, cuFile, 1, subTy, 1,
            llvm::DINode::FlagZero, llvm::DISubprogram::SPFlagDefinition);
        func->setSubprogram(sp);
    }

    debugBuilder->finalize();
}

bool CodeGen::configureTarget() {
    std::string triple = llvm::sys::getDefaultTargetTriple();
    module.setTargetTriple(llvm::Triple(triple));
    llvm::Triple t(triple);
    std::string err;
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(t, err);
    if (!target) return false;
    // Derive the data layout string from the triple (avoids creating a
    // TargetMachine whose internal subtarget objects are not safe to delete
    // under this LLVM build's linked runtime).
    module.setDataLayout(t.computeDataLayout());
    return true;
}

void CodeGen::generate(const Node& program) {
    // Runtime panic function (internal, not user-callable).
    llvm::FunctionType* panicType =
        llvm::FunctionType::get(builder.getVoidTy(),
            {builder.getPtrTy()}, false);
    llvm::Function::Create(panicType,
        llvm::Function::ExternalLinkage, "__sc_panic", module);

    llvm::FunctionType* putsType =
        llvm::FunctionType::get(builder.getInt32Ty(),
            {builder.getPtrTy()}, false);
    putsFunc = llvm::Function::Create(putsType,
        llvm::Function::ExternalLinkage, "puts", module);

    llvm::FunctionType* printfType =
        llvm::FunctionType::get(builder.getInt32Ty(),
            {builder.getPtrTy()}, true);
    printfFunc = llvm::Function::Create(printfType,
        llvm::Function::ExternalLinkage, "printf", module);

    // First pass: register functions and GLOBAL VARIABLES (so they're available in function bodies)
    for (const Node& node : program.children) {
        if (node.type != NODE_FUNC_DEF && node.type != NODE_STRUCT_DEF) {
            // Global variable declaration - create the GlobalVariable now
            if (node.type == NODE_VAR_DECL || node.type == NODE_ARRAY_DECL) {
                genNode(node);
            }
        }
        if (node.type != NODE_FUNC_DEF) continue;
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
        std::string mangled = mangleName(node.varName, parameterNames);
        funcParamTypes[mangled] = parameterNames;
        funcReturnTypes[mangled] = returnType;
        llvm::FunctionType* functionType = llvm::FunctionType::get(
            svType(returnType), parameterTypes, false);
        funcs[mangled] = llvm::Function::Create(
            functionType, llvm::Function::ExternalLinkage, mangled, module);
    }

    for (const Node& node : program.children)
        if (node.type == NODE_FUNC_DEF) genNode(node);

    llvm::FunctionType* funcType =
        llvm::FunctionType::get(builder.getInt32Ty(), false);
    llvm::Function* mainFunc =
        llvm::Function::Create(funcType,
            llvm::Function::ExternalLinkage, "main", module);
    llvm::BasicBlock* block =
        llvm::BasicBlock::Create(context, "entry", mainFunc);
    builder.SetInsertPoint(block);

    // Emit deferred global initializers (globals with non-constant initializers).
    for (const Node& node : globalDeferred) {
        llvm::GlobalVariable* gv = globals[node.varName];
        if (node.type == NODE_ARRAY_DECL) {
            unsigned n = (unsigned)node.args.size();
            llvm::Type* elementType = arrayElementTypes[node.varName];
            if (!elementType) elementType = builder.getInt32Ty();
            llvm::ArrayType* at = llvm::ArrayType::get(elementType, n);
            for (unsigned i = 0; i < n; i++) {
                llvm::Value* gep = builder.CreateInBoundsGEP(
                    at, gv, {builder.getInt32(0), builder.getInt32(i)});
                llvm::Value* val = genExpr(node.args[i]);
                val = castTo(val, elementType, isUnsignedExpr(node.args[i]));
                builder.CreateStore(val, gep);
            }
        } else {
            llvm::Value* val = genExpr(*node.left);
            val = castTo(val, gv->getValueType(), isUnsignedExpr(*node.left));
            builder.CreateStore(val, gv);
        }
    }

    for (const Node& node : program.children)
        if (node.type != NODE_FUNC_DEF && node.type != NODE_VAR_DECL && node.type != NODE_ARRAY_DECL) genNode(node);

    // If the user defined a main() function, it was emitted as the mangled
    // 'main__'. Call it and return its value so the entry point reflects the
    // user's program. Otherwise fall back to a plain zero return.
    if (funcs.count("main__")) {
        llvm::Value* result = builder.CreateCall(funcs["main__"]);
        if (result->getType()->isIntegerTy() && result->getType()->getIntegerBitWidth() < 32)
            result = builder.CreateZExt(result, builder.getInt32Ty());
        builder.CreateRet(result);
    } else {
        builder.CreateRet(builder.getInt32(0));
    }
}

bool CodeGen::verify(std::string& diagnostic) const {
    llvm::raw_string_ostream output(diagnostic);
    const bool invalid = llvm::verifyModule(module, &output);
    output.flush();
    return !invalid;
}

void CodeGen::runOptimizations(llvm::OptimizationLevel level) {
    if (level == llvm::OptimizationLevel::O0) return;
    llvm::PassBuilder pb;
    llvm::ModuleAnalysisManager mam;
    llvm::FunctionAnalysisManager fam;
    llvm::LoopAnalysisManager lam;
    llvm::CGSCCAnalysisManager cgam;
    pb.registerFunctionAnalyses(fam);
    pb.registerLoopAnalyses(lam);
    pb.registerCGSCCAnalyses(cgam);
    pb.registerModuleAnalyses(mam);
    pb.crossRegisterProxies(lam, fam, cgam, mam);
    llvm::ModulePassManager mp = pb.buildPerModuleDefaultPipeline(level);
    mp.run(module, mam);
}

void CodeGen::print() {
    module.print(llvm::outs(), nullptr);
}
