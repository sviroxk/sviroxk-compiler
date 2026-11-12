#ifndef CODEGEN_H
#define CODEGEN_H

#include "parser.h"
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <map>
#include <unordered_map>
#include <string>

class CodeGen {
    struct ScopeSnapshot {
        std::map<std::string, llvm::Value*> vars;
        std::map<std::string, bool> unsignedVars;
        std::map<std::string, bool> charVars;
        std::map<std::string, bool> constVars;
        std::map<std::string, unsigned> integerWidths;
        std::map<std::string, llvm::Value*> arrays;
        std::map<std::string, unsigned> arrayLengths;
        std::map<std::string, llvm::Type*> arrayElementTypes;
        std::map<std::string, std::string> structTypesByVariable;
        std::map<std::string, llvm::Value*> dynArrays;
        std::map<std::string, llvm::Type*> dynArrayElementTypes;
        std::map<std::string, llvm::Type*> pointeeTypes;
    };

    ScopeSnapshot saveScope() const;
    void restoreScope(const ScopeSnapshot& scope);
    struct Deferred { const Node* statement; ScopeSnapshot bindings; };
    std::vector<Deferred> deferred_;
    size_t loopDeferredBase_ = 0;
    void emitDeferred(size_t base);
    void genScope(const std::vector<Node>& statements);

public:
    llvm::LLVMContext context;
    llvm::IRBuilder<> builder;
    llvm::Module module;
    std::map<std::string, llvm::Value*> vars;
    std::map<std::string, bool> unsignedVars;
    std::map<std::string, bool> charVars;
    std::map<std::string, bool> constVars;
    std::map<std::string, unsigned> integerWidths;
    std::map<std::string, llvm::Value*> arrays;
    std::map<std::string, unsigned> arrayLengths;
    std::map<std::string, llvm::Type*> arrayElementTypes;
    std::map<std::string, llvm::Value*> dynArrays;
    std::map<std::string, llvm::Type*> dynArrayElementTypes;
    // Тип значения, на которое указывает ptr-переменная (известен для ptr p = &x).
    std::map<std::string, llvm::Type*> pointeeTypes;
    std::map<std::string, llvm::Function*> funcs;
    std::map<std::string, std::vector<std::string>> funcParamTypes;
    std::map<std::string, std::string> funcReturnTypes;
    struct StructField {
        std::string type;
        std::string name;
        Node defaultVal;
    };
    std::map<std::string, std::vector<StructField>> structDefs;
    std::map<std::string, llvm::StructType*> llvmStructTypes;
    std::map<std::string, std::map<std::string, unsigned>> structFieldIndices;
    std::map<std::string, std::string> structTypesByVariable;
    llvm::BasicBlock* breakBlock = nullptr;
    llvm::BasicBlock* continueBlock = nullptr;
    llvm::Function* putsFunc = nullptr;
    llvm::Function* printfFunc = nullptr;
    llvm::Function* abortFunc = nullptr;
    // true, пока генерируется код верхнего уровня: переменные становятся глобалами.
    bool globalStorage = false;
    // Готовые значения узлов (индекс в a[i] += x вычисляется один раз).
    std::map<const Node*, llvm::Value*> precomputed_;
    bool structFieldAddressOnly_ = false;
    std::map<std::string, bool> unsignedElements;  // массив -> беззнаковые элементы
    bool fieldIsUnsigned(const std::string& variable, const std::string& path) const;
    // Запись в более узкий целый тип (x += 1 для u8): значение вне диапазона — ошибка выполнения.
    void checkNarrowStore(llvm::Value* value, const Node& valueNode, llvm::Type* target,
                          bool targetUnsigned, const Node& location);  // NODE_STRUCT_FIELD: вернуть адрес поля, не загружать
    llvm::Type* lastFieldType_ = nullptr;
    llvm::Value* nullChecked(llvm::Value* pointer, const Node& location);

    CodeGen() : builder(context), module("SC", context) {}

    llvm::Value* getStringPtr(const std::string& str);

    llvm::Type* svType(const std::string& name);
    static llvm::Type* storageType(llvm::Value* storage);
    llvm::Value* createStorage(llvm::Type* type, const std::string& name);
    llvm::Function* runtimeFunction(const std::string& name);
    llvm::Value* castTo(llvm::Value* val, llvm::Type* ty, bool sourceUnsigned = false);
    llvm::Value* toBool(llvm::Value* val);
    void emitRuntimeFailureIf(llvm::Value* failed, const std::string& message,
                              const Node& location, llvm::Value* detail = nullptr);
    llvm::Value* allocateStringBuffer(llvm::Value* bytes, const Node& location);
    llvm::Function* readLineFunction();
    llvm::AllocaInst* entryAlloca(llvm::Type* type, const char* name);
    llvm::Value* emitReadLine(llvm::Value* file, const Node& location, bool eofIsError);
    void emitArrayBoundsCheck(llvm::Value* index, llvm::Value* length, const Node& location);
    llvm::StructType* dynArrayType();
    void growDynArray(const std::string& varName, llvm::Value* newCapacity, const Node& location);
    llvm::Value* emitArraySize(const Node& sizeExpr, const Node& location);
    bool isUnsignedExpr(const Node& node) const;
    unsigned integerWidth(const Node& node) const;
    // Кэш свойств поддеревьев (ключ — узел AST в куче, left/right) на время одного
    // оператора: без него каждый `+` цепочки `a + b + …` обходил бы всё левое
    // поддерево (O(n²)). Сбрасывается в genNode: объявления меняют таблицы переменных.
    mutable std::unordered_map<const Node*, bool> unsignedCache_;
    mutable std::unordered_map<const Node*, unsigned> widthCache_;
    bool cachedUnsigned(const std::shared_ptr<Node>& child) const;
    unsigned cachedWidth(const std::shared_ptr<Node>& child) const;
    void initStructLiteral(llvm::Value* storage, llvm::StructType* aggregateType,
                           const std::string& structName, const Node& literal);
    llvm::Value* genExpr(const Node& node);
    void genNode(const Node& node);
    void generate(const Node& program);
    bool verify(std::string& diagnostic) const;
    void print();
};

#endif
