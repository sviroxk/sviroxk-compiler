#ifndef SC_DIAGNOSTICS_H
#define SC_DIAGNOSTICS_H

#include <algorithm>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace sc {

// Внутренний суффикс своей функции с именем встроенной (parser.cpp, D-013).
inline constexpr std::string_view kUserBuiltinSuffix = ".sc_user";

inline std::string withoutUserSuffix(std::string_view message) {
    std::string text(message);
    for (size_t at = text.find(kUserBuiltinSuffix); at != std::string::npos;
         at = text.find(kUserBuiltinSuffix, at))
        text.erase(at, kUserBuiltinSuffix.size());
    return text;
}

// std::less<> — поиск по string_view без временной std::string.
inline std::map<std::string, std::vector<std::string>, std::less<>>& sourceFiles() {
    static std::map<std::string, std::vector<std::string>, std::less<>> files;
    return files;
}

inline void registerSource(const std::string& filename, const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream input(text);
    std::string line;
    while (std::getline(input, line)) lines.push_back(line);
    sourceFiles()[filename] = std::move(lines);
}

// Диагностика собирается в одну строку и пишется одним вызовом: std::cerr без
// буфера, и посимвольный вывод отступа под `^` на строке в 1 МБ — это миллион
// системных вызовов (секунды, а под ASan в CI — больше 10 с).
inline void reportDiagnostic(std::string_view filename, int line, int column,
                             std::string_view category, std::string_view message) {
    std::string text(filename.empty() ? "<source>" : filename);
    text.append(":").append(std::to_string(line)).append(":").append(std::to_string(column));
    text.append(": error[").append(category).append("]: ").append(withoutUserSuffix(message)).append("\n");

    auto source = sourceFiles().find(filename);
    if (source != sourceFiles().end() && line >= 1 &&
        static_cast<size_t>(line) <= source->second.size()) {
        const std::string& excerpt = source->second[static_cast<size_t>(line - 1)];
        const std::string gutter(std::to_string(line).size(), ' ');
        text += ' ' + gutter + std::to_string(line) + " | " + excerpt + '\n' + ' ' + gutter + " | ";
        for (int index = 1; index < std::max(column, 1); ++index)
            text += static_cast<size_t>(index - 1) < excerpt.size() &&
                            excerpt[static_cast<size_t>(index - 1)] == '\t' ? '\t' : ' ';
        text += "^\n";
    }
    std::cerr << text;
}

// Ошибка во входной программе, уже напечатанная: компиляция прекращается с этим
// кодом выхода (spec §1). Не std::exception — её не перехватят обработчики
// внутренних ошибок; ловит только main (вместо std::exit посреди кода).
struct CompileFailure {
    int exitCode = 1;
};

[[noreturn]] inline void failAt(std::string_view filename, int line, int column,
                                std::string_view category, std::string_view message,
                                int exitCode = 1) {
    reportDiagnostic(filename, line, column, category, message);
    throw CompileFailure{exitCode};
}

// Ошибка без позиции в тексте (например, не открылся исходный файл).
[[noreturn]] inline void failFile(std::string_view filename, std::string_view category,
                                  std::string_view message) {
    std::cerr << (filename.empty() ? std::string_view("<source>") : filename) << ": error[" << category
              << "]: " << message << '\n';
    throw CompileFailure{1};
}

} // namespace sc

#endif
