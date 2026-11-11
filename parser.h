#ifndef PARSER_H
#define PARSER_H

#include "lexer.h"
#include <memory>
#include <vector>

enum NodeType {
    NODE_VAR_DECL,
    NODE_NUMBER,
    NODE_CHAR,
    NODE_STRING,
    NODE_IDENTIFIER,
    NODE_SLOV,
    NODE_BINOP,
    NODE_PROGRAM,
    NODE_IF,
    NODE_LOOP_N,
    NODE_ASSIGN,
    NODE_FUNC_DEF,
    NODE_FUNC_CALL,
    NODE_STRUCT_DEF,
    NODE_STRUCT_INSTANCE,
    NODE_STRUCT_FIELD,
    NODE_ARG,
    NODE_RETURN,
    NODE_ARRAY_DECL,
    NODE_ARRAY_ACCESS,
    NODE_ADDR,
    NODE_DEREF,
    NODE_DEREF_ASSIGN,
    NODE_ARRAY_ASSIGN,
    NODE_DYN_ARRAY,
    NODE_DELETE,
    NODE_BREAK,
    NODE_CONTINUE,
    NODE_IMPORT,
    NODE_CAST,
    NODE_FILE_OPEN,
    NODE_INPUT,
    NODE_STRLEN,
    NODE_STRCAT,
    NODE_FILE_WRITE,
    NODE_FILE_READ,
    NODE_FILE_CLOSE,
    NODE_LOOP_W,
    NODE_FOR_RANGE,
    NODE_WHILE,
    NODE_LOOP,
    NODE_TERNARY,
    NODE_FOR_EACH,
    NODE_STRUCT_FIELD_ASSIGN,
    NODE_STRUCT_LITERAL,
    NODE_ARRAY_PUSH,
    NODE_ARRAY_POP,
    NODE_ARRAY_RESIZE,
    NODE_ARRAY_LEN,
    NODE_ARRAY_CAP,
    NODE_SLICE,
};

struct Node {
    NodeType type;
    std::string value;
    mutable std::string varType;
    std::string varName;
    bool isConst = false;
    bool inclusiveRange = false;
    bool isForwardDecl = false;  // fn T name(...); без тела
    std::string op;
    int line = 1;
    int column = 1;
    std::string file;
    Node* left = nullptr;
    Node* right = nullptr;
    Node* extra = nullptr;
    std::vector<Node> children;
    std::vector<Node> elseChildren;
    std::vector<Node> args;
    std::vector<std::string> params;
    std::shared_ptr<std::vector<std::unique_ptr<Node>>> arena;
};

Node parse(const std::vector<Token>& tokens);
bool parseHadErrors();
bool checkSemantics(const Node& program, const std::string& filename);

#endif
