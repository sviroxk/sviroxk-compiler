#include "parser.h"
#include <cassert>
#include <iostream>

int main() {
    Node expression;
    {
        ParseResult parsed = parse(tokenize("int value = 1 + 2 * 3;"));
        assert(!parsed.hadErrors);
        expression = *parsed.program.children.at(0).left;
    }
    ParseResult next = parse(tokenize("int next = 4;"));
    assert(expression.left->value == "1");
    assert(expression.right->op == "*");
    assert(expression.right->right->value == "3");
    assert(!next.hadErrors);
    ParseResult broken = parse(tokenize("int broken = ;"));
    assert(broken.hadErrors);
    ParseResult valid = parse(tokenize("int valid = 9;"));
    assert(!valid.hadErrors);
    assert(broken.hadErrors); // Later parses cannot overwrite an earlier status.
    assert(parse({}).program.children.empty());
    assert(expression.left->value == "1");
    std::weak_ptr<Node> released = expression.left;
    expression = Node{};
    assert(released.expired()); // No parser-global owner keeps old subtrees alive.
    std::cout << "PASS: parser subtree survives original tree and next parse\n";
}
