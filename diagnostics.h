#ifndef SC_DIAGNOSTICS_H
#define SC_DIAGNOSTICS_H

#include <algorithm>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace sc {

inline std::map<std::string, std::vector<std::string>>& sourceFiles() {
    static std::map<std::string, std::vector<std::string>> files;
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
inline void reportDiagnostic(const std::string& filename, int line, int column,
                             const std::string& category, const std::string& message) {
    const std::string displayName = filename.empty() ? "<source>" : filename;
    std::string text = displayName + ':' + std::to_string(line) + ':' + std::to_string(column) +
                       ": error[" + category + "]: " + message + '\n';

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

[[noreturn]] inline void failAt(const std::string& filename, int line, int column,
                                const std::string& category, const std::string& message,
                                int exitCode = 1) {
    reportDiagnostic(filename, line, column, category, message);
    throw CompileFailure{exitCode};
}

// Ошибка без позиции в тексте (например, не открылся исходный файл).
[[noreturn]] inline void failFile(const std::string& filename, const std::string& category,
                                  const std::string& message) {
    std::cerr << (filename.empty() ? "<source>" : filename) << ": error[" << category
              << "]: " << message << '\n';
    throw CompileFailure{1};
}

} // namespace sc

#endif
