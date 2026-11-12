#include "lexer.h"
#include "diagnostics.h"
#include <iostream>
#include <cctype>
#include <cstdlib>
#include <algorithm>
#include <string_view>
#include <utility>

namespace {

// Ключевые слова и встроенные имена → тип токена. Порядок и состав
// совпадают с прежней цепочкой if/else (первое совпадение выигрывало).
constexpr std::pair<std::string_view, TokenType> kKeywords[] = {
    {"let", LET},
    {"const", CONST_T},
    {"for", FOR_T},
    {"while", WHILE_T},
    {"loop", LOOP_T},
    {"in", IN_T},
    {"struct", TIP},
    {"new", NW},
    {"repeat", CERT},
    {"import", IMP},
    {"open", ATK},
    {"write", ZAP},
    {"read", PRO},
    {"close", ZAK},
    {"input_int", VD},
    {"input_str", VV_S},
    {"floor", FLO},
    {"ceil", CEL},
    {"round", RON},
    {"rand", RAN},
    {"len", AR},
    {"replace", REP},
    {"svi", SVI},
    {"lor", LOR},
    {"int", ROX},
    {"float", DOR},
    {"string", COS},
    {"bool", BUE},
    {"void", VOID_T},
    {"i8", FIXED_TYPE},
    {"i16", FIXED_TYPE},
    {"i32", FIXED_TYPE},
    {"i64", FIXED_TYPE},
    {"u8", FIXED_TYPE},
    {"u16", FIXED_TYPE},
    {"u32", FIXED_TYPE},
    {"u64", FIXED_TYPE},
    {"f32", FIXED_TYPE},
    {"f64", FIXED_TYPE},
    {"uint", IDENTIFIER},
    {"short", IDENTIFIER},
    {"ushort", IDENTIFIER},
    {"long", IDENTIFIER},
    {"ulong", IDENTIFIER},
    {"byte", IDENTIFIER},
    {"double", IDENTIFIER},
    {"str", IDENTIFIER},
    {"char", IDENTIFIER},
    {"rox", ROX},
    {"dor", DOR},
    {"cos", COS},
    {"bue", BUE},
    {"if", ELES},
    {"else", SELE},
    {"eles", ELES},
    {"sele", SELE},
    {"cert", CERT},
    {"ceh", CEH},
    {"usy", USY},
    {"fn", FUCN},
    {"return", REV},
    {"print", SLOV},
    {"fucn", FUCN},
    {"rev", REV},
    {"cop", COP},
    {"tip", TIP},
    {"slov", SLOV},
    {"ptr", PTR},
    {"atk", ATK},
    {"zap", ZAP},
    {"pro", PRO},
    {"zak", ZAK},
    {"file", FILE_T},
    {"sqrt", SQRT},
    {"abs", ABS},
    {"pow", POW},
    {"max", MAX},
    {"min", MIN},
    {"flo", FLO},
    {"cel", CEL},
    {"ron", RON},
    {"ran", RAN},
    {"vd", VD},
    {"nw", NW},
    {"del", DEL},
    {"defer", DEFER},
    {"drop", DROP},
    {"brk", BRK},
    {"break", BRK},
    {"imp", IMP},
    {"cont", CONT},
    {"continue", CONT},
    {"ar", AR},
    {"ser", SER},
    {"rep", REP},
    {"vv_s", VV_S},
    {"chr", CHR},
    {"rox64", ROX64},
    {"true", TRUE_T},
    {"false", FALSE_T},
};

TokenType keywordType(std::string_view word) {
    const auto it = std::ranges::find(kKeywords, word, &std::pair<std::string_view, TokenType>::first);
    return it == std::end(kKeywords) ? IDENTIFIER : it->second;
}

} // namespace

std::vector<Token> tokenize(const std::string& code, const std::string& filename) {
    std::vector<Token> tokens;
    int i = 0;
    int line = 1;
    int column = 1;

    auto add = [&](TokenType type, const std::string& value, int tokLine, int tokCol) {
        tokens.push_back({type, value, tokLine, tokCol, filename});
    };

    while (i < (int)code.size()) {
        if (code[i] == '\n') {
            line++;
            column = 1;
            i++;
            continue;
        }
        if (isspace(static_cast<unsigned char>(code[i]))) {
            column++;
            i++;
            continue;
        }
        if (code[i] == '/' && i + 1 < (int)code.size() && code[i + 1] == '/') {
            const TokenType previous = tokens.empty() ? END : tokens.back().type;
            const bool followsExpression = previous == NUMBER || previous == CHAR_LITERAL ||
                previous == IDENTIFIER || previous == RPAREN || previous == RBRACKET ||
                previous == TRUE_T || previous == FALSE_T;
            if (!followsExpression) {
                while (i < (int)code.size() && code[i] != '\n') {
                    ++i;
                    ++column;
                }
                continue;
            }
        }
        if (code[i] == '/' && i + 1 < (int)code.size() && code[i + 1] == '*') {
            const int commentLine = line;
            const int commentColumn = column;
            i += 2;
            column += 2;
            bool closed = false;
            while (i < static_cast<int>(code.size())) {
                if (code[i] == '*' && i + 1 < static_cast<int>(code.size()) && code[i + 1] == '/') {
                    i += 2;
                    column += 2;
                    closed = true;
                    break;
                }
                if (code[i] == '\n') {
                    ++line;
                    column = 1;
                    ++i;
                } else {
                    ++column;
                    ++i;
                }
            }
            if (!closed) {
                sc::failAt(filename, commentLine, commentColumn, "lexer", std::string("незакрытый блочный комментарий"));
            }
            continue;
        }

        int tokLine = line;
        int tokCol = column;

        if (isalpha(static_cast<unsigned char>(code[i])) || code[i] == '_') {
            std::string word;
            while (i < (int)code.size() &&
                   (isalnum(static_cast<unsigned char>(code[i])) ||
                    code[i] == '_' || isdigit(static_cast<unsigned char>(code[i])))) {
                word += code[i++];
                column++;
            }
            add(keywordType(word), word, tokLine, tokCol);
            continue;
        }

        if (isdigit(static_cast<unsigned char>(code[i]))) {
            std::string num;
            bool malformed = false;
            if (code[i] == '0' && i + 1 < static_cast<int>(code.size()) &&
                (code[i + 1] == 'x' || code[i + 1] == 'X' || code[i + 1] == 'b' || code[i + 1] == 'B')) {
                const char prefix = static_cast<char>(tolower(static_cast<unsigned char>(code[i + 1])));
                num += code[i++]; ++column;
                num += code[i++]; ++column;
                const int digitsStart = i;
                while (i < static_cast<int>(code.size())) {
                    const unsigned char ch = static_cast<unsigned char>(code[i]);
                    const bool valid = prefix == 'x' ? isxdigit(ch) : (ch == '0' || ch == '1');
                    if (!valid) break;
                    num += code[i++]; ++column;
                }
                if (i == digitsStart ||
                    (i < static_cast<int>(code.size()) &&
                     isalnum(static_cast<unsigned char>(code[i])))) malformed = true;
                if (malformed) {
                    sc::failAt(filename, tokLine, tokCol, "lexer", std::string("некорректный числовой литерал '") + num + "'");
                }
                try {
                    const unsigned long long value = std::stoull(num.substr(2), nullptr,
                        prefix == 'x' ? 16 : 2);
                    num = std::to_string(value);
                } catch (const std::exception&) {
                    sc::failAt(filename, tokLine, tokCol, "lexer", std::string("числовой литерал вне диапазона u64"));
                }
                add(NUMBER, num, tokLine, tokCol);
                continue;
            }
            while (i < static_cast<int>(code.size()) &&
                   isdigit(static_cast<unsigned char>(code[i]))) {
                num += code[i++];
                ++column;
            }
            if (i + 1 < static_cast<int>(code.size()) && code[i] == '.' && code[i + 1] == '.') {
                add(NUMBER, num, tokLine, tokCol);
                const int rangeLine = line;
                const int rangeColumn = column;
                i += 2;
                column += 2;
                if (i < static_cast<int>(code.size()) && code[i] == '=') {
                    add(RANGE_EQ, "..=", rangeLine, rangeColumn);
                    ++i;
                    ++column;
                } else {
                    add(RANGE, "..", rangeLine, rangeColumn);
                }
                continue;
            }
            if (i < static_cast<int>(code.size()) && code[i] == '.') {
                num += code[i++];
                ++column;
                if (i >= static_cast<int>(code.size()) ||
                    !isdigit(static_cast<unsigned char>(code[i]))) malformed = true;
                while (i < static_cast<int>(code.size()) &&
                       isdigit(static_cast<unsigned char>(code[i]))) {
                    num += code[i++];
                    ++column;
                }
            }
            if (i < static_cast<int>(code.size()) && (code[i] == 'e' || code[i] == 'E')) {
                num += code[i++];
                ++column;
                if (i < static_cast<int>(code.size()) && (code[i] == '+' || code[i] == '-')) {
                    num += code[i++];
                    ++column;
                }
                if (i >= static_cast<int>(code.size()) ||
                    !isdigit(static_cast<unsigned char>(code[i]))) malformed = true;
                while (i < static_cast<int>(code.size()) &&
                       isdigit(static_cast<unsigned char>(code[i]))) {
                    num += code[i++];
                    ++column;
                }
            }
            if (i < static_cast<int>(code.size()) && code[i] == '.') {
                malformed = true;
                while (i < static_cast<int>(code.size()) &&
                       (isdigit(static_cast<unsigned char>(code[i])) || code[i] == '.')) {
                    num += code[i++];
                    ++column;
                }
            }
            if (malformed) {
                sc::failAt(filename, tokLine, tokCol, "lexer", std::string("некорректный числовой литерал '") + num + "'");
            }
            add(NUMBER, num, tokLine, tokCol);
            continue;
        }

        if (code[i] == '\'') {
            ++i;
            ++column;
            if (i >= static_cast<int>(code.size()) || code[i] == '\n' || code[i] == '\'') {
                sc::failAt(filename, tokLine, tokCol, "lexer", std::string("ожидался один символ между одинарными кавычками"));
            }
            unsigned char charVal;
            if (code[i] == '\\') {
                ++i; ++column;
                if (i >= static_cast<int>(code.size()) || code[i] == '\n') {
                    sc::failAt(filename, tokLine, tokCol, "lexer", std::string("незавершённая escape-последовательность в символьном литерале"));
                }
                switch (code[i]) {
                    case '0': charVal = 0; break;
                    case '\'': charVal = '\''; break;
                    case 'n': charVal = '\n'; break;
                    case 'r': charVal = '\r'; break;
                    case 't': charVal = '\t'; break;
                    case '\\': charVal = '\\'; break;
                    default:
                        sc::failAt(filename, tokLine, tokCol, "lexer", std::string("неизвестная escape-последовательность в символьном литерале"));
                }
            } else {
                charVal = static_cast<unsigned char>(code[i]);
            }
            ++i;
            ++column;
            if (i >= static_cast<int>(code.size()) || code[i] != '\'') {
                sc::failAt(filename, tokLine, tokCol, "lexer", std::string("символьный литерал должен содержать один символ и закрывающую кавычку"));
            }
            ++i;
            ++column;
            add(CHAR_LITERAL, std::to_string(charVal), tokLine, tokCol);
            continue;
        }
        if (code[i] == '"') {
            std::string str;
            ++i;
            ++column;
            bool closed = false;
            while (i < static_cast<int>(code.size())) {
                if (code[i] == '"') {
                    ++i;
                    ++column;
                    closed = true;
                    break;
                }
                if (code[i] == '\n') break;
                if (code[i] == '\\') {
                    ++i;
                    ++column;
                    if (i >= static_cast<int>(code.size()) || code[i] == '\n') break;
                    switch (code[i]) {
                        case 'n': str += '\n'; break;
                        case 'r': str += '\r'; break;
                        case 't': str += '\t'; break;
                        case '0': str += '\0'; break;
                        case '\'': str += '\''; break;
                        case '"': str += '"'; break;
                        case '\\': str += '\\'; break;
                        default:
                            sc::failAt(filename, line, column, "lexer", std::string("неизвестная escape-последовательность"));
                    }
                    ++i;
                    ++column;
                    continue;
                }
                str += code[i++];
                ++column;
            }
            if (!closed) {
                sc::failAt(filename, tokLine, tokCol, "lexer", std::string("строковый литерал не закрыт кавычкой"));
            }
            add(STRING, str, tokLine, tokCol);
            continue;
        }

        char c = code[i];
        switch (c) {
            case '=':
                if (i + 1 < (int)code.size() && code[i + 1] == '=') {
                    add(EQ, "==", tokLine, tokCol);
                    i += 2;
                    column += 2;
                } else {
                    add(EQUALS, "=", tokLine, tokCol);
                    i++;
                    column++;
                }
                continue;
            case '!':
                if (i + 1 < (int)code.size() && code[i + 1] == '=') {
                    add(NEQ, "!=", tokLine, tokCol);
                    i += 2;
                    column += 2;
                    continue;
                }
                add(BANG, "!", tokLine, tokCol);
                ++i; ++column;
                continue;
            case '>':
                if (i + 1 < (int)code.size() && code[i + 1] == '=') {
                    add(GE, ">=", tokLine, tokCol); i += 2; column += 2;
                } else if (i + 1 < (int)code.size() && code[i + 1] == '>') {
                    add(SHIFT_RIGHT, ">>", tokLine, tokCol); i += 2; column += 2;
                } else { add(GT, ">", tokLine, tokCol); ++i; ++column; }
                continue;
            case '<':
                if (i + 1 < (int)code.size() && code[i + 1] == '=') {
                    add(LE, "<=", tokLine, tokCol); i += 2; column += 2;
                } else if (i + 1 < (int)code.size() && code[i + 1] == '<') {
                    add(SHIFT_LEFT, "<<", tokLine, tokCol); i += 2; column += 2;
                } else { add(LT, "<", tokLine, tokCol); ++i; ++column; }
                continue;
            case '+':
                if (i + 1 < static_cast<int>(code.size()) && code[i + 1] == '+') {
                    add(PLUS_PLUS, "++", tokLine, tokCol); ++i; ++column;
                } else if (i + 1 < static_cast<int>(code.size()) && code[i + 1] == '=') {
                    add(PLUS_EQ, "+=", tokLine, tokCol); ++i; ++column;
                } else add(PLUS, "+", tokLine, tokCol);
                break;
            case '-':
                if (i + 1 < static_cast<int>(code.size()) && code[i + 1] == '-') {
                    add(MINUS_MINUS, "--", tokLine, tokCol); ++i; ++column;
                } else if (i + 1 < static_cast<int>(code.size()) && code[i + 1] == '=') {
                    add(MINUS_EQ, "-=", tokLine, tokCol); ++i; ++column;
                } else add(MINUS, "-", tokLine, tokCol);
                break;
            case '*':
                if (i + 1 < static_cast<int>(code.size()) && code[i + 1] == '*') {
                    add(POWER, "**", tokLine, tokCol); ++i; ++column;
                } else if (i + 1 < static_cast<int>(code.size()) && code[i + 1] == '=') {
                    add(STAR_EQ, "*=", tokLine, tokCol); ++i; ++column;
                } else add(STAR, "*", tokLine, tokCol);
                break;
            case '/':
                if (i + 1 < static_cast<int>(code.size()) && code[i + 1] == '=') {
                    add(SLASH_EQ, "/=", tokLine, tokCol); ++i; ++column;
                } else if (i + 1 < static_cast<int>(code.size()) && code[i + 1] == '/') {
                    add(INTEGER_DIV, "//", tokLine, tokCol); ++i; ++column;
                } else add(SLASH, "/", tokLine, tokCol);
                break;
            case '%':
                if (i + 1 < static_cast<int>(code.size()) && code[i + 1] == '=') {
                    add(PERCENT_EQ, "%=", tokLine, tokCol); ++i; ++column;
                } else add(PERCENT, "%", tokLine, tokCol);
                break;
            case '^': add(BIT_XOR, "^", tokLine, tokCol); break;
            case '~': add(BIT_NOT, "~", tokLine, tokCol); break;
            case '&':
                if (i + 1 < (int)code.size() && code[i + 1] == '&') {
                    add(AND_AND, "&&", tokLine, tokCol); i += 2; column += 2;
                    continue;
                }
                add(BIT_AND, "&", tokLine, tokCol); break;
            case '|':
                if (i + 1 < (int)code.size() && code[i + 1] == '|') {
                    add(OR_OR, "||", tokLine, tokCol); i += 2; column += 2;
                    continue;
                }
                add(BIT_OR, "|", tokLine, tokCol); break;
            case ';': add(SEMICOLON, ";", tokLine, tokCol); break;
            case '(': add(LPAREN, "(", tokLine, tokCol); break;
            case ')': add(RPAREN, ")", tokLine, tokCol); break;
            case '{': add(LBRACE, "{", tokLine, tokCol); break;
            case '}': add(RBRACE, "}", tokLine, tokCol); break;
            case ',': add(COMMA, ",", tokLine, tokCol); break;
            case '[': add(LBRACKET, "[", tokLine, tokCol); break;
            case ']': add(RBRACKET, "]", tokLine, tokCol); break;
            case '.': add(DOT, ".", tokLine, tokCol); break;
            case ':': add(COLON, ":", tokLine, tokCol); break;
            case '?': add(QUESTION, "?", tokLine, tokCol); break;
            default:
                sc::failAt(filename, tokLine, tokCol, "lexer", std::string("неизвестный символ: ") + c);
        }
        i++;
        column++;
    }

    add(END, "", line, column);
    return tokens;
}
