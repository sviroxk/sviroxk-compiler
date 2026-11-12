#include "lexer.h"
#include "diagnostics.h"
#include <iostream>
#include <cctype>
#include <cstdlib>

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
                std::cerr << "Ошибка " << commentLine << ':' << commentColumn
                          << ": незакрытый блочный комментарий" << std::endl;
                std::exit(1);
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
            if      (word == "let")  add(LET,  word, tokLine, tokCol);
            else if (word == "const") add(CONST_T, word, tokLine, tokCol);
            else if (word == "for") add(FOR_T, word, tokLine, tokCol);
            else if (word == "while") add(WHILE_T, word, tokLine, tokCol);
            else if (word == "loop") add(LOOP_T, word, tokLine, tokCol);
            else if (word == "in") add(IN_T, word, tokLine, tokCol);
            else if (word == "svi")  add(SVI,  word, tokLine, tokCol);
            else if (word == "lor")  add(LOR,  word, tokLine, tokCol);
            else if (word == "int")  add(ROX,  word, tokLine, tokCol);
            else if (word == "float") add(DOR,  word, tokLine, tokCol);
            else if (word == "string") add(COS, word, tokLine, tokCol);
            else if (word == "bool") add(BUE,  word, tokLine, tokCol);
            else if (word == "void") add(VOID_T, word, tokLine, tokCol);
            else if (word == "i8" || word == "i16" || word == "i32" || word == "i64" ||
                     word == "u8" || word == "u16" || word == "u32" || word == "u64" ||
                     word == "f32" || word == "f64") add(FIXED_TYPE, word, tokLine, tokCol);
            else if (word == "uint" || word == "short" || word == "ushort" ||
                     word == "long" || word == "ulong" || word == "byte" ||
                     word == "double" || word == "str" || word == "char")
                add(IDENTIFIER, word, tokLine, tokCol);
            else if (word == "rox")  add(ROX,  word, tokLine, tokCol);
            else if (word == "dor")  add(DOR,  word, tokLine, tokCol);
            else if (word == "cos")  add(COS,  word, tokLine, tokCol);
            else if (word == "bue")  add(BUE,  word, tokLine, tokCol);
            else if (word == "if")   add(ELES, word, tokLine, tokCol);
            else if (word == "else") add(SELE, word, tokLine, tokCol);
            else if (word == "eles") add(ELES, word, tokLine, tokCol);
            else if (word == "sele") add(SELE, word, tokLine, tokCol);
            else if (word == "cert") add(CERT, word, tokLine, tokCol);
            else if (word == "ceh")  add(CEH,  word, tokLine, tokCol);
            else if (word == "usy")  add(USY,  word, tokLine, tokCol);
            else if (word == "fn")   add(FUCN, word, tokLine, tokCol);
            else if (word == "return") add(REV, word, tokLine, tokCol);
            else if (word == "print") add(SLOV, word, tokLine, tokCol);
            else if (word == "fucn") add(FUCN, word, tokLine, tokCol);
            else if (word == "rev")  add(REV,  word, tokLine, tokCol);
            else if (word == "cop")  add(COP,  word, tokLine, tokCol);
            else if (word == "tip")  add(TIP,  word, tokLine, tokCol);
            else if (word == "slov") add(SLOV, word, tokLine, tokCol);
            else if (word == "ptr")  add(PTR,  word, tokLine, tokCol);
            else if (word == "atk")  add(ATK,  word, tokLine, tokCol);
            else if (word == "zap")  add(ZAP,  word, tokLine, tokCol);
            else if (word == "pro")  add(PRO,  word, tokLine, tokCol);
            else if (word == "zak")  add(ZAK,  word, tokLine, tokCol);
            else if (word == "file") add(FILE_T, word, tokLine, tokCol);
            else if (word == "sqrt") add(SQRT, word, tokLine, tokCol);
            else if (word == "abs")  add(ABS,  word, tokLine, tokCol);
            else if (word == "pow")  add(POW,  word, tokLine, tokCol);
            else if (word == "max")  add(MAX,  word, tokLine, tokCol);
            else if (word == "min")  add(MIN,  word, tokLine, tokCol);
            else if (word == "flo")  add(FLO,  word, tokLine, tokCol);
            else if (word == "cel")  add(CEL,  word, tokLine, tokCol);
            else if (word == "ron")  add(RON,  word, tokLine, tokCol);
            else if (word == "ran")  add(RAN,  word, tokLine, tokCol);
            else if (word == "vd")   add(VD,   word, tokLine, tokCol);
            else if (word == "nw")   add(NW,   word, tokLine, tokCol);
            else if (word == "del")  add(DEL,  word, tokLine, tokCol);
            else if (word == "defer") add(DEFER, word, tokLine, tokCol);
            else if (word == "drop") add(DROP, word, tokLine, tokCol);
            else if (word == "brk" || word == "break") add(BRK, word, tokLine, tokCol);
            else if (word == "imp")  add(IMP,  word, tokLine, tokCol);
            else if (word == "cont" || word == "continue") add(CONT, word, tokLine, tokCol);
            else if (word == "ar")   add(AR,   word, tokLine, tokCol);
            else if (word == "ser")  add(SER,  word, tokLine, tokCol);
            else if (word == "rep")  add(REP,  word, tokLine, tokCol);
            else if (word == "vv_s") add(VV_S, word, tokLine, tokCol);
            else if (word == "str")  add(STR,  word, tokLine, tokCol);
            else if (word == "chr")  add(CHR,  word, tokLine, tokCol);
            else if (word == "rox64") add(ROX64, word, tokLine, tokCol);
            else if (word == "true")  add(TRUE_T, word, tokLine, tokCol);
            else if (word == "false") add(FALSE_T, word, tokLine, tokCol);
            else add(IDENTIFIER, word, tokLine, tokCol);
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
                    std::cerr << "Ошибка " << tokLine << ':' << tokCol
                              << ": некорректный числовой литерал '" << num << "'" << std::endl;
                    std::exit(1);
                }
                try {
                    const unsigned long long value = std::stoull(num.substr(2), nullptr,
                        prefix == 'x' ? 16 : 2);
                    num = std::to_string(value);
                } catch (const std::exception&) {
                    std::cerr << "Ошибка " << tokLine << ':' << tokCol
                              << ": числовой литерал вне диапазона u64" << std::endl;
                    std::exit(1);
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
                std::cerr << "Ошибка " << tokLine << ':' << tokCol
                          << ": некорректный числовой литерал '" << num << "'" << std::endl;
                std::exit(1);
            }
            add(NUMBER, num, tokLine, tokCol);
            continue;
        }

        if (code[i] == '\'') {
            ++i;
            ++column;
            if (i >= static_cast<int>(code.size()) || code[i] == '\n' || code[i] == '\'') {
                std::cerr << "Ошибка " << tokLine << ':' << tokCol
                          << ": ожидался один символ между одинарными кавычками" << std::endl;
                std::exit(1);
            }
            unsigned char charVal;
            if (code[i] == '\\') {
                ++i; ++column;
                if (i >= static_cast<int>(code.size()) || code[i] == '\n') {
                    std::cerr << "Ошибка " << tokLine << ':' << tokCol
                              << ": незавершённая escape-последовательность в символьном литерале" << std::endl;
                    std::exit(1);
                }
                switch (code[i]) {
                    case '0': charVal = 0; break;
                    case '\'': charVal = '\''; break;
                    case 'n': charVal = '\n'; break;
                    case 'r': charVal = '\r'; break;
                    case 't': charVal = '\t'; break;
                    case '\\': charVal = '\\'; break;
                    default:
                        std::cerr << "Ошибка " << tokLine << ':' << tokCol
                                  << ": неизвестная escape-последовательность в символьном литерале" << std::endl;
                        std::exit(1);
                }
            } else {
                charVal = static_cast<unsigned char>(code[i]);
            }
            ++i;
            ++column;
            if (i >= static_cast<int>(code.size()) || code[i] != '\'') {
                std::cerr << "Ошибка " << tokLine << ':' << tokCol
                          << ": символьный литерал должен содержать один символ и закрывающую кавычку" << std::endl;
                std::exit(1);
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
                            std::cerr << "Ошибка " << line << ':' << column
                                      << ": неизвестная escape-последовательность" << std::endl;
                            std::exit(1);
                    }
                    ++i;
                    ++column;
                    continue;
                }
                str += code[i++];
                ++column;
            }
            if (!closed) {
                std::cerr << "Ошибка " << tokLine << ':' << tokCol
                          << ": строковый литерал не закрыт кавычкой" << std::endl;
                std::exit(1);
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
                std::cerr << "Ошибка " << tokLine << ":" << tokCol
                          << " неизвестный символ: " << c << std::endl;
                exit(1);
        }
        i++;
        column++;
    }

    add(END, "", line, column);
    return tokens;
}
