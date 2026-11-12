static_assert(__cplusplus >= 202002L, "SC собирается как C++20: build.sh задаёт -std=c++20");
#include <iostream>
#include <fstream>
#include <string>
#include <filesystem>
#include <set>
#include <cstdlib>
#include <pthread.h>
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

// Ошибки импорта — в общем формате диагностик: «файл:строка:столбец: error[import]»
// у строки import, а для исходного файла из командной строки — «файл: error[import]».
[[noreturn]] static void importFailure(const Token* importedAt, const std::string& path,
                                       const std::string& message) {
    if (importedAt)
        sc::failAt(importedAt->file, importedAt->line, importedAt->column, "import", message);
    sc::failFile(path, "import", message);
}

static void appendModuleTokens(const fs::path& sourcePath,
                               std::vector<Token>& output,
                               std::set<fs::path>& active,
                               std::set<fs::path>& loaded,
                               const Token* importedAt = nullptr) {
    std::error_code ec;
    fs::path canonicalPath = fs::weakly_canonical(sourcePath, ec);
    if (ec)
        importFailure(importedAt, sourcePath.string(),
                      "не удалось разрешить путь модуля: " + sourcePath.string());

    if (active.contains(canonicalPath))
        importFailure(importedAt, canonicalPath.string(),
                      "обнаружен циклический импорт: " + canonicalPath.string());
    if (loaded.contains(canonicalPath)) return;

    if (fs::exists(canonicalPath, ec) && !fs::is_regular_file(canonicalPath, ec))
        importFailure(importedAt, canonicalPath.string(),
                      "не удалось открыть модуль: " + canonicalPath.string() + " (это не обычный файл)");
    std::ifstream file(canonicalPath);
    if (!file.is_open())
        importFailure(importedAt, canonicalPath.string(),
                      "не удалось открыть модуль: " + canonicalPath.string());

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
        if (importToken.value == "imp")
            sc::failAt(importToken.file, importToken.line, importToken.column, "syntax",
                       "слово 'imp' удалено: пишите 'import'");
        if (i >= tokens.size() || tokens[i].type != STRING)
            sc::failAt(importToken.file, importToken.line, importToken.column, "import",
                       "после 'import' ожидался путь в кавычках");
        fs::path importPath = canonicalPath.parent_path() / tokens[i++].value;
        if (i >= tokens.size() || tokens[i].type != SEMICOLON)
            sc::failAt(importToken.file, importToken.line, importToken.column, "import",
                       "после пути импорта ожидалась ';'");
        ++i;
        appendModuleTokens(importPath, output, active, loaded, &importToken);
    }

    active.erase(canonicalPath);
    loaded.insert(canonicalPath);
}

static int run(int argc, char* argv[]) {
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

// spec §1: код 3 — внутренняя ошибка компилятора. Любое исключение — это она,
// а не аварийное завершение (abort, код 134).
static int guardedRun(int argc, char* argv[]) {
    try {
        return run(argc, argv);
    } catch (const sc::CompileFailure& failure) {
        return failure.exitCode;  // ошибка в программе уже напечатана
    } catch (const std::exception& error) {
        std::cerr << "Ошибка: внутренняя ошибка компилятора: " << error.what() << '\n';
    } catch (...) {
        std::cerr << "Ошибка: внутренняя ошибка компилятора: неизвестное исключение\n";
    }
    return 3;
}

// Разбор, семантика и codegen рекурсивны: 256 уровней вложенности (лимит парсера)
// и длинные цепочки `a + b + …` не помещаются в стандартные 8 МБ стека. Поэтому
// компиляция идёт в потоке с большим стеком (память берётся по мере надобности),
// как делает clang (runWithSufficientStackSpace).
constexpr size_t kCompilerStackBytes = size_t{512} << 20;

struct CompileJob {
    int argc;
    char** argv;
    int status = 3;
};

static void* compileOnBigStack(void* raw) {
    auto* job = static_cast<CompileJob*>(raw);
    job->status = guardedRun(job->argc, job->argv);
    return nullptr;
}

int main(int argc, char* argv[]) {
    CompileJob job{argc, argv};
    pthread_attr_t attributes;
    pthread_t thread;
    if (pthread_attr_init(&attributes) == 0) {
        const bool started = pthread_attr_setstacksize(&attributes, kCompilerStackBytes) == 0 &&
                             pthread_create(&thread, &attributes, compileOnBigStack, &job) == 0;
        pthread_attr_destroy(&attributes);
        if (started && pthread_join(thread, nullptr) == 0) return job.status;
    }
    return guardedRun(argc, argv);  // поток не создан: обычный стек
}
