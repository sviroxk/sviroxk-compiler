#ifndef CODEGEN_H
#define CODEGEN_H

#include "parser.h"
#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Passes/OptimizationLevel.h>
#include <map>
#include <string>

class CodeGen {
    struct ScopeSnapshot {
        std::map<std::string, llvm::AllocaInst*> vars;
        std::map<std::string, bool> unsignedVars;
        std::map<std::string, bool> charVars;
        std::map<std::string, bool> constVars;
        std::map<std::string, unsigned> integerWidths;
        std::map<std::string, llvm::AllocaInst*> arrays;
        std::map<std::string, unsigned> arrayLengths;
        std::map<std::string, llvm::Type*> arrayElementTypes;
        std::map<std::string, std::string> structTypesByVariable;
        std::vector<Node> deferred;
    };

    ScopeSnapshot saveScope() const;
    void restoreScope(const ScopeSnapshot& scope);

public:
    llvm::LLVMContext context;
    llvm::IRBuilder<> builder;
    llvm::Module module;
    std::map<std::string, llvm::AllocaInst*> vars;
    std::map<std::string, bool> unsignedVars;
    std::map<std::string, bool> charVars;
    std::map<std::string, bool> constVars;
    std::map<std::string, unsigned> integerWidths;
    std::map<std::string, llvm::AllocaInst*> arrays;
    std::map<std::string, unsigned> arrayLengths;
    std::map<std::string, llvm::Type*> arrayElementTypes;
    std::map<std::string, llvm::GlobalVariable*> globals;
    std::map<std::string, bool> globalIsConst;
    std::vector<Node> globalDeferred;
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
    std::vector<Node> deferred;
    std::unique_ptr<llvm::DIBuilder> debugBuilder;

    CodeGen() : builder(context), module("SC", context) {}

    // Configure the module's target triple and data layout from the host.
    // Returns false if no registered target matches the host triple.
    bool configureTarget();

    // Emit DWARF debug info for the module (compile unit, files, functions,
    // variables, basic types). No-op when SC_DEBUG is not defined.
    void emitDebugInfo();

    // Map an SC type name to its DWARF type, creating basic types on demand.
    llvm::DIType* dwarfType(const std::string& scType);

    llvm::Value* getStringPtr(const std::string& str);

    llvm::Function* getPanicFunction();
    void emitPanic(const std::string& message);

    llvm::Type* svType(const std::string& name);
    llvm::Value* castTo(llvm::Value* val, llvm::Type* ty, bool sourceUnsigned = false);
    llvm::Value* toBool(llvm::Value* val);
    void emitArrayBoundsCheck(llvm::Value* index, unsigned length, const Node& location);
    bool isUnsignedExpr(const Node& node) const;
    unsigned integerWidth(const Node& node) const;
    llvm::Value* genExpr(const Node& node);
    void genNode(const Node& node);
    static bool isConstantInitializer(const Node& node);
    // Generate an aggregate value (array literal or struct literal, or copy from
    // another aggregate variable) into an already-allocated alloca.
    void emitAggregateInto(llvm::AllocaInst* dst, const Node& src);
    void emitDeferred();
    void generate(const Node& program);
    bool verify(std::string& diagnostic) const;
    void runOptimizations(llvm::OptimizationLevel level);
    void print();
};

#endif
