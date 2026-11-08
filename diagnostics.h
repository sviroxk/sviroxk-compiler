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

inline void reportDiagnostic(const std::string& filename, int line, int column,
                             const std::string& category, const std::string& message,
                             const std::string& code = "") {
    const std::string displayName = filename.empty() ? "<source>" : filename;
    std::string level = (category == "warning") ? "warning" : "error";
    std::string codePart = code.empty() ? "" : ("[" + code + "]");
    std::cerr << displayName << ':' << line << ':' << column
              << ": " << level << "[" << category << "]" << codePart << ": " << message << '\n';

    auto source = sourceFiles().find(filename);
    if (source == sourceFiles().end() || line < 1 ||
        static_cast<size_t>(line) > source->second.size()) return;

    const std::string& excerpt = source->second[static_cast<size_t>(line - 1)];
    const size_t gutterWidth = std::to_string(line).size();
    std::cerr << ' ' << std::string(gutterWidth, ' ') << line << " | " << excerpt << '\n'
              << ' ' << std::string(gutterWidth, ' ') << " | ";
    for (int index = 1; index < std::max(column, 1); ++index)
        std::cerr << (static_cast<size_t>(index - 1) < excerpt.size() && excerpt[static_cast<size_t>(index - 1)] == '\t' ? '\t' : ' ');
    std::cerr << "^\n";
}

} // namespace sc

#endif
