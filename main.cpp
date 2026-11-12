#include <iostream>
#include <fstream>
#include <string>
#include <filesystem>
#include <set>
#include <cstdlib>
#include "lexer.h"
#include "diagnostics.h"
#include "parser.h"
#include "semantic.h"
#include "codegen.h"

namespace fs = std::filesystem;

#ifndef SC_VERSION
#define SC_VERSION "0.3.0"
#endif

static void printHelp(std::ostream& out) {
    out << "SC compiler " << SC_VERSION << '\n'
        << "Usage: sc <source.svx>\n"
        << "       sc --version\n"
        << "       sc --help\n";
}

static void appendModuleTokens(const fs::path& sourcePath,
                               std::vector<Token>& output,
                               std::set<fs::path>& active,
                               std::set<fs::path>& loaded) {
    std::error_code ec;
    fs::path canonicalPath = fs::weakly_canonical(sourcePath, ec);
    if (ec) {
        std::cerr << "Ошибка: не удалось разрешить путь модуля: "
                  << sourcePath.string() << std::endl;
        std::exit(1);
    }

    if (active.count(canonicalPath)) {
        std::cerr << "Ошибка: обнаружен циклический импорт: "
                  << canonicalPath.string() << std::endl;
        std::exit(1);
    }
    if (loaded.count(canonicalPath)) return;

    std::ifstream file(canonicalPath);
    if (!file.is_open()) {
        std::cerr << "Ошибка: не удалось открыть модуль: "
                  << canonicalPath.string() << std::endl;
        std::exit(1);
    }

    std::string code((std::istreambuf_iterator<char>(file)),
                     std::istreambuf_iterator<char>());
    sc::registerSource(canonicalPath.string(), code);
    active.insert(canonicalPath);
    std::vector<Token> tokens = tokenize(code, canonicalPath.string());

    for (size_t i = 0; i < tokens.size();) {
        if (tokens[i].type != IMP) {
            if (tokens[i].type == END) break;
            output.push_back(tokens[i++]);
            continue;
        }

        Token importToken = tokens[i++];
        if (importToken.value == "imp") {
            sc::reportDiagnostic(importToken.file, importToken.line, importToken.column, "syntax",
                                 "слово 'imp' удалено: пишите 'import'");
            std::exit(1);
        }
        if (i >= tokens.size() || tokens[i].type != STRING) {
            std::cerr << "Ошибка " << importToken.line << ':' << importToken.column
                      << ": после 'import' ожидался путь в кавычках" << std::endl;
            std::exit(1);
        }
        fs::path importPath = canonicalPath.parent_path() / tokens[i++].value;
        if (i >= tokens.size() || tokens[i].type != SEMICOLON) {
            std::cerr << "Ошибка " << importToken.line << ':' << importToken.column
                      << ": после пути импорта ожидалась ';'" << std::endl;
            std::exit(1);
        }
        ++i;
        appendModuleTokens(importPath, output, active, loaded);
    }

    active.erase(canonicalPath);
    loaded.insert(canonicalPath);
}

int main(int argc, char* argv[]) {
    if (argc == 2 && (std::string(argv[1]) == "--version" ||
                      std::string(argv[1]) == "-V")) {
        std::cout << "SC compiler " << SC_VERSION << '\n';
        return 0;
    }
    if (argc == 2 && (std::string(argv[1]) == "--help" ||
                      std::string(argv[1]) == "-h")) {
        printHelp(std::cout);
        return 0;
    }
    if (argc != 2 || argv[1][0] == '-') {
        printHelp(std::cerr);
        return 1;
    }

    std::vector<Token> allTokens;
    std::set<fs::path> activeImports;
    std::set<fs::path> loadedImports;
    appendModuleTokens(fs::absolute(argv[1]), allTokens, activeImports, loadedImports);
    allTokens.push_back({END, "", 1, 1, fs::absolute(argv[1]).string()});

    ParseResult parsed = parse(allTokens);
    if (parsed.hadErrors) return 1;
    Node& node = parsed.program;
    if (!checkSemantics(node, fs::absolute(argv[1]).string())) return 2;

    CodeGen codegen;
    codegen.generate(node);
    std::string verifierDiagnostic;
    if (!codegen.verify(verifierDiagnostic)) {
        std::cerr << "Ошибка: внутренняя ошибка компилятора — сгенерирован некорректный LLVM IR:\n"
                  << verifierDiagnostic;
        return 3;
    }
    codegen.print();

    return 0;
}
