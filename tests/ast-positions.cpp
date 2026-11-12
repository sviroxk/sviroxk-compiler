// Every AST node produced by the parser carries a source position (file, line, column).
// Usage: ast-positions file.svx...   Exit 1 and list the nodes without a position.
#include "parser.h"
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>

static std::map<int, int> missing;
static std::map<int, std::string> where;
static unsigned total = 0;

static void walk(const Node& node, const std::string& path, const Node* parent = nullptr) {
    ++total;
    if (node.line <= 0 || node.column <= 0 || node.file.empty()) {
        ++missing[node.type];
        if (!where.count(node.type)) where[node.type] = path + " (value '" + node.value + "'" +
            (parent ? ", parent type " + std::to_string(parent->type) + " at " + std::to_string(parent->line) + ":" + std::to_string(parent->column) : "") + ")";
    }
    if (node.left) walk(*node.left, path, &node);
    if (node.right) walk(*node.right, path, &node);
    if (node.extra) walk(*node.extra, path, &node);
    for (const Node& child : node.children) walk(child, path, &node);
    for (const Node& child : node.elseChildren) walk(child, path, &node);
    for (const Node& child : node.args) walk(child, path, &node);
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        std::ifstream in(argv[i]);
        std::stringstream text;
        text << in.rdbuf();
        ParseResult parsed = parse(tokenize(text.str(), argv[i]));
        if (parsed.hadErrors) continue;  // only well-formed programs are checked
        for (const Node& top : parsed.program.children) walk(top, argv[i]);
    }
    for (const auto& [type, count] : missing)
        std::cerr << "node type " << type << ": " << count << " without position, e.g. " << where[type] << "\n";
    if (!missing.empty()) return 1;
    std::cout << "PASS: all " << total << " AST nodes have a source position\n";
}
