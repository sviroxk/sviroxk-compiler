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
#include <llvm/IR/PassManager.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/TargetSelect.h>

namespace fs = std::filesystem;

#ifndef SC_VERSION
#define SC_VERSION "0.3.0"
#endif

static void printHelp(std::ostream& out) {
    out << "SC compiler " << SC_VERSION << '\n'
        << "Usage: sc <source.svx>\n"
        << "       sc --version\n"
        << "       sc --help\n"
        << "       sc -O0 <source.svx>\n"
        << "       sc -O1 <source.svx>\n"
        << "       sc -O2 <source.svx>\n"
        << "       sc -O3 <source.svx>\n";
}

static llvm::OptimizationLevel parseOptLevel(const std::string& flag) {
    if (flag == "-O0" || flag == "-O") return llvm::OptimizationLevel::O0;
    if (flag == "-O1") return llvm::OptimizationLevel::O1;
    if (flag == "-O2") return llvm::OptimizationLevel::O2;
    if (flag == "-O3") return llvm::OptimizationLevel::O3;
    return llvm::OptimizationLevel::O0;
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
        if (i >= tokens.size() || tokens[i].type != STRING) {
            std::cerr << "Ошибка " << importToken.line << ':' << importToken.column
                      << ": после 'imp' ожидался путь в кавычках" << std::endl;
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
    std::string optLevelFlag;
    std::string sourcePath;
    if (argc == 2) {
        sourcePath = argv[1];
    } else if (argc == 3 && argv[1][0] == '-' && argv[1][1] == 'O') {
        optLevelFlag = argv[1];
        sourcePath = argv[2];
    } else {
        printHelp(std::cerr);
        return 1;
    }
    if (sourcePath[0] == '-') {
        printHelp(std::cerr);
        return 1;
    }

    std::vector<Token> allTokens;
    std::set<fs::path> activeImports;
    std::set<fs::path> loadedImports;
    appendModuleTokens(fs::absolute(sourcePath), allTokens, activeImports, loadedImports);
    allTokens.push_back({END, "", 1, 1, fs::absolute(sourcePath).string()});

    Node node = parse(allTokens);
    if (parseHadErrors()) return 1;
    if (!checkSemantics(node, fs::absolute(sourcePath).string())) return 2;

    CodeGen codegen;
    llvm::InitializeAllTargetInfos();
    llvm::InitializeAllTargets();
    if (!codegen.configureTarget()) {
        std::cerr << "Ошибка: не удалось определить целевую архитектуру LLVM IR\n";
        return 4;
    }
    codegen.generate(node);
    codegen.runOptimizations(parseOptLevel(optLevelFlag));
    codegen.emitDebugInfo();
    if (getenv("SC_DUMP_IR")) codegen.print();
    std::string verifierDiagnostic;
    if (!codegen.verify(verifierDiagnostic)) {
        std::cerr << "Ошибка: внутренняя ошибка компилятора — сгенерирован некорректный LLVM IR:\n"
                  << verifierDiagnostic;
        return 3;
    }
    codegen.print();

    return 0;
}
