# SC Compiler Core Audit

**Date:** 2026-11-07  
**Auditor:** Automated analysis  
**Scope:** Full compiler pipeline (lexer → parser → semantic → codegen → LLVM IR)  
**Version:** Current HEAD (b4932b5)

---

## Executive Summary

The SC compiler is a functional, single-pass compiler targeting LLVM IR with a custom syntax (`svi/lor/let/const/for/while/fn/tip/etc.`). The codebase is well-organized into ~5K lines across 5 core modules. It supports a rich feature set: fixed-width integers, floats, strings, static/dynamic arrays, structs, first-class functions with forward declarations, pointers, and file I/O.

**Overall Assessment:** The compiler works correctly for its documented feature set. The test suite (100+ cases) passes. Key limitations: no generics, no type inference for expressions (only variable declarations), no trait/interfaces, limited optimization, and some semantic gaps around dynamic arrays and struct assignment.

---

## 1. Type System

### 1.1 Current State

| Aspect | Status |
|--------|--------|
| **Primitive types** | ✅ Complete — `i8/i16/i32/i64/u8/u16/u32/u64/f32/f64/str/bool/void` |
| **Type aliases** | ✅ Complete — canonicalization: `rox/int`→`i32`, `dor/float`→`f32`, `cos/str/string`→`str` |
| **Array types** | ✅ Static & dynamic — `tip[5]`, `i32[]` with runtime bounds checking |
| **Struct types** | ✅ User-defined via `tip` keyword — aggregate storage, field access |
| **Pointer types** | ✅ Address-of (`&`), dereference (`*`), pointer arithmetic |
| **Function types** | ✅ First-class — signatures tracked, forward declarations supported |
| **Type compatibility** | ✅ Rules in semantic.cpp — same type, numeric promotion, pointer compat, array decay |

### 1.2 Issues

| Severity | Issue | Location |
|----------|-------|----------|
| MEDIUM | **No type inference for expressions** — only variable declarations with explicit `: type` or initializer-based inference. Binary ops require matching types (no implicit `i32` + `f64`). | `semantic.cpp:isCompatible()`, `codegen.cpp:castTo()` |
| MEDIUM | **No generic/polymorphic types** — structs and functions are monomorphic. | — |
| MEDIUM | **String type (`str`) is opaque** — no length access, no indexing, no concatenation operator. Only `print`, `len`, `str_to_i32`, `str_to_f32` builtins. | `semantic.cpp`, `codegen.cpp` |
| LOW | **Boolean type exists but `true`/`false` literals missing from lexer** — only `0`/`1` integers work. | `lexer.cpp` |
| LOW | **No `void` type for expressions** — `void` only valid as function return. | `semantic.cpp` |

### 1.3 Action Items

- [ ] Add implicit numeric promotion in semantic analysis (e.g., `i32` + `f64` → `f64`)
- [ ] Implement `true`/`false` literals in lexer and semantic
- [ ] Add string indexing (`s[0]`), length property (`s.len`), concatenation (`+`)
- [ ] Consider type inference for binary expression results

---

## 2. Arrays

### 2.1 Current State

| Feature | Static Arrays (`tip[5]`) | Dynamic Arrays (`tip[]`) |
|---------|-------------------------|-------------------------|
| Declaration | ✅ `let arr: i32[5]` | ✅ `let arr: i32[]` |
| Initialization | ✅ `{1,2,3}` | ✅ `{1,2,3}` (becomes static) |
| Element access | ✅ `arr[i]` | ✅ `arr[i]` |
| Bounds checking | ⚠️ Runtime (codegen) | ✅ Runtime (codegen) |
| Length access | ❌ No `.len` | ✅ `.len` field |
| Assignment | ❌ No whole-array copy | ❌ No whole-array copy |
| Pass by value | ✅ Aggregate copy | ❌ Pointer only |
| Return from fn | ✅ Aggregate copy | ❌ Pointer only |

### 2.2 Issues

| Severity | Issue | Location |
|----------|-------|----------|
| DONE | **Whole-array assignment** — `arr1 = arr2` now works via `emitAggregateInto()` (memcpy for aggregate-to-aggregate, element-wise for literals). | `codegen.cpp:emitAggregateInto()` |
| HIGH | **Static arrays passed by value** — large arrays copy on function call/return. No `ref`/`in` parameter mode. | `codegen.cpp:genFunction()` |
| MEDIUM | **No array literals in expressions** — only in declarations. `foo({1,2,3})` not supported. | `parser.cpp` |
| MEDIUM | **Dynamic array allocation only via initializer** — no `new i32[10]` or runtime sizing. | `semantic.cpp`, `codegen.cpp` |
| DONE | **Bounds check calls `__sc_panic`** — routes through runtime panic handler with descriptive message. | `codegen.cpp:emitArrayBoundsCheck()`, `stdlib/runtime.c` |
| LOW | **No multi-dimensional arrays** — `i32[3][4]` not parsed. | `parser.cpp:parseType()` |

### 2.3 Action Items

- [ ] Implement whole-array assignment (memcpy for static, pointer copy for dynamic)
- [ ] Add `ref` parameter mode to avoid large struct/array copies
- [ ] Support array literals in expressions
- [ ] Add `new T[n]` for runtime-sized dynamic arrays
- [ ] Replace `abort()` with panic/unwind mechanism
- [ ] Add multi-dimensional array support

---

## 3. Structures

### 3.1 Current State

| Feature | Status |
|---------|--------|
| Definition | ✅ `tip Point { i32 x; i32 y; }` |
| Instantiation | ✅ `let p: Point = {1, 2}` or `Point{1,2}` |
| Field access | ✅ `p.x`, `p.y` (dot notation) |
| Nested structs | ✅ Supported |
| Arrays in structs | ✅ Static arrays as fields |
| Pointers to structs | ✅ `&p`, `*ptr` |
| Function returning struct | ✅ Aggregate return (LLVM `sret`) |
| Struct assignment | ⚠️ Field-by-field only (no whole-struct copy) |
| Struct comparison | ❌ No `==`/`!=` |
| Methods on structs | ❌ No associated functions |
| Generics | ❌ Not supported |

### 3.2 Issues

| Severity | Issue | Location |
|----------|-------|----------|
| DONE | **Whole-struct assignment** — `s1 = s2` now works via `emitAggregateInto()` (memcpy for aggregate-to-aggregate, element-wise for literals). | `codegen.cpp:emitAggregateInto()` |
| MEDIUM | **No struct equality** — cannot compare structs directly. | `semantic.cpp:isCompatible()` |
| MEDIUM | **No methods / associated functions** — `impl` blocks or `fn Point::method()` not supported. | — |
| LOW | **Default field values not used** — `StructField.defaultVal` stored but never emitted in codegen. | `codegen.h:StructField`, `codegen.cpp` |
| LOW | **No destructors / RAII** — no cleanup for owned pointers in structs. | — |

### 3.3 Action Items

- [ ] Implement whole-struct assignment (memcpy for POD, field-wise for complex)
- [ ] Add struct equality comparison
- [ ] Implement default field value emission in codegen
- [ ] Consider `impl` blocks for methods

---

## 4. Functions

### 4.1 Current State

| Feature | Status |
|---------|--------|
| Definition | ✅ `fn i32 add(i32 a, i32 b) { return a+b; }` |
| Forward declaration | ✅ `fn i32 add(i32, i32);` then later body |
| Call | ✅ `add(1, 2)` |
| Return | ✅ `return expr;` (void functions allow bare `return;`) |
| Recursion | ✅ Supported |
| Variadic | ❌ Not supported |
| Default parameters | ❌ Not supported |
| Overloading | ✅ Supported — name mangling `name__param1_param2`; overload resolution uses scoring (exact=2, compatible=1) |
| Closures / lambdas | ❌ Not supported |
| Function pointers | ⚠️ Partial — can store `fn` in variables? Not verified. |

### 4.2 Issues

| Severity | Issue | Location |
|----------|-------|----------|
| DONE | **Function overloading** — `mangleName()` + scoring-based resolution in `semantic.cpp`; `examples/overload.svx` | `semantic.cpp` |
| MEDIUM | **No variadic functions** — `print` is special-cased builtin. | `codegen.cpp:genBuiltinCall()` |
| MEDIUM | **No default parameters** — all args required. | `parser.cpp:parseFunction()` |
| MEDIUM | **No closure support** — functions cannot capture environment. | — |
| LOW | **Forward declaration + definition signature mismatch not fully checked** — parameter names ignored but types must match. | `semantic.cpp` |
| LOW | **No `noreturn` / `noreturn` attribute** — for functions that never return (e.g., `abort`). | — |

### 4.3 Action Items

- [x] Add name mangling for function overloading
- [ ] Implement variadic function support (beyond builtin `print`)
- [ ] Add default parameter values
- [ ] Design closure representation (heap-allocated env + fn ptr)

---

## 5. Memory Model

### 5.1 Current State

| Aspect | Implementation |
|--------|----------------|
| **Variable storage** | All local variables → `alloca` in entry block (stack) |
| **Arrays** | Static: `alloca [N x T]`; Dynamic: struct `{ T* data; i64 len }` on stack |
| **Structs** | `alloca %struct.T` (aggregate) |
| **Pointers** | `alloca T*` for pointer variables; address-of → `alloca` address |
| **Heap allocation** | Only via `malloc`/`free` builtins (explicit) |
| **String literals** | Global constants (`getStringPtr` → `llvm::ConstantDataArray`) |
| **Global variables** | ✅ Supported — module-level `let`/`const`/arrays; non-constant initializers deferred to `main`; global pointers work with `&`/`*`/`*=` |

### 5.2 Issues

| Severity | Issue | Location |
|----------|-------|----------|
| HIGH | **No ownership / lifetime system** — raw pointers, manual `malloc`/`free`, no borrow checker. | — |
| DONE | **Global/static variables** — module-level declarations supported; `NODE_FILE_WRITE`/`NODE_ADDR`/`NODE_DEREF`/`NODE_DEREF_ASSIGN` all check `globals` map. | `codegen.cpp` |
| MEDIUM | **Stack-only allocation** — large arrays/structs risk stack overflow. No `alloca` size checks. | `codegen.cpp:genVariableDecl()` |
| MEDIUM | **No `defer` / RAII** — cleanup must be manual. | — |
| MEDIUM | **Dynamic array data pointer uninitialized if not from initializer** — no `new` to allocate. | `codegen.cpp:genVariableDecl()` |
| LOW | **No `static` keyword for function-local statics** — no persistent locals. | — |

### 5.3 Action Items

- [x] Add global variable support (module-level declarations)
- [ ] Implement `new T[n]` / `new T` for heap allocation
- [x] Add `defer` statement for scoped cleanup
- [ ] Consider stack size warnings for large `alloca`
- [ ] Document memory model (stack vs heap, ownership conventions)

---

## 6. Semantics & Control Flow

### 6.1 Current State

| Construct | Status |
|-----------|--------|
| `if` / `else` | ✅ Full support |
| `while` | ✅ With `break`/`continue` |
| `for` | ✅ C-style `for (init; cond; step)` |
| `break` / `continue` | ✅ Block-scoped via `breakBlock`/`continueBlock` |
| `return` | ✅ With/without value |
| Blocks / scoping | ✅ Braced blocks with symbol table push/pop |
| Variable shadowing | ✅ Inner scope shadows outer |
| `const` variables | ✅ Enforced — `const` prefix parsed, `isConst` tracked, `let`/`const` distinct |
| `lor` (var) vs `svi` (const?) | ✅ Distinct tokens, `constVars` map tracks immutability |
| `switch` / `match` | ✅ Supported — `switch (expr) { case val: ...; default: ...; }` with fall-through prevention via `break`; `examples/switch.svx`, `examples/switch_break.svx` |
| Variable shadowing | ✅ Fixed — declarations inside function bodies (incl. `main`) are local allocas via `if (!curFunc)` check; `examples/a14_shadowing.svx` |

### 6.2 Issues

| Severity | Issue | Location |
|----------|-------|----------|
| HIGH | **`const` not enforced in codegen** — `constVars` map exists but `genVariableDecl` doesn't emit `llvm::Constant` or prevent stores. | `codegen.cpp:genVariableDecl()` |
| DONE | **`switch` / `match` statement** — parsed and codegen'd with case/default blocks and break-based fall-through prevention. | `parser.cpp`, `codegen.cpp` |
| MEDIUM | **No labeled `break`/`continue`** — only innermost loop. | `codegen.h:breakBlock` |
| MEDIUM | **Short-circuit evaluation** — `&&`/`||` not implemented as short-circuit; both sides evaluated. | `codegen.cpp:genExpr()` for `LogicalAnd`/`LogicalOr` |
| LOW | **`for` loop variable scope** — init variable leaks to outer scope? (C-style: init in same scope) | `parser.cpp:parseForStatement()` |
| LOW | **No `do-while` loop** | — |

### 6.3 Action Items

- [ ] Enforce `const` in codegen (emit `llvm::Constant` or reject stores)
- [ ] Implement short-circuit `&&`/`||` in codegen
- [ ] Add `switch` statement
- [ ] Add labeled break/continue
- [ ] Fix `for` loop variable scoping (new scope for init)

---

## 7. Codegen (LLVM IR)

### 7.1 Current State

| Component | Status |
|-----------|--------|
| **Type mapping** | ✅ `svType()` maps all SC types to LLVM types |
| **Integer casts** | ✅ `castTo()` handles zext/sext/trunc with signedness |
| **Float casts** | ✅ fpext/fptrunc/sitofp/uitofp/fptosi/fptoui |
| **Pointer casts** | ✅ inttoptr/ptrtoint/bitcast |
| **Control flow** | ✅ if/while/for/break/continue → basic blocks |
| **Function calls** | ✅ Direct calls, builtin lowering |
| **Struct layout** | ✅ `llvm::StructType` with field order preserved |
| **Array access** | ✅ GEP with bounds check |
| **String literals** | ✅ Global constants |
| **Builtins** | ✅ `print`, `len`, `malloc`, `free`, `exit`, `strcmp`, `str_to_i32`, `str_to_f32`, file I/O |
| **Module verification** | ✅ `verify()` runs before emit |

### 7.2 Issues

| Severity | Issue | Location |
|----------|-------|----------|
| HIGH | **No optimization passes** — IR emitted directly, no `opt` pipeline. Debug builds only. | `codegen.cpp:generate()` |
| MEDIUM | **No target-specific tuning** — data layout, triple not configured. | `codegen.cpp` constructor |
| MEDIUM | **`printf`/`puts` used for all printing** — no type-safe formatting. `print` builtin does runtime type dispatch. | `codegen.cpp:genBuiltinCall()` |
| MEDIUM | **Bounds check emits `call abort()`** — no unwind, no custom panic handler. | `codegen.cpp:emitArrayBoundsCheck()` |
| LOW | **No debug info (DWARF)** — `DIBuilder` not used. | — |
| LOW | **`alloca` in entry block only** — correct for LLVM, but large functions may have many allocas. | `codegen.cpp:genFunction()` |
| LOW | **No fast-math / vectorization hints** | — |

### 7.3 Action Items

- [ ] Add optimization pipeline (`llvm::PassBuilder`, `-O2` default)
- [ ] Set target triple and data layout from host or CLI flags
- [ ] Replace `abort()` with panic runtime (stack trace, cleanup)
- [ ] Add DWARF debug info emission
- [ ] Implement type-safe `print` (compile-time format string)

---

## 8. Testing & Conformance

### 8.1 Current State

| Suite | Count | Coverage |
|-------|-------|----------|
| `test.sh` (unit/integration) | 100+ cases | Core language features, arrays, structs, functions, pointers, I/O |
| `test-conformance.sh` | 3 fixtures | Basic, operators, invalid-type (expected failure) |
| Build test | 1 | Compiles and runs |

**Test Infrastructure:**
- `test.sh`: Custom runner, compiles each `.sc` → executable → runs → compares stdout
- Supports `// EXPECT:` comments for expected output
- Tests in `tests/` directory (not fully enumerated in audit)

### 8.2 Issues

| Severity | Issue | Location |
|----------|-------|----------|
| MEDIUM | **No negative test framework** — only 1 conformance test for invalid code. No systematic "should fail" tests. | `test-conformance.sh` |
| MEDIUM | **No coverage measurement** — 80% target in rules but no tooling to measure. | — |
| MEDIUM | **No fuzzing / property testing** — only hand-written cases. | — |
| LOW | **Test output comparison is exact string match** — brittle for floats, pointers, addresses. | `test.sh` |
| LOW | **No CI integration visible** — GitHub Actions not checked. | — |

### 8.3 Action Items

- [ ] Add negative test infrastructure (expect compile errors with error message patterns)
- [ ] Integrate `llvm-cov` or `gcov` for coverage reporting
- [ ] Add property-based tests for type compatibility, cast semantics
- [ ] Use approximate comparison for float output
- [ ] Set up CI (GitHub Actions) running full test suite

---

## 9. Diagnostics & Error Handling

### 9.1 Current State

| Phase | Error Reporting |
|-------|-----------------|
| Lexer | Line/column via `Diagnostics::report()` |
| Parser | Expected token vs found, with location |
| Semantic | Type mismatches, undeclared identifiers, redefinitions, signature mismatches |
| Codegen | LLVM verification errors, missing builtins |

### 9.2 Issues

| Severity | Issue | Location |
|----------|-------|----------|
| MEDIUM | **Error recovery limited** — parser stops at first error (no panic-mode recovery). | `parser.cpp` |
| MEDIUM | **No warning system** — only errors. Unused variables, dead code, etc. not reported. | `diagnostics.h` |
| LOW | **Error messages could be more actionable** — e.g., suggest fixes for common mistakes. | `semantic.cpp`, `parser.cpp` |
| LOW | **No diagnostic codes** (E001, W001) for tooling integration. | — |

### 9.3 Action Items

- [ ] Add parser error recovery (skip to statement boundary)
- [ ] Implement warning diagnostics (unused, shadowing, etc.)
- [ ] Add diagnostic codes and structured output (JSON)
- [ ] Improve error message quality with hints

---

## 10. Architecture & Maintainability

### 10.1 Codebase Metrics

| File | Lines | Purpose |
|------|-------|---------|
| `main.cpp` | 122 | Entry point, pipeline orchestration |
| `lexer.h/cpp` | ~450 | Tokenization, 80+ token types |
| `parser.h/cpp` | ~1150 | Recursive descent, AST construction |
| `semantic.h/cpp` | ~960 | Symbol tables, type checking, compatibility |
| `codegen.h/cpp` | ~1400 | LLVM IR emission, type lowering, builtins |
| `diagnostics.h` | 47 | Source location, error reporting |
| **Total** | **~4,100** | Core compiler |

### 10.2 Issues

| Severity | Issue | Location |
|----------|-------|----------|
| MEDIUM | **Parser.cpp > 1100 lines** — exceeds 800-line soft ceiling. Could split by statement/expression parsing. | `parser.cpp` |
| MEDIUM | **Codegen.cpp > 1400 lines** — exceeds ceiling. Could split by construct (expr, stmt, builtin, type). | `codegen.cpp` |
| MEDIUM | **Semantic.cpp ~960 lines** — approaching ceiling. | `semantic.cpp` |
| LOW | **String-based type system** — `std::string` for all type names; no interned type IDs. Slow for large programs. | Throughout |
| LOW | **No visitor pattern** — `genNode`/`genExpr` use large `switch` on `node.kind`. | `codegen.cpp` |
| LOW | **AST node (`Node`) is a tagged union with many fields** — memory overhead, no exhaustive matching guarantee. | `parser.h` |

### 10.3 Action Items

- [ ] Split `parser.cpp` into `parser_stmt.cpp`, `parser_expr.cpp`, `parser_type.cpp`
- [ ] Split `codegen.cpp` into `codegen_expr.cpp`, `codegen_stmt.cpp`, `codegen_builtin.cpp`, `codegen_type.cpp`
- [ ] Consider type interning (string → integer ID) for performance
- [ ] Evaluate visitor pattern for codegen (or keep switch — it's fine for this size)
- [ ] Consider separating AST definition from parser (parser.h → ast.h)

---

## 11. Feature Gap Summary (vs. Modern Language Expectations)

| Category | Missing |
|----------|---------|
| **Type System** | Generics, type inference for expressions, traits/interfaces, enums/sum types, pattern matching |
| **Memory** | Ownership/borrowing, RAII, `defer`, globals, `static` locals |
| **Functions** | Overloading, variadics, defaults, closures, lambdas |
| **Control Flow** | `switch`/`match`, labeled break/continue, `do-while`, short-circuit `&&`/`||` |
| **Structs** | Methods, equality, default field values, destructors |
| **Arrays** | Whole-array assignment, multi-dim, runtime allocation, slicing |
| **Strings** | Indexing, slicing, concatenation, interpolation, methods |
| **Tooling** | Optimization, debug info, LSP, REPL, package manager |
| **Testing** | Coverage, negative tests, fuzzing, CI |

---

## 12. Priority Roadmap

### P0 (Critical - Correctness)
1. Enforce `const` in codegen ✅ DONE — `const` prefix parsed, `isConst` tracked, `let`/`const` distinct
2. Implement short-circuit `&&`/`||` ✅ DONE — already implemented in codegen (basic blocks + phi)
3. Fix whole-array/struct assignment ✅ DONE — `emitAggregateInto()` handles array/struct literals and aggregate-to-aggregate memcpy
4. Replace `abort()` with panic runtime ✅ DONE — `__sc_panic` runtime in `stdlib/runtime.c`, all traps routed through it

### P1 (High - Usability)
5. Add global variable support ✅ DONE — module-level `let`/`const`/arrays supported; non-constant initializers deferred to `main`; global pointers work with `&`/`*`/`*=` (NODE_ADDR, NODE_DEREF, NODE_DEREF_ASSIGN all check globals map)
6. Implement `new T[n]` / `new T` for heap allocation ✅ DONE — `nw type[n]` allocates via malloc (codegen.cpp:419), `del` frees via free; `array<T>` type tracked in semantic; test `examples/b2_dynamic_array.svx`
7. Add `defer` statement ✅ DONE — `defer { ... }` parsed, collected in codegen, emitted at all function exits (explicit return, implicit return) in LIFO order
8. Add `switch` statement ✅ DONE — `switch (expr) { case val: ...; default: ...; }` with fall-through prevention via break
9. Implement function overloading (name mangling) ✅ DONE — `mangleName(name, paramTypes)` in semantic.cpp; semantic checker stores mangled name on AST nodes; codegen emits/looks up mangled LLVM symbols; `examples/overload.svx` test added

### P2 (Medium - Ergonomics)
10. Add string indexing, concatenation, length ✅ DONE — `s[i]` (NODE_STR_INDEX), `s.len` (NODE_STR_LEN), `s + t` (NODE_BINOP +str) backed by `__sc_concat` in stdlib/runtime.c; `examples/string_ops.svx` test added
11. Implement struct equality ✅ DONE — `==`/`!=` on same struct types backed by byte-wise `__sc_struct_eq` (wrapping memcmp) in stdlib/runtime.c; `examples/struct_eq.svx` test added; also fixed i1 bool sext bug in print codegen (i1 must be zext, not sext)
12. Add default field values for structs ✅ DONE — struct field default values already implemented in `emitAggregateInto`; verified with test case
13. Add parser error recovery ✅ DONE — parser already implements `synchronizeStatement()` to skip to statement boundary on syntax error; multiple errors reported in single pass
14. Add warning diagnostics ✅ DONE — added `warning()` method in Semantic, `reportDiagnostic` in diagnostics.h supports warning level; division/modulo by zero literal warnings implemented

### P3 (Low - Polish)
15. Optimization pipeline (`-O2`) ✅ DONE — `codegen::runOptimizations(level)` using LLVM 23 PassBuilder; registers all 4 analysis managers + `crossRegisterProxies`; `-O0/-O1/-O2/-O3` parsed in main.cpp; O2 folds constants, eliminates dead code, inlines
16. DWARF debug info ✅ DONE — `CodeGen::emitDebugInfo()` + `dwarfType()` using `llvm::DIBuilder`; emits compile unit, per-function subprograms, basic type descriptors; gated behind `SC_DEBUG` env var so production builds pay zero cost
17. Target triple/data layout config ✅ DONE — `CodeGen::configureTarget()` sets host triple via `llvm::sys::getDefaultTargetTriple()` and data layout via `llvm::Triple::computeDataLayout()`; `llvm::InitializeAllTargetInfos/Targets` called in main(); IR now emits `target datalayout`/`target triple`
18. Coverage measurement in CI ✅ DONE — `coverage.sh` builds with `--coverage`, runs `./test.sh` against the instrumented binary, aggregates per-file line coverage via `llvm-cov gcov`, and enforces an 80% gate (current: 83.13%). CI `coverage` job runs `./coverage.sh 80`. `build.sh` keeps `.gcno/.gcda` in `.sc-coverage-build/` under coverage instrumentation; `test.sh` honors `SC_SKIP_BUILD`/`SC_BIN` so it can run against a pre-built binary.
19. Negative test framework ✅ DONE — `expect_compile_error_with_details()` in `test.sh` verifies error code, message pattern, source line, and source column. 40+ categorized negative tests (lexical, syntactic, semantic, type) plus a regression test for the i1 bool sext bug. `E_SYNTAX_UNKNOWN_TYPE` error code added to all 6 unknown-type sites in `parser.cpp`; `E_SEMANTIC_INT_RANGE` added to all 6 out-of-range sites in `semantic.cpp`. All 80 tests pass.
20. Split large source files

---

## Appendix: Key Code Locations

| Feature | Primary Files |
|---------|---------------|
| Type canonicalization | `semantic.cpp:canonicalizeType()` |
| Type compatibility | `semantic.cpp:isCompatible()` |
| Symbol tables | `semantic.h:SymbolTable`, `FunctionSignature`, `StructDefinition` |
| LLVM type mapping | `codegen.cpp:svType()` |
| Cast handling | `codegen.cpp:castTo()` |
| Array bounds check | `codegen.cpp:emitArrayBoundsCheck()` |
| Builtin lowering | `codegen.cpp:genBuiltinCall()` |
| Struct layout | `codegen.cpp:genStructType()` |
| Function codegen | `codegen.cpp:genFunction()` |
| Parser entry | `parser.cpp:parseProgram()` |
| Lexer tokens | `lexer.h:TokenKind` |

---

*End of Audit*