#ifndef CODEGEN_H
#define CODEGEN_H

#include "parser.h"
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <map>
#include <set>
#include <unordered_map>
#include <string>
#include <string_view>

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
    // statement == nullptr: освободить строку из слота freeSlot (D-014, конец блока).
    // temporary — временная строка выражения temporary (освобождается в конце оператора).
    struct Deferred {
        const Node* statement; ScopeSnapshot bindings; llvm::Value* freeSlot = nullptr;
        const Node* temporary = nullptr;
    };
    std::vector<Deferred> deferred_;
    size_t loopDeferredBase_ = 0;
    void emitDeferred(size_t base);
    void genScope(const std::vector<Node>& statements);
    // D-014: string-переменная владеет своей копией в куче и освобождает её в конце блока.
    std::set<const llvm::Value*> ownedStringSlots_;
    bool isOwnedString(const std::string& name) const;
    bool isFreshString(const Node& expr) const;
    llvm::Value* copyString(llvm::Value* value);
    llvm::Value* ownString(const Node& expr, llvm::Value* value);
    llvm::Value* stableString(const Node& expr, llvm::Value* value);
    void ownStringSlot(llvm::Value* slot);
    void registerTemporary(const Node& expr, llvm::Value* value);
    void consumeTemporary(const Node& expr);
    void releaseTemporaries(size_t mark);
    void genStatement(const Node& statement);

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
    bool fieldIsUnsigned(const std::string& variable, std::string_view path) const;
    // Запись в более узкий целый тип (x += 1 для u8): значение вне диапазона — ошибка выполнения.
    void checkNarrowStore(llvm::Value* value, const Node& valueNode, llvm::Type* target,
                          bool targetUnsigned, const Node& location);  // NODE_STRUCT_FIELD: вернуть адрес поля, не загружать
    llvm::Type* lastFieldType_ = nullptr;
    llvm::Value* nullChecked(llvm::Value* pointer, const Node& location);

    CodeGen() : builder(context), module("SC", context) {}

    llvm::Value* getStringPtr(llvm::StringRef str);

    llvm::Type* svType(const std::string& name);
    static llvm::Type* storageType(llvm::Value* storage);
    llvm::Value* createStorage(llvm::Type* type, const std::string& name);
    llvm::Function* runtimeFunction(std::string_view name);
    llvm::Value* castTo(llvm::Value* val, llvm::Type* ty, bool sourceUnsigned = false);
    llvm::Value* toBool(llvm::Value* val);
    void emitRuntimeFailureIf(llvm::Value* failed, std::string_view message,
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
    // Не встраиваются: иначе большой кадр genOtherExpr попадёт в каждый уровень
    // рекурсии по цепочке операторов (предел длины выражения, spec §1).
    [[gnu::noinline]] llvm::Value* genBinary(const Node& node, llvm::Value* left);
    [[gnu::noinline]] llvm::Value* genOtherExpr(const Node& node);
    [[gnu::noinline]] llvm::Value* genCallExpr(const Node& node);
    void genNode(const Node& node);
    void generate(const Node& program);
    bool verify(std::string& diagnostic) const;
    void print();
};

#endif
