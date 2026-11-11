#include "parser.h"
#include "diagnostics.h"
#include <iostream>
#include <sstream>
#include <cctype>
#include <cstdlib>
#include <memory>
#include <utility>

static int pos = 0;
static unsigned parseErrorCount = 0;
static int lastErrorPos = 0;
static bool recovering = false;
// Защита от зависаний: после kMaxSyntaxErrors сообщений или kMaxFailCalls
// попыток сообщить об ошибке разбор останавливается (позиция переносится в END).
static const unsigned kMaxSyntaxErrors = 20;
static const unsigned kMaxFailCalls = 10000;
static unsigned failCalls = 0;
static bool parseStopped = false;
static std::vector<Token> gtokens;
static std::shared_ptr<std::vector<std::unique_ptr<Node>>> nodeArena =
    std::make_shared<std::vector<std::unique_ptr<Node>>>();

template <typename... Args>
static Node* makeNode(Args&&... args) {
    auto node = std::make_unique<Node>(std::forward<Args>(args)...);
    Node* result = node.get();
    nodeArena->push_back(std::move(node));
    return result;
}

static Token peek(size_t offset = 0) {
    if (gtokens.empty()) return {END, "", 1, 1, {}};
    size_t index = static_cast<size_t>(pos) + offset;
    if (index >= gtokens.size()) return gtokens.back();
    return gtokens[index];
}

static Token consume() {
    Token token = peek();
    if (token.type != END && pos < static_cast<int>(gtokens.size())) ++pos;
    return token;
}

static std::string tokenDescription(const Token& token) {
    if (token.type == END) return "конец файла";
    return "'" + token.value + "'";
}

static void stopParsing() {
    parseStopped = true;
    if (!gtokens.empty()) pos = static_cast<int>(gtokens.size()) - 1;
}

static void failAt(const Token& token, const std::string& message) {
    if (parseStopped) return;
    if (++failCalls > kMaxFailCalls) {
        stopParsing();
        return;
    }
    if (!recovering) {
        ++parseErrorCount;
        lastErrorPos = pos;
        if (pos > 0) {
            const Token& previous = gtokens[static_cast<size_t>(pos - 1)];
            if (previous.file == token.file && previous.line == token.line &&
                previous.column == token.column && previous.type == token.type)
                lastErrorPos = pos - 1;
        }
        sc::reportDiagnostic(token.file, token.line, token.column, "syntax", message);
        if (parseErrorCount >= kMaxSyntaxErrors) {
            sc::reportDiagnostic(token.file, token.line, token.column, "syntax",
                                 "слишком много синтаксических ошибок, разбор остановлен");
            stopParsing();
        }
    }
    recovering = true;
}

// Пропускает токены до конца ошибочной конструкции с учётом вложенных `{ }`:
// останавливается после `;` на нулевой глубине, после `}`, закрывающего
// открытый здесь блок, или перед `}`, принадлежащей внешнему блоку.
static void synchronizeStatement() {
    int depth = 0;
    while (peek().type != END) {
        TokenType type = peek().type;
        if (type == LBRACE) {
            ++depth;
        } else if (type == RBRACE) {
            if (depth == 0) break;
            --depth;
            if (depth == 0) {
                consume();
                if (peek().type == SEMICOLON) consume();
                break;
            }
        } else if (type == SEMICOLON && depth == 0) {
            consume();
            break;
        }
        consume();
    }
    recovering = false;
}

// Восстановление после ошибки в parse()/parseBlock(): вернуться к месту первой
// ошибки (но не раньше начала конструкции), пропустить конструкцию и
// гарантировать, что позиция сдвинулась хотя бы на один токен.
static void recoverAfterError(int start, bool insideBlock) {
    if (parseStopped) return;
    if (recovering) {
        pos = lastErrorPos < start ? start : lastErrorPos;
        synchronizeStatement();
    }
    recovering = false;
    if (pos <= start && peek().type != END && !(insideBlock && peek().type == RBRACE))
        consume();
}

static Token expect(TokenType type, const std::string& expected) {
    Token token = peek();
    if (token.type != type) {
        failAt(token, "ожидалось " + expected + ", получено " + tokenDescription(token));
    }
    return consume();
}

static Token expect(TokenType type) {
    return expect(type, "другой токен");
}

static bool isTypeName(const std::string& type) {
    return type == "rox" || type == "dor" || type == "cos" || type == "bue" ||
           type == "int" || type == "float" || type == "string" || type == "str" || type == "bool" ||
           type == "uint" || type == "short" || type == "ushort" || type == "long" ||
           type == "ulong" || type == "byte" || type == "double" || type == "char" ||
           type == "i8" || type == "i16" || type == "i32" || type == "i64" ||
           type == "u8" || type == "u16" || type == "u32" || type == "u64" ||
           type == "f32" || type == "f64" || type == "ptr" || type == "file" ||
           type == "chr" || type == "rox64";
}

static bool isBuiltinToken(TokenType t) {
    return t == SQRT || t == ABS || t == POW || t == MAX || t == MIN ||
           t == FLO || t == CEL || t == RON || t == RAN || t == AR ||
           t == SER || t == REP || t == STR || t == DEL || t == PRO ||
           t == TIP || t == FILE_T || t == PTR;
}

static bool isNameToken(TokenType t) {
    return t == IDENTIFIER || isBuiltinToken(t);
}

static bool isCallableBuiltinToken(TokenType t) {
    return t == SQRT || t == ABS || t == POW || t == MAX || t == MIN ||
           t == FLO || t == CEL || t == RON || t == RAN || t == AR ||
           t == SER || t == REP || t == STR;
}

static bool isTypeToken(TokenType t) {
    return t == ROX || t == DOR || t == COS || t == BUE || t == PTR ||
           t == FILE_T || t == CHR || t == ROX64 || t == FIXED_TYPE || t == IDENTIFIER;
}

static std::string canonicalType(const std::string& type) {
    if (type == "int" || type == "i32" || type == "rox") return "i32";
    if (type == "uint" || type == "u32") return "u32";
    if (type == "short" || type == "i16") return "i16";
    if (type == "ushort" || type == "u16") return "u16";
    if (type == "long" || type == "i64" || type == "rox64") return "i64";
    if (type == "ulong" || type == "u64") return "u64";
    if (type == "byte" || type == "u8") return "u8";
    if (type == "char" || type == "chr") return "char";
    if (type == "float" || type == "f32" || type == "dor") return "f32";
    if (type == "double" || type == "f64") return "f64";
    if (type == "string" || type == "str" || type == "cos") return "str";
    if (type == "bool" || type == "bue") return "bool";
    return type;
}

Node parseExpr();
Node parsePrimary();
Node parseUnary();
Node parseStructLiteral();
Node parseMul();
Node parseAdd();
std::vector<Node> parseBlock();

Node parseStructLiteral() {
    Node node; node.type = NODE_STRUCT_LITERAL;
    expect(LBRACE);
    while (peek().type != RBRACE && peek().type != END) {
        Node init;
        if (isNameToken(peek().type) && peek(1).type == EQUALS) {
            init.varName = consume().value;
            consume(); // =
            init.left = makeNode(parseExpr());
        } else {
            init.left = makeNode(parseExpr());
        }
        node.children.push_back(init);
        if (peek().type == COMMA || peek().type == SEMICOLON)
            consume();
    }
    expect(RBRACE);
    return node;
}

Node parsePrimary() {
    Node left;
    // (dor)x — приведение типов
    if (peek().type == LPAREN && isTypeName(peek(1).value)) {
        consume(); // (
        Token castToken = consume();
        expect(RPAREN, "')'");
        left.type = NODE_CAST;
        left.varType = canonicalType(castToken.value);
        left.line = castToken.line;
        left.column = castToken.column;
        left.file = castToken.file;
        left.left = makeNode(parseUnary());
        return left;
    }
    if (peek().type == LPAREN) {
        consume();
        Node grouped = parseExpr();
        expect(RPAREN, "')'");
        return grouped;
    }
    if (peek().type == TRUE_T) {
        Token token = consume();
        left.type = NODE_NUMBER; left.value = "1";
        left.varType = "bue";
        left.line = token.line; left.column = token.column; left.file = token.file;
        return left;
    }
    if (peek().type == FALSE_T) {
        Token token = consume();
        left.type = NODE_NUMBER; left.value = "0";
        left.varType = "bue";
        left.line = token.line; left.column = token.column;
        return left;
    }
    // математика
    if (isCallableBuiltinToken(peek().type) && peek(1).type == LPAREN) {
        Node left;
        left.type = NODE_FUNC_CALL;
        Token builtinToken = consume();
        left.varName = builtinToken.value;
        left.line = builtinToken.line;
        left.column = builtinToken.column;
        left.file = builtinToken.file;
        expect(LPAREN);
        while (peek().type != RPAREN && peek().type != END) {
            left.args.push_back(parseExpr());
            if (peek().type == COMMA) consume();
        }
        expect(RPAREN);
        return left;
    }
    // Struct literal: { field = value, ... } or { value, ... }
    if (peek().type == LBRACE) {
        return parseStructLiteral();
    }
    // vd — ввод с клавиатуры
    if (peek().type == VD) {
        consume();
        Node left; left.type = NODE_INPUT;
        expect(LPAREN);
        left.value = consume().value;
        expect(RPAREN);
        return left;
    }
    // vv_s — ввод строки (в выражении)
    if (peek().type == VV_S) {
        consume();
        Node left; left.type = NODE_INPUT;
        left.varType = "cos";
        expect(LPAREN);
        left.value = consume().value;
        expect(RPAREN);
        return left;
    }
    // nw — создать динамический массив
    if (peek().type == NW) {
        consume();
        Node left; left.type = NODE_DYN_ARRAY;
        left.varType = consume().value; // тип (rox etc)
        expect(LBRACKET);
        left.left = makeNode(parseExpr()); // размер
        expect(RBRACKET);
        return left;
    }
    // atk — открыть файл
    if (peek().type == ATK) {
        Token token = consume();
        Node left; left.type = NODE_FILE_OPEN;
        left.line = token.line;
        left.column = token.column;
        expect(LPAREN);
        left.left = makeNode(parseExpr());
        if (peek().type == COMMA) {
            consume();
            left.right = makeNode(parseExpr());
        }
        expect(RPAREN);
        return left;
    }
    // pro — прочитать файл
    if (peek().type == PRO) {
        consume();
        Node left; left.type = NODE_FILE_READ;
        expect(LPAREN);
        left.varName = consume().value;
        expect(RPAREN);
        return left;
    }
    // *p — разыменование
    if (peek().type == STAR) {
        consume();
        Token name = consume();
        left.type = NODE_DEREF;
        left.varName = name.value;
        return left;
    }
    Token t = consume();
    left.line = t.line;
    left.column = t.column;
    left.file = t.file;
    if (t.type == NUMBER) {
        left.type = NODE_NUMBER;
        left.value = t.value;
        left.varType.clear();
    }
    else if (t.type == CHAR_LITERAL) {
        left.type = NODE_CHAR;
        left.value = t.value;
        left.varType = "char";
    }
    else if (t.type == STRING) { left.type = NODE_STRING; left.value = t.value; left.varType = "cos"; }
    else if (t.type == COP) {
        left.type = NODE_FUNC_CALL;
        left.varName = consume().value;
        expect(LPAREN);
        while (peek().type != RPAREN && peek().type != END) {
            left.args.push_back(parseExpr());
            if (peek().type == COMMA) consume();
        }
        expect(RPAREN);
    }
    else if (t.type == IDENTIFIER || isBuiltinToken(t.type)) {
        left.type = NODE_IDENTIFIER; left.value = t.value;
        if (peek().type == LPAREN) {
            left.type = NODE_FUNC_CALL;
            left.varName = t.value;
            left.line = t.line; left.column = t.column; left.file = t.file;
            consume();
            while (peek().type != RPAREN && peek().type != END) {
                left.args.push_back(parseExpr());
                if (peek().type == COMMA) consume();
                else if (peek().type != RPAREN)
                    failAt(peek(), "ожидалась ',' или ')' в списке аргументов");
            }
            expect(RPAREN);
        } else if (peek().type == DOT) {
            consume();
            Token field = consume();
            // Array methods: arr.push(x), arr.pop(), arr.resize(n), arr.len(), arr.cap()
            if (field.type == IDENTIFIER && field.value == "push") {
                left.type = NODE_ARRAY_PUSH;
                left.varName = t.value;
                left.line = field.line;
                left.column = field.column;
                left.file = field.file;
                expect(LPAREN);
                left.left = makeNode(parseExpr());
                expect(RPAREN);
            } else if (field.type == IDENTIFIER && field.value == "pop") {
                left.type = NODE_ARRAY_POP;
                left.varName = t.value;
                left.line = field.line;
                left.column = field.column;
                left.file = field.file;
                expect(LPAREN);
                expect(RPAREN);
            } else if (field.type == IDENTIFIER && field.value == "resize") {
                left.type = NODE_ARRAY_RESIZE;
                left.varName = t.value;
                left.line = field.line;
                left.column = field.column;
                left.file = field.file;
                expect(LPAREN);
                left.left = makeNode(parseExpr());
                expect(RPAREN);
            } else if (field.type == IDENTIFIER && field.value == "len") {
                left.type = NODE_ARRAY_LEN;
                left.varName = t.value;
                left.line = field.line;
                left.column = field.column;
                left.file = field.file;
                expect(LPAREN);
                expect(RPAREN);
            } else if (field.type == IDENTIFIER && field.value == "cap") {
                left.type = NODE_ARRAY_CAP;
                left.varName = t.value;
                left.line = field.line;
                left.column = field.column;
                left.file = field.file;
                expect(LPAREN);
                expect(RPAREN);
            } else {
                left.type = NODE_STRUCT_FIELD;
                left.varName = t.value;
                left.op = field.value;
                // Support chained access: a.b.c.d
                while (peek().type == DOT) {
                    consume();
                    Token nextField = consume();
                    if (nextField.type != IDENTIFIER && !isBuiltinToken(nextField.type))
                        failAt(nextField, "ожидалось имя поля после '.'");
                    left.op += "." + nextField.value;
                }
            }
        } else if (peek().type == LBRACKET) {
            consume();
            Node* access = makeNode();
            access->type = NODE_ARRAY_ACCESS;
            access->line = t.line;
            access->column = t.column;
            access->varName = t.value;
            access->left = makeNode(parseExpr());
            expect(RBRACKET);
            left = *access;
        }
    } else {
        failAt(t, "ожидалось выражение, получено " + tokenDescription(t));
    }
    return left;
}

Node parseUnary() {
    if (peek().type == BIT_AND) {
        Token address = consume();
        Token name = peek();
        if (!isNameToken(name.type))
            failAt(name, "после '&' ожидалось имя переменной");
        consume();
        Node node; node.type = NODE_ADDR;
        node.line = address.line;
        node.column = address.column;
        node.file = address.file;
        node.varName = name.value;
        return node;
    }
    if (peek().type == BIT_NOT) {
        Token op = consume();
        Node node; node.type = NODE_BINOP;
        node.line = op.line; node.column = op.column; node.file = op.file;
        node.op = "~";
        node.left = makeNode(parseUnary());
        return node;
    }
    if (peek().type == BANG) {
        Token op = consume();
        Node node; node.type = NODE_BINOP;
        node.line = op.line; node.column = op.column;
        node.op = "!";
        node.left = makeNode(parseUnary());
        node.right = makeNode();
        node.right->type = NODE_NUMBER;
        node.right->value = "0";
        node.right->varType = "bool";
        return node;
    }
    if (peek().type == MINUS) {
        Token minus = consume();
        if (peek().type == NUMBER) {
            Token num = consume();
            Node n;
            n.type = NODE_NUMBER;
            n.line = minus.line;
            n.column = minus.column;
            n.file = minus.file;
            n.value = "-" + num.value;
            return n;
        }
        Node n;
        n.type = NODE_BINOP;
        n.line = minus.line;
        n.column = minus.column;
        n.file = minus.file;
        n.op = "-";
        n.left = makeNode();
        n.left->type = NODE_NUMBER;
        n.left->value = "0";
        n.right = makeNode(parseUnary());
        return n;
    }
    return parsePrimary();
}

// * и / — умножение и деление (+ и - в parseAdd). Символы те же: + - * /
static Node makeBinary(Node left, const Token& op, Node right);

Node parsePower() {
    Node left = parseUnary();
    if (peek().type == POWER) {
        Token op = consume();
        left = makeBinary(left, op, parsePower());
    }
    return left;
}

Node parseMul() {
    Node left = parsePower();
    while (peek().type == STAR || peek().type == SLASH ||
           peek().type == INTEGER_DIV || peek().type == PERCENT) {
        std::string op = consume().value;
        Node* b = makeNode();
        b->type = NODE_BINOP;
        b->op = op;
        b->line = gtokens[static_cast<size_t>(pos - 1)].line;
        b->column = gtokens[static_cast<size_t>(pos - 1)].column;
        b->left = makeNode(left);
        b->right = makeNode(parsePower());
        left = *b;
    }
    return left;
}

Node parseAdd() {
    Node left = parseMul();
    while (peek().type == PLUS || peek().type == MINUS) {
        Token op = consume();
        Node* b = makeNode();
        b->type = NODE_BINOP;
        b->line = op.line;
        b->column = op.column;
        b->op = op.value;
        b->left = makeNode(left);
        b->right = makeNode(parseMul());
        left = *b;
    }
    return left;
}

static Node makeBinary(Node left, const Token& op, Node right);

Node parseShift() {
    Node left = parseAdd();
    while (peek().type == SHIFT_LEFT || peek().type == SHIFT_RIGHT) {
        Token op = consume();
        left = makeBinary(left, op, parseAdd());
    }
    return left;
}

Node parseBitAnd() {
    Node left = parseShift();
    while (peek().type == BIT_AND) {
        Token op = consume();
        left = makeBinary(left, op, parseShift());
    }
    return left;
}

Node parseBitXor() {
    Node left = parseBitAnd();
    while (peek().type == BIT_XOR) {
        Token op = consume();
        left = makeBinary(left, op, parseBitAnd());
    }
    return left;
}

Node parseBitOr() {
    Node left = parseBitXor();
    while (peek().type == BIT_OR) {
        Token op = consume();
        left = makeBinary(left, op, parseBitXor());
    }
    return left;
}

static Node makeBinary(Node left, const Token& op, Node right) {
    Node result;
    result.type = NODE_BINOP;
    result.line = op.line;
    result.column = op.column;
    result.file = op.file;
    result.op = op.value;
    result.left = makeNode(left);
    result.right = makeNode(right);
    return result;
}

Node parseCompare() {
    Node left = parseBitOr();
    while (peek().type == GT || peek().type == LT || peek().type == GE ||
           peek().type == LE || peek().type == EQ || peek().type == NEQ) {
        Token op = consume();
        left = makeBinary(left, op, parseBitOr());
    }
    return left;
}

Node parseLogicalAnd() {
    Node left = parseCompare();
    while (peek().type == AND_AND) {
        Token op = consume();
        left = makeBinary(left, op, parseCompare());
    }
    return left;
}

Node parseExpr() {
    Node left = parseLogicalAnd();
    while (peek().type == OR_OR) {
        Token op = consume();
        left = makeBinary(left, op, parseLogicalAnd());
    }
    if (peek().type == QUESTION) {
        Token question = consume();
        Node ternary;
        ternary.type = NODE_TERNARY;
        ternary.line = question.line;
        ternary.column = question.column;
        ternary.left = makeNode(left);
        ternary.right = makeNode(parseExpr());
        expect(COLON, "':'");
        ternary.extra = makeNode(parseExpr());
        return ternary;
    }
    return left;
}

Node parseCond() {
    return parseExpr();
}

Node parseOne();

std::vector<Node> parseBlock() {
    std::vector<Node> nodes;
    while (!parseStopped && peek().type != RBRACE && peek().type != END) {
        if (peek().type == SEMICOLON) { consume(); continue; }
        const int start = pos;
        const unsigned errorsBefore = parseErrorCount;
        const unsigned failsBefore = failCalls;
        Node statement = parseOne();
        if (parseErrorCount == errorsBefore && failCalls == failsBefore) {
            nodes.push_back(std::move(statement));
        } else {
            recoverAfterError(start, true);
        }
    }
    return nodes;
}

// Parse the body of a declaration: `= <expr> ;` or `= [ <expr> , ... ] ;`.
// Sets node.type to NODE_ARRAY_DECL for array form, NODE_VAR_DECL otherwise.
static void parseDeclBody(Node& node, bool isArray) {
    expect(EQUALS);
    if (isArray) {
        node.type = NODE_ARRAY_DECL;
        expect(LBRACKET);
        while (peek().type != RBRACKET && peek().type != END) {
            node.args.push_back(parseExpr());
            if (peek().type == COMMA) consume();
        }
        expect(RBRACKET);
    } else {
        node.type = NODE_VAR_DECL;
        node.left = makeNode(parseExpr());
    }
    expect(SEMICOLON);
}

Node parseOne() {
    // Short variable and constant declarations.
    if (peek().type == LET || peek().type == CONST_T) {
        Token declarationToken = consume();
        Node node;
        node.isConst = declarationToken.type == CONST_T;
        node.line = declarationToken.line;
        node.column = declarationToken.column;
        node.file = declarationToken.file;
        Token nameToken = peek();
        if (!isNameToken(nameToken.type))
            failAt(nameToken, "ожидалось имя переменной");
        node.varName = consume().value;
        bool isArray = false;
        if (peek().type == COLON) {
            consume();
            Token typeToken = consume();
            if (!isTypeName(typeToken.value))
                failAt(typeToken, "неизвестный тип '" + typeToken.value + "'");
            node.varType = canonicalType(typeToken.value);
            if (peek().type == LBRACKET) {
                consume(); expect(RBRACKET);
                isArray = true;
            }
        }
        parseDeclBody(node, isArray);
        return node;
    }
    // Type-first declarations: int value = 42; f64[] values = [...]
    bool isTypeFirstDecl = false;
    if (isTypeToken(peek().type)) {
        if (peek(1).type == IDENTIFIER) isTypeFirstDecl = true;
        if (peek(1).type == LBRACKET && peek(2).type == RBRACKET && peek(3).type == IDENTIFIER) isTypeFirstDecl = true;
    }
    if (isTypeFirstDecl) {
        Token typeToken = consume();
        if (!isTypeName(typeToken.value))
            failAt(typeToken, "неизвестный тип '" + typeToken.value + "'");
        Node node;
        node.line = typeToken.line;
        node.column = typeToken.column;
        node.file = typeToken.file;
        node.varType = canonicalType(typeToken.value);
        bool isArray = false;
        if (peek().type == LBRACKET) {
            consume(); expect(RBRACKET);
            isArray = true;
        }
        node.varName = consume().value;
        parseDeclBody(node, isArray);
        return node;
    }
    // svi
    if (peek().type == SVI) {
        Token declarationToken = consume();
        if (peek().type == TIP) {
            consume();
            std::string structName = consume().value;
            expect(LOR);
            std::string varName = consume().value;
            Node node;
            node.line = declarationToken.line;
            node.column = declarationToken.column;
            node.file = declarationToken.file;
            node.type = NODE_STRUCT_INSTANCE;
            node.varType = structName;
            node.varName = varName;
            if (peek().type == EQUALS && peek(1).type == LBRACE) {
                consume();
                expect(LBRACE);
                while (peek().type != RBRACE && peek().type != END) {
                    Node init;
                    if (isNameToken(peek().type) && peek(1).type == EQUALS) {
                        init.varName = consume().value;
                        consume();
                        init.left = makeNode(parseExpr());
                    } else {
                        init.left = makeNode(parseExpr());
                    }
                    node.children.push_back(init);
                    if (peek().type == COMMA || peek().type == SEMICOLON)
                        consume();
                }
                expect(RBRACE);
            }
            expect(SEMICOLON);
            return node;
        }
        Token declaredType = consume();
        if (!isTypeName(declaredType.value))
            failAt(declaredType, "неизвестный тип '" + declaredType.value + "'");
        std::string varType = canonicalType(declaredType.value);
        bool isArray = false;
        if (peek().type == LBRACKET) {
            consume(); expect(RBRACKET);
            isArray = true;
        }
        consume(); // lor
        std::string varName = consume().value;
        expect(EQUALS);
        if (isArray) {
            Node node; node.type = NODE_ARRAY_DECL;
            node.line = declarationToken.line;
            node.column = declarationToken.column;
            node.varType = varType;
            node.varName = varName;
            expect(LBRACKET);
            while (peek().type != RBRACKET && peek().type != END) {
                node.args.push_back(parseExpr());
                if (peek().type == COMMA) consume();
            }
            expect(RBRACKET);
            expect(SEMICOLON);
            return node;
        } else {
            Node node; node.type = NODE_VAR_DECL;
            node.line = declarationToken.line;
            node.column = declarationToken.column;
            node.varType = varType;
            node.varName = varName;
            node.left = makeNode(parseExpr());
            expect(SEMICOLON);
            return node;
        }
    }
    // slov
    if (peek().type == SLOV) {
        Node node; node.type = NODE_SLOV;
        node.line = peek().line; node.column = peek().column; node.file = peek().file;
        consume();
        expect(LPAREN);
        Node expr = parseExpr();
        node.value = expr.value;
        node.left = makeNode(expr);
        expect(RPAREN);
        expect(SEMICOLON);
        return node;
    }
    // fucn
    if (peek().type == FUCN) {
        Node node; node.type = NODE_FUNC_DEF;
        node.line = peek().line;
        node.column = peek().column;
        node.file = peek().file;
        consume();
        node.varType = "rox";
        if (peek().type == VOID_T) {
            consume();
            node.varType = "void";
        } else if (isTypeToken(peek().type) && peek(1).type != LPAREN &&
                   peek(2).type == LPAREN) {
            Token returnType = consume();
            if (!isTypeName(returnType.value))
                failAt(returnType, "неизвестный тип результата '" + returnType.value + "'");
            node.varType = canonicalType(returnType.value);
        }
        Token functionName = peek();
        if (!isNameToken(functionName.type)) {
            if (!functionName.value.empty() && functionName.type != END &&
                (std::isalpha(static_cast<unsigned char>(functionName.value[0])) ||
                 functionName.value[0] == '_'))
                failAt(functionName, "'" + functionName.value +
                       "' — зарезервированное слово, его нельзя использовать как имя функции");
            else
                failAt(functionName, "ожидалось имя функции");
        }
        node.varName = consume().value;
        expect(LPAREN);
        while (peek().type != RPAREN && peek().type != END) {
            Token typeToken = consume();
            if (!isTypeToken(typeToken.type))
                failAt(typeToken, "ожидался тип параметра функции");
            if (!isTypeName(typeToken.value))
                failAt(typeToken, "неизвестный тип параметра '" + typeToken.value + "'");
            std::string ptype = canonicalType(typeToken.value);
            Token parameterName = peek();
            if (!isNameToken(parameterName.type))
                failAt(parameterName, "ожидалось имя параметра");
            std::string pname = consume().value;
            node.params.push_back(ptype + ":" + pname);
            if (peek().type == COMMA) {
                consume();
                if (peek().type == RPAREN)
                    failAt(peek(), "после запятой ожидался параметр");
            } else if (peek().type != RPAREN) {
                failAt(peek(), "ожидалась ',' или ')' после параметра");
            }
        }
        expect(RPAREN);
        expect(LBRACE);
        node.children = parseBlock();
        expect(RBRACE);
        if (peek().type == SEMICOLON) consume();
        return node;
    }
    // cop
    if (peek().type == COP) {
        Node node; node.type = NODE_FUNC_CALL;
        node.line = peek().line; node.column = peek().column;
        consume();
        node.varName = consume().value;
        expect(LPAREN);
        while (peek().type != RPAREN && peek().type != END) {
            node.args.push_back(parseExpr());
            if (peek().type == COMMA) consume();
        }
        expect(RPAREN);
        expect(SEMICOLON);
        return node;
    }
    // rev
    if (peek().type == REV) {
        Node node; node.type = NODE_RETURN;
        node.line = peek().line; node.column = peek().column;
        consume();
        if (peek().type != SEMICOLON)
            node.left = makeNode(parseExpr());
        expect(SEMICOLON);
        return node;
    }
    // мат. функции / str как оператор
    if (peek().type == SQRT || peek().type == ABS ||
        peek().type == POW  || peek().type == MAX ||
        peek().type == MIN  || peek().type == AR  ||
        peek().type == FLO  || peek().type == CEL ||
        peek().type == RON  || peek().type == RAN ||
        peek().type == SER  || peek().type == REP ||
        peek().type == STR) {
        Node node = parseExpr();
        expect(SEMICOLON);
        return node;
    }
    // vd — ввод числа (оператор)
    if (peek().type == VD) {
        Node node = parseExpr();
        expect(SEMICOLON);
        return node;
    }
    // vv_s — ввод строки (оператор)
    if (peek().type == VV_S) {
        consume();
        Node node; node.type = NODE_INPUT;
        node.varType = "cos"; // строка
        expect(LPAREN);
        node.value = consume().value;
        expect(RPAREN);
        expect(SEMICOLON);
        return node;
    }
    // brk — выход из цикла
    if (peek().type == BRK) {
        consume();
        expect(SEMICOLON);
        Node node; node.type = NODE_BREAK;
        return node;
    }
    // cont — следующая итерация
    if (peek().type == CONT) {
        consume();
        expect(SEMICOLON);
        Node node; node.type = NODE_CONTINUE;
        return node;
    }
    // del — удалить массив
    if (peek().type == DEL) {
        Node node; node.type = NODE_DELETE;
        consume();
        node.varName = consume().value;
        expect(SEMICOLON);
        return node;
    }
    // zap — записать в файл
    if (peek().type == ZAP) {
        Node node; node.type = NODE_FILE_WRITE;
        consume();
        expect(LPAREN);
        node.varName = consume().value; // имя файла
        expect(COMMA);
        node.left = makeNode(parseExpr());
        expect(RPAREN);
        expect(SEMICOLON);
        return node;
    }
    // zak — закрыть файл
    if (peek().type == ZAK) {
        Node node; node.type = NODE_FILE_CLOSE;
        consume();
        expect(LPAREN);
        node.varName = consume().value;
        expect(RPAREN);
        expect(SEMICOLON);
        return node;
    }
    // *p = 10 — присваивание через указатель
    if (peek().type == STAR) {
        consume(); // *
        Node node; node.type = NODE_DEREF_ASSIGN;
        node.varName = consume().value; // имя указателя
        expect(EQUALS);
        node.left = makeNode(parseExpr());
        expect(SEMICOLON);
        return node;
    }
    // p.health = 10 — поле структуры
    if (isNameToken(peek().type) && peek(1).type == DOT &&
        peek(3).type == EQUALS) {
        std::string inst = consume().value;
        consume();
        std::string field = consume().value;
        Node node;
        node.type = NODE_STRUCT_FIELD_ASSIGN;
        node.line = peek().line;
        node.column = peek().column;
        node.file = peek().file;
        node.varName = inst;
        node.op = field;
        expect(EQUALS);
        node.left = makeNode(parseExpr());
        expect(SEMICOLON);
        return node;
    }
    // arr[0] = 10 — присваивание элемента массива
    if (isNameToken(peek().type) && peek(1).type == LBRACKET) {
        Node node; node.type = NODE_ARRAY_ASSIGN;
        node.varName = consume().value;
        consume(); // [
        node.left = makeNode(parseExpr());
        expect(RBRACKET);
        expect(EQUALS);
        node.right = makeNode(parseExpr());
        expect(SEMICOLON);
        return node;
    }
    // pro — прочитать файл
    if (peek().type == PRO) {
        Token token = consume();
        Node node; node.type = NODE_FILE_READ;
        node.line = token.line;
        node.column = token.column;
        node.file = token.file;
        expect(LPAREN);
        node.varName = consume().value;
        expect(RPAREN);
        expect(SEMICOLON);
        return node;
    }
    // вызов функции как отдельный оператор
    if ((peek().type == IDENTIFIER || isCallableBuiltinToken(peek().type)) &&
        peek().type != PRO && peek(1).type == LPAREN) {
        Node node; node.type = NODE_FUNC_CALL;
        Token name = consume();
        node.varName = name.value;
        node.line = name.line; node.column = name.column; node.file = name.file;
        expect(LPAREN);
        while (peek().type != RPAREN && peek().type != END) {
            node.args.push_back(parseExpr());
            if (peek().type == COMMA) consume();
            else if (peek().type != RPAREN)
                failAt(peek(), "ожидалась ',' или ')' в списке аргументов");
        }
        expect(RPAREN);
        expect(SEMICOLON);
        return node;
    }
    // Array method calls as statements: arr.push(x), arr.pop(), arr.resize(n), arr.len(), arr.cap()
    if (peek().type == IDENTIFIER && peek(1).type == DOT) {
        Token name = peek();
        Token field = peek(2);
        if (field.type == IDENTIFIER &&
            (field.value == "push" || field.value == "pop" ||
             field.value == "resize" || field.value == "len" || field.value == "cap")) {
            consume(); // identifier
            consume(); // DOT
            Token method = consume(); // method name
            Node node;
            node.line = name.line;
            node.column = name.column;
            node.file = name.file;
            node.varName = name.value;
            if (method.value == "push") {
                node.type = NODE_ARRAY_PUSH;
                expect(LPAREN);
                node.left = makeNode(parseExpr());
                expect(RPAREN);
            } else if (method.value == "pop") {
                node.type = NODE_ARRAY_POP;
                expect(LPAREN);
                expect(RPAREN);
            } else if (method.value == "resize") {
                node.type = NODE_ARRAY_RESIZE;
                expect(LPAREN);
                node.left = makeNode(parseExpr());
                expect(RPAREN);
            } else if (method.value == "len") {
                node.type = NODE_ARRAY_LEN;
                expect(LPAREN);
                expect(RPAREN);
            } else if (method.value == "cap") {
                node.type = NODE_ARRAY_CAP;
                expect(LPAREN);
                expect(RPAREN);
            }
            expect(SEMICOLON);
            return node;
        }
    }
    // nw — создать динамический массив как оператор
    if (peek().type == NW) {
        Token token = peek();
        Node node; node.type = NODE_VAR_DECL;
        node.line = token.line; node.column = token.column; node.file = token.file;
        consume(); // nw
        Token typeToken = consume(); // rox / i32 / etc
        node.left = makeNode();
        node.left->type = NODE_DYN_ARRAY;
        node.left->varType = typeToken.value;
        node.left->line = typeToken.line;
        node.left->column = typeToken.column;
        node.left->file = typeToken.file;
        expect(LBRACKET);
        node.left->left = makeNode(parseExpr());
        expect(RBRACKET);
        node.varName = consume().value;
        expect(SEMICOLON);
        return node;
    }
    // Удобное объявление с явным типом: int count = 1;
    if (isTypeName(peek().value) &&
        (peek().type != IDENTIFIER || peek(1).type == IDENTIFIER)) {
        Token typeToken = consume();
        if (!isTypeName(typeToken.value))
            failAt(typeToken, "неизвестный тип '" + typeToken.value + "'");
        Node node; node.type = NODE_VAR_DECL;
        node.line = typeToken.line; node.column = typeToken.column; node.file = typeToken.file;
        node.varType = canonicalType(typeToken.value);
        Token variableName = peek();
        if (!isNameToken(variableName.type))
            failAt(variableName, "ожидалось имя переменной");
        node.varName = consume().value;
        expect(EQUALS, "'='");
        node.left = makeNode(parseExpr());
        expect(SEMICOLON, "';'");
        return node;
    }
    // Postfix increment/decrement.
    if (isNameToken(peek().type) &&
        (peek(1).type == PLUS_PLUS || peek(1).type == MINUS_MINUS)) {
        Token name = consume();
        Token operation = consume();
        Node node; node.type = NODE_ASSIGN;
        node.varName = name.value;
        node.line = name.line;
        node.column = name.column;
        node.file = name.file;
        Node binary;
        binary.type = NODE_BINOP;
        binary.line = operation.line;
        binary.column = operation.column;
        binary.file = operation.file;
        binary.op = operation.type == PLUS_PLUS ? "+" : "-";
        Node identifier;
        identifier.type = NODE_IDENTIFIER;
        identifier.value = name.value;
        identifier.line = name.line;
        identifier.column = name.column;
        identifier.file = name.file;
        binary.left = makeNode(identifier);
        Node one;
        one.type = NODE_NUMBER;
        one.value = "1";
        one.line = operation.line;
        one.column = operation.column;
        one.file = operation.file;
        binary.right = makeNode(one);
        node.left = makeNode(binary);
        expect(SEMICOLON);
        return node;
    }
    // Assignment and arithmetic compound assignment.
    if (isNameToken(peek().type) &&
        (peek(1).type == EQUALS || peek(1).type == PLUS_EQ ||
         peek(1).type == MINUS_EQ || peek(1).type == STAR_EQ ||
         peek(1).type == SLASH_EQ || peek(1).type == PERCENT_EQ)) {
        Node node; node.type = NODE_ASSIGN;
        Token name = consume();
        node.varName = name.value;
        node.line = name.line;
        node.column = name.column;
        Token operation = consume();
        Node right = parseExpr();
        if (operation.type != EQUALS) {
            Node binary;
            binary.type = NODE_BINOP;
            binary.line = operation.line;
            binary.column = operation.column;
            binary.file = operation.file;
            binary.op = operation.type == PLUS_EQ ? "+" :
                operation.type == MINUS_EQ ? "-" :
                operation.type == STAR_EQ ? "*" :
                operation.type == SLASH_EQ ? "/" : "%";
            Node identifier;
            identifier.type = NODE_IDENTIFIER;
            identifier.value = name.value;
            identifier.line = name.line;
            identifier.column = name.column;
            binary.left = makeNode(identifier);
            binary.right = makeNode(right);
            node.left = makeNode(binary);
        } else {
            node.left = makeNode(right);
        }
        expect(SEMICOLON);
        return node;
    }
    if (peek().type == LOOP_T) {
        Token token = consume();
        Node node; node.type = NODE_LOOP;
        node.line = token.line;
        node.column = token.column;
        expect(LBRACE);
        node.children = parseBlock();
        expect(RBRACE);
        return node;
    }
    if (peek().type == WHILE_T) {
        Token token = consume();
        Node node; node.type = NODE_WHILE;
        node.line = token.line;
        node.column = token.column;
        expect(LPAREN);
        node.left = makeNode(parseExpr());
        expect(RPAREN);
        expect(LBRACE);
        node.children = parseBlock();
        expect(RBRACE);
        return node;
    }
    // Range loop: for index in start..end { ... } / start..=end.
    if (peek().type == FOR_T) {
        Token forToken = consume();
        Node node; node.type = NODE_FOR_RANGE;
        node.line = forToken.line;
        node.column = forToken.column;
        node.file = forToken.file;
        Token name = peek();
        if (!isNameToken(name.type)) failAt(name, "ожидалось имя счётчика for");
        node.varName = consume().value;
        expect(IN_T, "'in'");
        if (peek().type == IDENTIFIER && peek(1).type == LBRACE) {
            node.type = NODE_FOR_EACH;
            Token arrayToken = consume();
            node.left = makeNode();
            node.left->type = NODE_IDENTIFIER;
            node.left->value = arrayToken.value;
            node.left->line = arrayToken.line;
            node.left->column = arrayToken.column;
            node.left->file = arrayToken.file;
            expect(LBRACE);
            node.children = parseBlock();
            expect(RBRACE);
            return node;
        }
        node.left = makeNode(parseAdd());
        if (peek().type == RANGE_EQ) {
            consume();
            node.inclusiveRange = true;
        } else {
            expect(RANGE, "'..' или '..='");
        }
        node.right = makeNode(parseExpr());
        expect(LBRACE);
        node.children = parseBlock();
        expect(RBRACE);
        return node;
    }
    // cert
    if (peek().type == CERT) {
        Node node;
        consume();
        Token kind = consume();
        node.type = (kind.type == CEH) ? NODE_LOOP_N : NODE_LOOP_W;
        expect(LPAREN);
        node.left = makeNode(parseExpr());
        expect(RPAREN);
        expect(LBRACE);
        node.children = parseBlock();
        expect(RBRACE);
        if (peek().type == SEMICOLON) consume();
        return node;
    }
    // eles
    if (peek().type == ELES) {
        Node node; node.type = NODE_IF;
        consume();
        expect(LPAREN);
        node.left = makeNode(parseCond());
        expect(RPAREN);
        expect(LBRACE);
        node.children = parseBlock();
        expect(RBRACE);
        if (peek().type == SELE) {
            consume();
            if (peek().type == ELES) {
                node.elseChildren.push_back(parseOne());
            } else {
                expect(LBRACE);
                node.elseChildren = parseBlock();
                expect(RBRACE);
            }
        }
        if (peek().type == SEMICOLON) consume();
        return node;
    }
    // tip
    if (peek().type == TIP) {
        Node node; node.type = NODE_STRUCT_DEF;
        node.line = peek().line;
        node.column = peek().column;
        consume();
        node.varName = consume().value;
        expect(LBRACE);
        while (peek().type != RBRACE && peek().type != END) {
            Node field; field.type = NODE_VAR_DECL;
            Token fieldType = consume();
            if (!isTypeName(fieldType.value) && fieldType.type != IDENTIFIER)
                failAt(fieldType, "неизвестный тип поля '" + fieldType.value + "'");
            field.varType = canonicalType(fieldType.value);
            field.varName = consume().value;
            if (peek().type == EQUALS) {
                consume(); // =
                field.left = makeNode(parseExpr());
            }
            expect(SEMICOLON);
            node.children.push_back(field);
        }
        expect(RBRACE);
        if (peek().type == SEMICOLON) consume();
        return node;
    }

    failAt(peek(), "неизвестная конструкция, начинается с " + tokenDescription(peek()));
    return Node{};
}

Node parse(const std::vector<Token>& tokens) {
    nodeArena = std::make_shared<std::vector<std::unique_ptr<Node>>>();
    gtokens = tokens;
    pos = 0;
    parseErrorCount = 0;
    lastErrorPos = 0;
    recovering = false;
    failCalls = 0;
    parseStopped = false;
    Node program;
    program.type = NODE_PROGRAM;
    program.arena = nodeArena;
    while (!parseStopped && peek().type != END) {
        if (peek().type == SEMICOLON) { consume(); continue; }
        if (peek().type == RBRACE) {
            // Лишняя `}` на верхнем уровне: одна ошибка, токен пропускается.
            failAt(peek(), "лишняя '}': нет открытого блока");
            consume();
            recovering = false;
            continue;
        }
        if (peek().type == IMP) {
            consume();
            Node node; node.type = NODE_IMPORT;
            node.value = consume().value; // имя файла
            expect(SEMICOLON);
            program.children.push_back(node);
        } else {
            const int start = pos;
            const unsigned errorsBefore = parseErrorCount;
            const unsigned failsBefore = failCalls;
            Node declaration = parseOne();
            if (parseErrorCount == errorsBefore && failCalls == failsBefore) {
                program.children.push_back(std::move(declaration));
            } else {
                recoverAfterError(start, false);
            }
        }
    }
    return program;
}

bool parseHadErrors() {
    return parseErrorCount != 0;
}
