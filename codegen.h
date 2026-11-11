#ifndef CODEGEN_H
#define CODEGEN_H

#include "parser.h"
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <map>
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
    };

    ScopeSnapshot saveScope() const;
    void restoreScope(const ScopeSnapshot& scope);

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
    void emitArrayBoundsCheck(llvm::Value* index, llvm::Value* length, const Node& location);
    llvm::StructType* dynArrayType();
    void growDynArray(const std::string& varName, llvm::Value* newCapacity);
    bool isUnsignedExpr(const Node& node) const;
    unsigned integerWidth(const Node& node) const;
    void initStructLiteral(llvm::Value* storage, llvm::StructType* aggregateType,
                           const std::string& structName, const Node& literal);
    llvm::Value* genExpr(const Node& node);
    void genNode(const Node& node);
    void generate(const Node& program);
    bool verify(std::string& diagnostic) const;
    void print();
};

#endif
