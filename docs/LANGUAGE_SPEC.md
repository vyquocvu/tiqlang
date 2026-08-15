# Tiq Language Specification v0.1

Status: Draft, normative for the bootstrap compiler.

## 1. Scope

Tiq is a statically typed, ahead-of-time compiled language for small command-line tools and network services. Version 0.1 deliberately defines a small core. Features not described here are unsupported.

## 2. Source files

- Extension: `.tiq`
- Encoding: UTF-8; v0.1 identifiers are ASCII only.
- Newlines separate statements unless grouping is open.
- `//` starts a line comment. A line comment is a marker for the developer test runner (`tiq test`, §2.1) when it begins with `//! expected:`.
- Top-level statements execute in source order inside an implicit program entry point.
- A program may span multiple files connected by `import` declarations (§17.6). All loaded files share one flat global namespace and compile into a single unit.

### 2.1 Test annotations

The developer tool `tiq test` (CLI.md) treats a source file as a test when it contains a marker line: a line whose first non-whitespace characters are `//! expected:`. The expected stdout of the program is the concatenation of the marker remainder of each consecutive marker line, joined with a single `\n`:

```tiq
print("hello")        //! expected: hello
```

For output spanning multiple lines, consecutive marker lines join with newlines; every continuation line must also begin with `//! expected:`:

```tiq
[i <- 0..3] { print(i) }   //! expected: 0
//! expected: 1
//! expected: 2
```

The runner builds the file, executes it, strips trailing newlines from the captured stdout, and requires an exact byte match against the expected text. A file without a marker line is not a test and is reported as skipped. Files whose expected text is empty pass when the program writes no output. The marker is a comment to the language itself: it has no effect on lexing, parsing, semantic checking, or code generation.

## 3. Design constraints

1. Familiar arithmetic, comparison, logical, and bitwise operators keep their conventional meanings.
2. The language removes ceremony rather than replacing ordinary programming with code-golf symbols.
3. Parsing must be deterministic without semantic backtracking.
4. Allocation and control flow must be visible in source or derivable without runtime reflection.
5. No mandatory VM, garbage collector, exception runtime, or global async runtime.

## 4. Lexical elements

Identifiers match `[A-Za-z_][A-Za-z0-9_]*`.

Reserved words in v0.1 (matching the lexer exactly):

```text
break chan defer enum extern false import match move mut none skip spawn struct true until while
```

`while` and `until` are clause keywords only: they appear in stream generator bounds and predicate slicing (§14). There are no `while`/`for` statement forms (§10). `chan`, `spawn`, `match`, `struct`, and `mut` are reserved for provisional or rejected constructs (§17). `enum` declares named integer constant sets (§17.5). `import` loads another source file into the program (§17.6). `extern` declares a foreign C function (§7.1). `none` is a literal (§15.1).

Literals:

```tiq
42
-7
3.14
true
false
none
"hello"
```

Literals:

```tiq
42
-7
3.14
true
false
"hello"
```

Escape sequences: `\\`, `\"`, `\n`, `\r`, `\t`, and `\0`. Each decodes to the escaped character in the string's value. Any other escape sequence is rejected at lex time with E01 (`unsupported escape sequence`).

## 5. Operators

Tiq preserves conventional operators:

```text
+ - * / %
== != < <= > >=
&& || !
& | ^ << >>
+= -= *= /= %=
```

Tiq-specific operators:

```text
=    immutable definition
<-   mutable definition or reassignment
:    type annotation (params, return, fields, record literals)
->   function definition / body introduction
? :  conditional expression
..   half-open range
...  stream generator expansion
=>   match arm (§17.1); lambda reserved for a later v0.x
??   optional fallback; reserved for a later v0.x
```

`!` is logical negation. Its operand must be `bool` and the result is `bool`; Tiq has no truthiness. Printing is not an operator — it is the `print` builtin (§12).

## 6. Bindings

Immutable definition:

```tiq
port = 8080
```

Mutable definition and reassignment:

```tiq
count <- 0
count <- count + 1
count += 1
```

Reading an uninitialized binding, redefining a name in the same scope, or assigning to an immutable binding is a compile-time error.

**`<-` never shadows an existing binding.** For `name <- expr`, the compiler
resolves `name` from the nearest lexical scope outward and then applies
exactly one outcome:

```text
nearest binding is mutable   => reassign that binding
nearest binding is immutable => compile-time error
no binding anywhere          => declare a new mutable binding in the current scope
```

There is no fallback from an immutable collision to a new mutable
declaration, and nested scopes never create a hidden local `x`: every
`<-` below refers to the same outer mutable binding.

```tiq
x <- 1

{
    x <- 2

    {
        x <- 3
    }
}

print(x)   // 3
```

Reassigning an immutable binding through `<-` (whether declared in the
same scope or an outer scope) is rejected with `error[E11]: cannot mutate
immutable binding '<name>'`. The compiler never reinterprets such a
statement as a new mutable declaration.

An immutable `=` definition may not redefine a name that already exists
in any enclosing lexical scope; `x = 1` then `x = 2` (or `x <- 1` then
`x = 2`) fails closed with `error[E11]: cannot redefine binding '<name>'`.

Functions do not close over module-level bindings. The v0.1 emitter
representss module-level bindings as locals of `main`, so a function
body may not read or reassign a top-level binding: reading one fails
closed at the host C compiler stage, and the result is left unspecified
for this slice. Reassignment of a module-level mutable binding from
inside a function therefore has no supported meaning.

## 7. Functions

Single-expression function:

```tiq
add a b -> a + b
```

**Type annotations (M12.4/M25)**: parameters may have optional type annotations using `param:type` syntax. An optional return type may follow the parameter list, using `:` for type information and reserving `->` for body introduction (issue #8):

```tiq
add a: i32 b: i32 : i32 -> a + b
```

When annotations are omitted, types are inferred from use. A program whose recursive or exported function type cannot be inferred is rejected.

Supported type annotations:
- Primitive types: `i8`, `i16`, `i32`, `i64`, `u8`, `u16`, `u32`, `u64`, `f32`, `f64`, `bool`, `str`
- Array types: `[T; N]` (e.g., `[int; 3]`)
- Slice types: `[]T` (e.g., `[]int`)
- Container types (M13.1-P8, function parameter and return position only): `vec[T]` with `T` one of `int`, `str`, or a named struct; `strbuf`; `map`. `vec[T]` uses the existing `[`/`]` tokens so the annotation grammar stays LL(1) with no new token kinds. `vec` without `[T]` is rejected with E09 ("vec annotation requires an element type: vec[T]"). Container annotations are not valid struct field types (fail closed). Borrow prefixes (`&`/`&mut`) are rejected on container annotations with E23: container bindings are already reference-semantics handles (§19.7–§19.9), so passing one by value copies the handle and the callee mutates the same underlying container as the caller (shared-handle semantics).

The value of the final expression is the function result.

```tiq
max a b -> a > b ? a : b
```

A function body may also be a block (§9); the block's final expression is the result:

```tiq
classify n -> {
  n < 0 ? "neg" : n > 0 ? "pos" : "zero"
}
```

Recursive functions are allowed after their complete signature can be inferred.

### 7.1 Extern declarations (FFI)

A top-level `extern` declaration binds a name to a function defined by the
host C environment (M16.1/M16.2):

```tiq
extern "C" llabs x: i64 : i64      // fully annotated params, mandatory return
extern "C" strlen s: str : i64
extern "C" getpid : i64            // zero-param form: extern-only exception
```

Rules:

- Top-level only. `extern` inside a block has no parse path and fails closed (E05).
- The ABI operand must be the string literal `"C"`. A missing/non-string operand is a parse error (E04); any other string content is rejected with E29 (`extern ABI must be "C"`).
- Every parameter must carry a type annotation (E29 otherwise). Borrow prefixes (`&`/`&mut`) on extern parameters are rejected with E23.
- The return type is mandatory; the declaration ends at the return type (no body).
- Zero parameters are allowed — the only surface form in v0.1 with a zero-parameter signature (user functions require at least one parameter).
- The declared name must not duplicate another extern declaration or collide with an existing function, struct, or enum name (E29).

FFI-safe signature types (the M16.2 C ABI mapping). Anything not listed —
arrays, slices, `vec[T]`, `strbuf`, `map`, Option/Result, streams — fails
closed with E29 (`extern parameter type is not FFI-safe` / `extern return
type is not FFI-safe`):

| Tiq type | C type |
|----------|--------|
| `i8`, `i16`, `i32`, `i64` | `int8_t`, `int16_t`, `int32_t`, `int64_t` |
| `u8`, `u16`, `u32`, `u64` | `uint8_t`, `uint16_t`, `uint32_t`, `uint64_t` |
| `f32`, `f64` | `float`, `double` |
| `bool` | `int64_t` (current backend representation) |
| `str` | `const char *` |
| named struct | its emitted C typedef, passed by value |

Pointers: v0.1 has no first-class pointer type. Pointer values cross the
boundary as `u64` (address-as-integer); a real pointer type is deferred to a
later v0.x.

Calls to extern functions type-check exactly like user-function calls:
arity mismatches are E12 and argument types are checked against the declared
parameter types (E09).

Ownership: an extern `str` result is not a heap-allocating builtin and not a
fresh result (§16.4); binding one never frees it. The policy is leak,
never dangle.

Emission: each compiler emits `extern <ret> <name>(<params>);` in
declaration order, immediately after the enum constants and before the
stream-generator forward declarations (M13_DETERMINISM.md §1). Zero-param
declarations emit `(void)`. Programs without extern decls emit byte-identical
C to before this feature.

Preamble shadowing: the generated C preamble includes system headers whose
declarations of common names use types Tiq's fixed-width table cannot spell
(`size_t`/`pid_t`/`int`/`void` returns). Redeclaring such a name with the
fixed-width ABI would conflict with the header prototype, so both compilers
suppress the prototype for exactly these names and let the header
declaration serve for codegen and linking:

```text
clock close exit fork getpid getppid memcmp rand read sleep strcmp strlen time write
```

Linking external libraries: `tiq build`/`tiq run` accept repeatable
`-l <lib>` and `-L <dir>` options forwarded to the host C compiler (CLI.md).

Status: implemented (M16.1/M16.2). M16.3 (header tooling) and M16.4 (dlopen)
remain queued.

## 8. Conditional expressions

```tiq
label = age >= 18 ? "adult" : "minor"
```

The condition must be `bool`. Both branches must have a compatible type.

### 8.1 Bracket Conditions

Single-branch conditional execution uses `?[condition] { body }` or the single-line form `?[condition] statement`. The condition must be `bool`. The body executes once if the condition is `true`; it is a pure conditional (not a loop). `break` and `skip` inside a bracket condition apply to the nearest enclosing bracket loop (`[...]`); a `break` or `skip` with no enclosing bracket loop is a compile-time error.

```tiq
?[count > 10] {
    print("over limit")
}

?[done] break
```

## 9. Blocks

Indentation is not semantic in v0.1. Multi-statement blocks use braces:

```tiq
clamp x lo hi -> {
  x < lo ? lo : x > hi ? hi : x
}
```

A block evaluates to its final expression. Statements before the final expression end with a newline or `;`.

## 10. Loops

Tiq uses unified **Bracket Loops (`[domain] { body }`)** for iteration; there are no `for` or `while` statement forms. The loop header in `[ ]` takes a full expression; the body is a `{ }` block whose statements are separated by newlines or `;`.

Range iteration requires an explicit binder that names the loop variable:

```tiq
[i <- 0..10] { print(i) }
```

A bare range domain without a binder (`[0..10] { ... }`) is rejected at compile time (E15: "range loop requires an explicit binder: use [name <- domain]"). Every loop variable must have a visible binding site.

Loop variables are immutable inside the body; assigning to them is an error (E11). Binders are only valid for range domains; `[j <- condition]` is rejected with `loop binder requires a range domain` (E15).

Multiple binders iterate the Cartesian product as nested loops; later binders may reference earlier ones, `break`/`skip` apply to the innermost loop, and duplicate binder names are rejected (`duplicate loop binder`, E15):

```tiq
[j <- 0..3, k <- 0..j] { print(j * 10 + k) }
```

Conditional iteration:

```tiq
[count < 10] { count += 1 }
```

Loop control statements:
- `break`: Terminate loop execution immediately (`[i <- 0..10] { print(i); break }`).
- `skip`: Skip the remainder of the current iteration (`[x <- 1..4] { print(x); skip; x += 100 }`).

Inline guards (`break if condition`, `skip if condition`) are planned but not implemented; the bootstrap compiler rejects them (fail closed).

## 11. Primitive types

Planned core primitive types:

```text
bool
u8 u16 u32 u64
 i8 i16 i32 i64
f32 f64
str
unit
never
```

Integer literals default to `i64` unless the context requires another integer type. Floating literals default to `f64`. Integer literals that do not fit the resolved type are rejected at compile time.

### `str` representation

`str` is defined as an immutable UTF-8 byte sequence identified by pointer and length. It is not NUL-terminated by language contract.

**Bootstrap deviation (v0.1)**: the C backend currently represents `str` as a NUL-terminated `const char *`. Only slices and string views (`TiqSlice`) use the pointer+length model. This means `len()` on a `str` literal calls `strlen()` under the hood. The end state — a pure pointer+length view with no NUL dependency — is scheduled for the M12 backend migration. Programs must not rely on the NUL-termination behavior; it is an implementation artifact, not a language guarantee.

## 12. The `print` builtin

`print(expression)` writes the expression's value and a trailing newline to standard output. It is an ordinary builtin function call, not dedicated syntax, and takes exactly one argument:

```tiq
print(42)          // prints 42
print("hello")     // prints hello
print(fib[10])     // prints 55
```

Output formatting is determined by the argument's static type: integers print in decimal, floats with shortest-round-trip style (`%g`), `bool` as `true`/`false`, `str` and string/array views as their bytes. Arguments whose type has no printable representation (arrays, streams, functions) are rejected at compile time.

`print` returns the number of bytes written as `int`. Historical note: earlier drafts overloaded statement-position `!` as a print statement; that form was removed — `!` is logical negation only (§5).

## 13. Arrays and Slices

Array literals are written with square brackets and comma-separated elements:

```tiq
xs = [1, 2, 3]
```

All elements must have the same type. The element type is inferred from the literal. A single-element array `[x]` is a valid array of length one. An empty array `[]` is rejected at compile time because the element type cannot be inferred. Array access uses indexing syntax:

```tiq
first = xs[0]
```

The index must be an `int` value. Multi-dimensional arrays are not supported in v0.1.

An array fill `[value; length]` constructs an array whose elements are all `value`:

```tiq
zeros = [0; 4]   // [0, 0, 0, 0]
```

In v0.1 the fill length must be an integer literal.

Array bounds are checked unless the compiler proves the access safe. An explicit unsafe facility is deferred.

### 13.1 Slices and String Views

Slices construct non-owning dynamic views over contiguous sequence elements (array elements or string characters). Slicing syntax uses half-open range notation inside index brackets:

```tiq
sub = xs[1..3]   // slice of xs from index 1 (inclusive) to 3 (exclusive)
tail = xs[1..]   // slice from index 1 to end
head = xs[..2]   // slice from start (0) to index 2
full = xs[..]    // full slice view of all elements
```

Slicing a string `s[i..j]` returns a `str` view over the designated byte range (a `TiqSlice` in the C backend).

**String byte indexing**: indexing a string or string view with a single integer index `s[i]` returns the **raw byte value** at position `i` as an `int`. This is a byte position, not a Unicode code point or character. For ASCII strings the values match the character codes; for multi-byte UTF-8 sequences, individual bytes may fall within the continuation range (128–191) and do not represent valid Unicode scalars on their own.

```tiq
s = "abc"
print(s[0])   // prints 97  (ASCII code of 'a')
print(s[1])   // prints 98  (ASCII code of 'b')
```

To work with Unicode code points, split by `len()` and slice rather than index.

**Byte-index bounds (M13.1-P8).** A single-index byte read `s[i]` is valid exactly when `0 <= i < len(s)` (byte length). An out-of-range index aborts deterministically, following the array-indexing precedent: the program prints `tiq: index <i> out of bounds for string of length <len>` to standard error and exits with code 1 — never undefined behavior. The check is evaluated at runtime.

Omitting the start index defaults to `0`. Omitting the end index defaults to `len(xs)` (or string length for `str`).

Slice bounds are evaluated at runtime. Slicing dynamically enforces 0 <= start <= end <= len. If a slice bound is violated, execution halts deterministically.

The built-in `len()` returns the runtime length of arrays, array slices, and string views:

```tiq
print(len(xs))       // prints 3
print(len(xs[1..3])) // prints 2
```

`len()` accepts seeds of exactly one argument, which must be an array, slice, or string view.

## 14. Stream Generators

Stream generators define infinite or lazy recursive sequences via initial seed values followed by `...` and a windowed combination expression with explicit binders:

```tiq
fib = [0, 1, ... (a, b) -> a + b]
first = fib[0]   // 0
tenth = fib[10]  // 55

// Bounded stream generator
powers = [1, ... (x) -> x * 2 while x < 100]

// Predicate slicing
print(fib[while x < 100])
```

The generator expression is preceded by an explicit binder list `(w1, w2) ->` (one or two window binders) that names the preceding terms bound in the expression. An optional index binder may follow after a semicolon: `(w1, w2; idx) ->`. The number of seed elements determines the window size. Generators can specify inline termination bounds (`while condition` or `until condition`). Indexing or predicate slicing evaluates or retrieves terms in $O(k)$ time using $O(1)$ state.

Every binder in a stream generator must be explicitly named; the compiler does not inject any implicit names. A stream generator without explicit binders is rejected at compile time (E23: "stream generators require explicit binders: use (name) -> expr").

## 15. Errors

Tiq has no exceptions. Fallible functions return a result value.

### 15.1 Option and Result types

`T?` and `T!E` are postfix **type constructors**: `T?` is an optional (`T` or absent), `T!E` is a result (`T` or an error of type `E`).

```tiq
// Option: may hold a value or be absent
find xs: i64 key: i64 : i64? -> {
  [i <- 0..len(xs)] { xs[i] == key ? i : skip }
  none
}

// Result: may hold a value or an error
parse s: str : i64!str -> {
  // ... parsing logic ...
  ok(42)   // or err("invalid")
}
```

**Constructors:**
- `some(x)` wraps a value in an Option.
- `none` is the absent Option value (a reserved literal; it cannot name a declared identifier).
- `ok(x)` wraps a value in a Result.
- `err(e)` wraps an error in a Result.

**Fallback operator (`??`):**
`a ?? b` evaluates to `a` if `a` is present/ok, otherwise `b`. It short-circuits — the right operand is evaluated only when the left operand is absent or an error. Its precedence sits between `||` and `?:` (tighter than `?:`, looser than `||`), so `a ?? b ? x : y` parses as `(a ?? b) ? x : y`.

```tiq
x = find(xs, 5) ?? -1   // -1 if not found
```

**Propagation (`expr?`):**
If `expr` is absent or an error, the enclosing function returns that state immediately; otherwise `expr?` evaluates to the unwrapped value. The enclosing function's return type must itself be optional/result-compatible, checked at compile time.

```tiq
process s: str : i64? -> {
  x = parse(s)?   // returns none if parse fails
  some(x * 2)
}
```

**Pattern matching:**
A `T?` or `T!E` value destructures with `match`:

```tiq
result = find(xs, key)
msg = match result {
  some(v) => "found",
  none => "not found",
  _ => "unreachable"
}
```

Status: implemented — Option/Result types, constructors, fallback, and propagation are fully specified, compiled, and tested.

## 16. Memory

Values have deterministic scope-based destruction. Scalars and small aggregates use value semantics.

### 16.1 Move semantics

The `move` keyword transfers ownership of a binding to a new location:

```tiq
x <- [1, 2, 3]
y <- move x   // ownership transferred to y
```

After a move, the source binding is invalidated. Any subsequent use of the source produces a compile-time error. Moving an immutable binding (`=`) is also a compile-time error.

Compound assignment resets the moved state, allowing the binding to be reused:

```tiq
x <- [1, 2, 3]
y <- move x
x += 1   // x is valid again with a new value
```

Heap allocation and shared ownership are specified in `MEMORY_MODEL.md` and are not part of the bootstrap slice. Borrowed parameters are specified in §16.3.

### 16.2 Deferred actions

The `defer` keyword schedules an expression to run when the enclosing block exits:

```tiq
{
    print(1)
    defer print(2)
    print(3)
}
// Output:
// 1
// 3
// 2
```

Deferred actions execute in reverse declaration order. `defer` is only valid inside `{ }` blocks. A `defer` outside a block is a compile-time error.

### 16.3 Borrowed parameters

A function parameter may be declared as a borrow with `&type` (shared, read-only) or `&mut type` (exclusive, mutable):

```tiq
bump r:&mut i64 -> {
    r <- r + 1
}
show v:&i64 -> print(v)

n <- 41
bump(&mut n)
show(&n)     // 42
```

At the call site the argument for a borrowed parameter must be written `&x` or `&mut x`, where `x` names a binding in scope; expressions cannot be borrowed. The borrow kind must match the parameter exactly.

Inside the callee a borrowed parameter reads like a value of the referent type. Reassignment (`r <- expr`, `r += expr`, ...) through a `&mut` parameter updates the caller's binding. Reassignment through a shared `&` parameter is a compile-time error.

Rules (all violations are compile-time errors, code E23 unless noted):

- `&mut x` requires `x` to be a mutable binding (`<-`).
- A borrow argument requires a `&`/`&mut` parameter; passing `&x` to a value parameter is rejected.
- A `&`/`&mut` parameter requires a borrow argument; passing a plain value is rejected.
- Within one call, a binding may be borrowed shared any number of times, but at most once mutably, and never both mutably and shared (aliasing check).
- Borrowing a moved binding is a use-after-move error (E18).
- Borrowed parameters cannot be re-borrowed, stored in bindings, or returned; borrows are only valid in call argument position (E07 elsewhere).

Every borrow ends when the call returns. Because borrows cannot be stored, returned, or re-borrowed, no borrow can outlive its referent; the lifetime rule is enforced structurally.

### 16.4 Scope-bound destruction of owned strings

An immutable binding (`=`) whose initializer is a direct call to a heap-allocating builtin — `fs_read`, `json_encode_str`, `json_get`, `json_arr_get`, `net_fetch`, or any other builtin documented as returning a heap-allocated `str` owned per this section (§19), including `str_sub`, `fs_list`, and `str_buf_to_str` — owns the resulting string. Owned strings are destroyed at the end of the enclosing scope, in reverse declaration order, after that scope's deferred actions have run.

Heap-allocating builtins always return fresh heap storage, including their empty-string failure results. If the runtime cannot allocate, the program terminates deterministically with exit code 1 and the message `tiq: out of memory`.

A binding initialized from another binding (`b = a`) aliases without owning; only the owner is destroyed. Aliases and views cannot outlive the owner because they live in the same or an inner scope.

`break` and `skip` destroy the owned strings of every scope they exit — innermost first, from the jump statement's own scope through the enclosing loop body — before transferring control. Only owners already bound at the jump point are destroyed.

Inside a function whose result type is scalar (an integer type, a float type, or `bool`), owned strings of the body's outermost scope are destroyed before the function returns: after the body's deferred actions, and after the result value has been computed. A function whose result may carry a pointer destroys its owners only when the result provably cannot alias one: a `str`-result function whose final expression is a string literal or a direct call to a heap-allocating builtin (both produce storage distinct from every owner) destroys its owners after the result is computed. A `str`-result function whose final expression is a bare identifier naming an owner of the body's outermost scope transfers that owner to the caller: every owner holds distinct fresh storage, so the function destroys every *other* owner after the result is computed and returns the named owner's string undestroyed (the caller does not yet destroy transferred results; they leak at the call site). Any other pointer-carrying result — an alias identifier, a composite value, a conditional — may alias an owner, so those functions leak instead of dangling.

A mutable (`<-`) binding owns its string under a conservative escape test: its initializer and every later `<-` assignment to it must be a direct call to a heap-allocating builtin, and its name may otherwise appear only as an argument to standard-library builtins. A qualifying mutable owner destroys its previous string when reassigned — after the new value is computed, so self-referencing reassignments such as `s <- json_get(s, "k")` are safe — and is destroyed at scope end like an immutable owner. A mutable binding that fails the test never destroys anything: it may leak; it never double-frees or dangles.

A user function is *fresh-result* when its result type is `str` and its result expression — the final expression of a block body, or the body of an expression-bodied function — is either a direct call to a heap-allocating builtin or a bare identifier naming an owner of the body's outermost scope. Both cases return heap storage distinct from every value the caller can already reach, so a binding whose initializer is a direct call to a fresh-result function owns the returned string exactly like a binding initialized from a heap-allocating builtin: it is destroyed at the end of its scope in reverse declaration order, and a mutable such binding owns it under the same escape test — whose reassignment rule stays restricted to heap-allocating builtins, so a mutable later reassigned from a fresh-result function call destroys nothing (it may leak). A function whose result expression is a string literal is not fresh-result (the storage is static), and a call to any other function never creates an owner; ownership classification never depends on another user function's classification.

A binding whose initializer is a conditional expression owns the result when both branches are *owning expressions* — direct calls to heap-allocating builtins, direct calls to fresh-result functions, or nested conditional expressions whose branches are all owning expressions. Exactly one branch is evaluated, and every branch produces distinct fresh heap storage, so the binding owns whichever string the selected branch returns. A mutable binding with such an initializer qualifies under the same escape test (its reassignment rule still requires a direct heap-builtin call, so a mutable later reassigned from a conditional destroys nothing). A conditional whose condition or either branch is not an owning expression does not create an owner; it may leak, never dangle.

A statement-level `proc_exit(code)` call destroys the owned strings of every scope lexically enclosing the call — innermost first, up to the current function body or the top-level program scope — before terminating. The exit code is computed before any destruction runs. Only owners already bound at the call site are destroyed; owners in calling functions are not (they leak). A `proc_exit` embedded in a larger expression terminates without destruction.

A temporary — a call to a heap-allocating builtin or to a fresh-result function whose result is not bound — is destroyed at the end of the statement that contains it, provided it appears in an unconditionally evaluated position of a simple statement (a binding, an assignment, or an expression statement): as the bare statement expression itself, or as a direct argument to a standard-library builtin or to a fresh-result function, nested to any depth through such calls. A fresh-result function returns storage the caller cannot already reach, so its result can neither alias one of its own arguments nor be reachable after the statement ends. The temporary's value is computed into a hidden binding before the statement runs and freed immediately after it; hoisted temporaries evaluate left to right, deterministically.

A conditional expression whose branches are both owning expressions — appearing as a direct argument to a standard-library builtin or to a fresh-result function, or as the bare statement expression — is itself a temporary: it is computed into a hidden binding before the statement and freed immediately after it, exactly like an owning call in the same position. Exactly one branch is evaluated, and every branch produces distinct fresh heap storage, so the hidden binding holds a valid heap pointer regardless of which branch was taken. A conditional whose branches are not both owning expressions is not hoisted; it may leak, never dangle.

Temporaries in any other position — match arms, loop headers, arguments to functions that are not fresh-result, conditionals with non-owning branches — are not destroyed: freeing them could dangle (such a function may return its argument), so they leak; they never double-free or dangle.

Bootstrap limits: temporaries outside the positions above are not destroyed, owners in calling functions are not destroyed by `proc_exit`, and a `str` result that is neither fresh-result nor bound at the call site is not destroyed. These paths leak; they never double-free or dangle.

## 17. Provisional constructs and surface status

This section is normative for the bootstrap compiler's observable boundary. Every surface feature falls into exactly one tier:

| Tier | Meaning | Example |
|------|---------|---------|
| **Implemented** | Fully specified, compiled, and tested | `[i <- 0..10] { print(i) }`, `move x`, `defer`, `struct`, `enum`, `f(&mut x)`, `extern "C" llabs x:i64 : i64` |
| **Provisional** | Parsed and partially checked; semantics may change | `match` |
| **Fail-closed** | Parsed but rejected at semantic analysis; no code produced | `spawn`, `chan`, `b = &x` |
| **Reserved** | Keyword exists in lexer; no parse path exists yet | `mut` (standalone) |

The bootstrap compiler must reject anything that is not **Implemented** or **Provisional** before code generation, producing a non-zero exit code and a diagnostic with source location.

### 17.1 Match expressions and pattern matching (implemented)

`match` selects the first arm whose pattern matches the scrutinee:

```tiq
x = 10
res = match x { 10 => 100, 20 => 200, _ => 0 }
```

Patterns are a first-class syntactic category (not arbitrary expressions). The following pattern forms are supported:

**Wildcard pattern** (`_`): Matches any value. Must be present as the last arm (E07: "match must have a wildcard arm").

**Irrefutable-last rule (Pre-M13 S1)**: A bare binding pattern (`x`) is also irrefutable — it matches every value — and must likewise be the last arm. Earlier irrefutable arms (wildcard or bare binding) make every later arm unreachable and are rejected with E07 ("irrefutable pattern must be the last arm").

**Literal pattern** (`10`, `"hello"`, `true`, `none`): Matches when the scrutinee equals the literal value. For `none`, the scrutinee must be an Option or Result type. String literal patterns use byte-equality (`tiq_str_eq`) on the value bytes — never C pointer equality — so heap-allocated strings (the result of `str_cat`, `fs_read`, etc.) compare correctly across allocations.

**Binding pattern** (`x`): A bare identifier creates a fresh immutable binding scoped to the arm body. The binding's type is the scrutinee type.

```tiq
res = match x {
    0 => 10,
    v => v * 2  // v binds to x
}
```

**Constructor pattern** (`some(v)`, `ok(v)`, `err(e)`): Matches Option or Result types and destructures the inner value:
- `some(v)` requires an Option scrutinee, binds the inner value to `v`
- `ok(v)` requires a Result scrutinee, binds the success value to `v`
- `err(e)` requires a Result scrutinee, binds the error value to `e`

The payload pattern `p` is lowered recursively: the constructor's outer tag is tested first, then `p` is tested against the inner value at the corresponding field path (`_t.value` for `some`/`ok`, `_t.error` for `err`). Nested constructors (`some(some(v))`, `ok(0)`) therefore test the full payload structure, not just the outer tag.

```tiq
opt = some(42)
res = match opt {
    some(v) => v,
    none => 0,
    _ => -1
}
```

**Enum variant pattern** (`Color.Red`): Matches when the scrutinee equals the enum variant constant. The scrutinee must be an integer type (enum variants are i64 constants). An enum variant pattern against a non-integer scrutinee is rejected with E09 ("enum variant pattern requires integer scrutinee, found str") before code generation.

```tiq
enum Color { Red, Green, Blue }
c = Color.Red
name = match c {
    Color.Red => "red",
    Color.Green => "green",
    Color.Blue => "blue",
    _ => "unknown"
}
```

All arm bodies must have the same type. Pattern bindings are immutable (E11 on assignment). Duplicate binding names within a single pattern are rejected. Shadowing follows existing binding rules.

Status: implemented in M17.1. Guards and exhaustiveness checking beyond wildcard requirement are deferred.

### 17.2 Struct types and field access (implemented)

Struct definitions declare a nominal record type with named, typed fields:

```tiq
struct Point {
  x: i32,
  y: i32
}
```

Record literals construct values of a struct type:

```tiq
p = Point { x: 1, y: 2 }
```

Field access reads a field from a struct value:

```tiq
print(p.x)  // 1
```

Rules:
- Struct names must be unique within a module (E09: "duplicate struct definition").
- Field names must be unique within a struct (E09: "duplicate field").
- Record literals must initialize all fields exactly once (E09: "missing field" / "unknown field").
- Field access on a non-struct type is rejected (E09: "field access on non-struct type").
- Field access with an unknown field name is rejected (E09: "unknown field").

Status: implemented — struct definitions, record literals, and field access are fully specified, compiled, and tested.

### 17.3 Fail-closed constructs

The following constructs **parse** successfully but are **rejected during semantic analysis** with a located diagnostic (fail closed; no executable is produced):

| Construct | Error | Blocking milestone |
|-----------|-------|--------------------|
| `spawn expr` | E07: "spawn is not supported yet" | M7 (concurrency runtime) |
| `chan T` | E07: "chan is not supported yet" | M7 (concurrency runtime) |
| `&x` outside call arguments | E07: "borrow is only valid as an argument to a reference parameter" | M9 (borrow extensions: stored/returned borrows) |

The parser accepts these forward-compatibility spellings so that future milestones can promote them to implemented without grammar breakage.

### 17.4 Reserved keywords

These keywords are recognized by the lexer and reserved for future milestones. They have **no parse path** for their intended construct: programs that attempt to use them in a structural position are rejected with a parse error (E04/E05):

| Keyword | Reserved for |
|---------|-------------|
| `mut` | Used in borrow syntax (`&mut x`, `&mut T`, §16.3) — `mut` alone outside a borrow prefix is rejected |
| `while` | Stream generator bounds and predicate slicing (`§14`) only; no `while` loop statement |
| `until` | Stream generator bounds only; no `until` loop statement |
| `chan` | M7 channel type — lexes but semantic-rejects (§17.3) |
| `spawn` | M7 concurrent spawn — lexes but semantic-rejects (§17.3) |

### 17.5 Enum declarations (implemented)

`enum` declares a named set of integer constants at top level:

```tiq
enum Color { Red, Green, Blue }
```

- Variants are bare identifiers, comma-separated, auto-numbered from `0` in declaration order (`Red = 0`, `Green = 1`, `Blue = 2`).
- Explicit variant values (`= n`), payloads (tagged unions), and generics are not supported; attempting them is a parse error (fail closed).
- A variant is referenced as `Name.Variant` and is a plain `i64` value, usable anywhere an `i64` is: bindings, comparisons, match scrutinees and patterns, function arguments.
- In field-access position (`Name.Variant`), an identifier that names a declared enum resolves to the enum; the enum takes precedence over any same-named value binding.
- An enum must be declared before its first use (top-level source order), like structs.
- `enum` declarations are accepted at top level only; `enum` inside a block is a parse error (E05).
- The C backend emits each enum as an anonymous C enumeration of constants named `tiq_enum_<Name>_<Variant>` in declaration order, and a `Name.Variant` expression emits its constant name. Emission is deterministic (declaration order, no hashing). An enum with zero variants is legal and emits no constants.

Rules (all violations are compile-time errors with source location):
- Enum names must be unique within a module (E24: "duplicate enum definition").
- An enum name must not collide with a struct name, in either declaration order (E24: "enum '…' conflicts with struct '…'" / "struct '…' conflicts with enum '…'").
- An enum name must not collide with a value binding (function, binding, or parameter) in the same scope (E24: "'Name' is already defined as a value" / "'Name' is already defined as an enum"). This prevents the same identifier from denoting both a type-level enum and a value-level binding.
- Variant names must be unique within an enum (E25: "duplicate variant").
- Referencing a variant that does not exist (`Name.X`) is rejected (E26: "unknown variant").
- Using a bare enum name as a value (for example `x = Color`) is rejected (E09: "enum 'Color' is not a value; use Color.<variant>").

Status: implemented — enum declarations and variant references are fully specified, compiled, and tested (M13.1-P2).

### 17.6 Modules (`import`) (implemented)

`import` loads another Tiq source file into the program:

```tiq
import "lib/util.tiq"
```

Syntax and position:
- The operand is a **string literal** path; anything else is a parse error (E04: "expected string literal path after 'import'"). Escape sequences inside the path are not interpreted: the characters between the quotes are used verbatim.
- Imports are allowed **only at the top of a file**, before any other top-level item. An `import` after a non-import item is rejected (E04: "import must appear before any other top-level item"). `import` inside a block is a parse error (E05).
- There are no aliases, re-exports, or conditional imports.

Resolution and loading:
- The path is resolved **relative to the directory of the importing file** (POSIX `/` separators).
- Modules are loaded depth-first from the root file, in import declaration order.
- Each file is loaded **at most once**: files are deduplicated by canonical (fully resolved) filesystem path, so the same file imported via different relative spellings loads a single time. Re-imports are skipped silently (diamond imports are legal).
- A missing or unreadable module is rejected with E27 ("module not found"), reporting the path as written and the source location of the `import`.
- A cyclic import chain is rejected with E28 ("circular import"), reporting the cycle chain.

Semantics:
- All loaded modules share **one flat global namespace**: top-level functions, structs, enums, and bindings from every module are visible everywhere. Duplicate top-level definitions across modules are rejected with the same diagnostics as duplicates within one file (E09 duplicate struct, E24 duplicate enum / enum-struct collision).
- The program compiles into a **single C translation unit**. Modules are emitted in dependency post-order (imported files first; DFS by import declaration order, first visit wins), so top-level statements of an imported module execute before the statements of its importer. Emission is deterministic and contains no filesystem paths.
- Declaration-before-use rules (§17.2, §17.5) apply to the post-order concatenation of all modules.

Rules (all violations are compile-time errors with source location):
- The imported file must exist and be readable (E27: `module not found: "<path as written>"`).
- The import graph must be acyclic (E28: `circular import: a.tiq -> b.tiq -> a.tiq`).
- `import` must precede every other top-level item in its file (E04).
- The import operand must be a string literal (E04).

Status: implemented — multi-file programs, canonical-path dedupe, cycle detection, and deterministic post-order emission are fully specified, compiled, and tested (M13.1-P6).

### 17.7 Standard-library modules (`std/`) and gated builtins (implemented)

A subset of the builtins in §19 is **gated**: they are recognized as compiler intrinsics only inside a `std/` module, and user code reaches them by importing the matching standard-library module.

Gated domains and their modules:
- `import "std/json.tiq"` — `json_parse_int`, `json_encode_str`, `json_get`, `json_arr_len`, `json_arr_get`, `json_has`, `json_set`, `json_del` (§19.1).
- `import "std/net.tiq"` — `net_fetch`, `net_listen`, `net_accept`, `net_connect`, `net_recv`, `net_send`, `net_close`, `net_port`, `net_shutdown`, `http_method`, `http_path`, `http_header` (§19.2–§19.3).
- `import "std/ev.tiq"` — `ev_add`, `ev_wait`, `ev_ready` (§19.4).
- `import "std/dl.tiq"` — `dl_open`, `dl_sym`, `dl_call` (§19.11).

Behavior:
- Outside a `std/` module, calling a gated builtin without importing its module is a compile-time `error[E08]: undefined symbol '<name>'` whose message carries a hint naming the module to import (for example, `— import "std/json.tiq" for JSON operations`). With the import present, the call resolves to the wrapper function defined in that module, which has the same name and signature as the builtin.
- Inside a `std/` module the builtin is recognized directly, so each wrapper body calls the intrinsic.
- Core builtins remain always available with no import: `print`, `eprint`, `len`, `str_cat`, `int_str`, `str_sub`, `str_sub_code`, `str_eq`, `fs_read`, `fs_write`, `fs_exists`, `fs_list`, `proc_exec`, `proc_exit`, `cli_arg_count`, `cli_arg`, `clock_ms`, and the `vec_*`, `str_buf_*`, and `map_*` families.
- Two domain builtins stay ungated because they cannot be wrapped in a Tiq function: `json_view` (§19.1) returns a zero-copy `str_view` for which there is no function-return annotation, and `ev_loop` (§19.4) takes no parameters while Tiq has no zero-parameter function syntax. `dl_error` (§19.11) is likewise ungated for the zero-parameter reason.

Import resolution for `std/`: the path is first resolved relative to the importing file (§17.6); if that fails, it is retried from the current working directory, so `import "std/<mod>.tiq"` resolves from any file depth when `tiq` is invoked from the project root.

Status: implemented — gating, the four `std/` modules, the cwd import fallback, and the diagnostic hint are specified, compiled, and tested (M15, M16.4).

### 17.8 Extern declarations (FFI) (implemented)

`extern "C"` top-level declarations bind foreign C functions with the full
C ABI type mapping, prototype emission, and fail-closed rejection of unsafe
signatures. The complete normative text is §7.1.

Status: implemented — lexer keyword, grammar production, E29 semantic
checks, deterministic prototype pass (both compilers), and `-l`/`-L` link
options are specified, compiled, and tested (M16.1/M16.2).

### 17.9 Reserved builtin names (visible binding principle)

The following names are reserved by the compiler and cannot be redefined by user code as bindings, function names, or parameters:

- Core builtins: `print`, `eprint`, `len`, `str_cat`, `int_str`, `str_sub`, `str_sub_code`, `str_eq`, `fs_read`, `fs_write`, `fs_exists`, `fs_list`, `proc_exec`, `proc_exit`, `cli_arg_count`, `cli_arg`, `clock_ms`.
- Container families: `vec_new`, `vec_push`, `vec_get`, `vec_set`, `vec_len`, `vec_pop`, `str_buf_new`, `str_buf_append`, `str_buf_to_str`, `str_buf_len`, `map_new`, `map_set`, `map_get`, `map_has`, `map_len`, `map_key_at`, `map_val_at`.
- Option/Result constructors: `some`, `ok`, `err`.
- Reserved literals: `none`.

A program that attempts to define a function or binding whose name matches a reserved builtin is rejected at compile time (E09: "'name' is a reserved builtin name"). This ensures every identifier reference resolves to a visible binding site — no user definition can silently shadow a builtin.

## 18. Program entry

Top-level executable statements form the implicit entry point. Libraries may contain definitions only. A future explicit `main` function may be supported but is not required for scripts and tools.

### 18.1 Command-line arguments

Programs read their command-line arguments through two builtins:

- `cli_arg_count()` takes no arguments and returns the number of arguments passed after the program name, as `int`.
- `cli_arg(i)` takes exactly one `int` and returns the `i`-th argument after the program name (0-based) as `str`. An index outside `0 <= i < cli_arg_count()` yields the empty string; it is not a runtime error.

Both are ordinary builtin calls: wrong arity is rejected with E12 and a non-`int` index with E09 at compile time.

### 18.2 Package manifests

A package manifest is an INI-style `*.tiq.toml` file describing a package. The format has three recognized sections — `[package]`, `[deps]`, and `[tests]` — and the body of each is a sequence of `key = value` lines, `#` comment lines, and blank lines. Values are the trimmed text after the first `=`, with one layer of double or single quotes stripped when present; a value may be empty. A line that is neither blank, a comment, a section header, nor `key = value` (no `=` or an empty key) is malformed.

The manifest rules (M14.4, `src/tiq/manifest.tiq`; see `docs/CLI.md` "Package manifests"):

- `[package]` must appear exactly once and must contain a non-empty `name`. A package name is a non-empty string of ASCII letters, digits, `-`, `_`, and `.` and must not be `.` or `..`. Valid `[package]` keys are `name`, `version`, `description`, `license`, `repository`, and `src`; `version`, when present, must be `major.minor.patch` — three dot-separated runs of digits with no empty part.
- `[deps]` entries are `name = source` pairs. The dependency name must be a valid package name (same rules as `[package] name`). The source must be non-empty and use one of the following forms: `path:<dir>` (a local directory), `git:<url>` (a git repository, default branch), `git:<url>#<ref>` (a git repository at a specific branch, tag, or version constraint), or `registry:<name>` / `registry:<name>#<constraint>` (a package from the Tiq registry); a bare value without a recognized scheme prefix is treated as a path source. When `git:` sources contain a `#` separator, the ref part after `#` is either a branch/tag name or a version constraint. Version constraints use semver format: `1.2.3` (exact), `>=1.0.0`, `<=2.0.0`, `>1.0.0`, `<2.0.0`, `!=1.5.0`, or comma-separated combinations like `>=1.0.0,<2.0.0`. When a version constraint is provided, `tiq install` resolves it against the repository's git tags (with optional `v` prefix stripped) and clones the highest matching version. For `registry:` sources, `tiq install` queries the registry at `http://127.0.0.1:7070` for the package metadata, selects the highest version satisfying the constraint (or the latest if no constraint), fetches the source URL, and installs accordingly.
- `[tests]` accepts the keys `dir` and `include`.
- `name` and `version` must not be duplicated within `[package]`. Unknown sections and unknown keys within a known section fail closed.

Manifest errors are reported to standard error in the standard located form `path:line: error[E30]: message` — `line` is 1-based and points at the offending line (structural errors such as a missing `[package]` or `name` are reported at line 1). E30 is a tool-local code used by `tiq init --check`, `tiq install`, `tiq search`, `tiq publish`, `tiq yank`, and `tiq audit` (LANGUAGE_SPEC §18.2 context); it is never emitted by the compiler itself. A manifest is valid only when it parses and satisfies every rule above; `tiq init --check` exits 0 for a valid manifest and 1 otherwise. `tiq init [name]` scaffolds a deterministic template (default `name = "my-package"`, `version = "0.1.0"`, `description = "A Tiq package"`, `[tests] dir = "tests"`) and refuses to overwrite an existing file. `tiq install` reads `tiq.toml`, resolves each `[deps]` entry into `.tiq-deps/<name>/` (copying for `path:` sources, `git clone --depth 1` for `git:` sources, registry query for `registry:` sources), and writes a `tiq.lock` lockfile with FNV-1a content hashes for each resolved dependency. `tiq search [query]` queries the registry for packages matching the query string (substring match); `tiq registry [port]` starts the package registry server (default port 7070). `tiq publish [--registry <url>]` reads the manifest, extracts the package name, version, and source URL (from `repository`, `src`, or `path:.` default), and publishes to the registry via PUT. `tiq yank [--registry <url>] <name> <version>` removes a specific version from the registry via DELETE. `tiq audit` verifies dependency integrity: checks the lockfile exists, every manifest dep has a lockfile entry, every lockfile entry has a manifest dep, and each installed dep's FNV-1a content hash matches the lockfile.

### 18.3 Libraries and C embedding (M16.3)

A *library* is a module intended to be compiled into a C host program rather than run as a Tiq program. Libraries contain **definitions only** — function definitions, struct definitions, enum definitions, and extern declarations (`import` statements resolve to module statements as usual and are not counted). The first top-level executable statement fails closed with `path:line: error[E31]: library requires definitions only; top-level statement is not allowed`, reported at the offending statement, byte-identical in both compilers. Library mode is enforced by two commands; plain `tiq build`/`tiq run` are unchanged:

- `tiq emit-c --lib <file.tiq>` runs the normal `emit-c` pipeline but omits the generated `int main`, so the emitted translation unit links into a host program. The preamble, definitions, stream generators, and function definitions are otherwise unchanged.
- `tiq emit-header <file.tiq> [-o output]` emits a deterministic C header declaring the library's export surface (stdout by default, a file with `-o`).

The header's **export surface** is every top-level user function whose parameters and return type are all FFI-safe per the §7.1 ABI table: `i8`–`i64`, `u8`–`u64`, `f32`, `f64`, `bool`, `str`, and named structs passed by value. Functions with unsupported signatures (`vec`, arrays, slices, `map`, `option`/`result`, borrowed parameters), stream-generator bodies, and extern declarations are skipped from the header but remain callable from Tiq code. There is no export keyword: every FFI-safe function is exported. Unannotated parameters and returns follow the emitted-C convention (`int64_t`) and are spelled identically by the header prototype and the `--lib` definition.

The header shape is fixed and deterministic (declaration order):

```c
/* Generated by tiq emit-header from <basename>. Do not edit. */
/* Returned str values are Tiq-owned: callers must not free them (leak, never dangle). */
#ifndef TIQ_<GUARD>_H
#define TIQ_<GUARD>_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* one typedef per top-level user struct, declaration order */
/* one prototype per exported function; zero params -> (void) */

#ifdef __cplusplus
}
#endif

#endif
```

The guard is derived from the input basename (directory stripped, trailing `.tiq` stripped, empty → `lib`), uppercased, with non-alphanumeric characters mapped to `_`. Struct typedefs are included so struct-by-value parameters compile; enum constants are omitted (enums cross the boundary as `int64_t`). The `extern "C"` guards make the header usable from C++ hosts. Ownership follows §16.4: a returned `str` is Tiq-owned and the C caller must not free it — the value leaks when the host is done with it and never dangles within a call.

Both commands are implemented in lock-step in the C bootstrap and the self-hosted compiler; `tests/ffi.sh` pins the header golden, the E31 diagnostic, the usage errors, and an end-to-end embedding build, and `tests/selfhost_emit_c.sh` byte-compares the header output and pins the `--lib` structure across compilers.

## 19. Standard library builtins

Most domain builtins described below (`json_*`, `net_*`, `http_*`, `ev_*`, `dl_*`) are **gated** behind a `std/` module import (§17.7): they are compiler intrinsics only inside a `std/` module, and user code accesses them by importing `std/json.tiq`, `std/net.tiq`, `std/ev.tiq`, or `std/dl.tiq`. Core builtins (`print`, string/`fs`/`proc`/`cli`/`clock_ms`, and the `vec_*`/`str_buf_*`/`map_*` families) and the non-wrappable builtins `json_view`, `ev_loop`, and `dl_error` remain available without any import.

### 19.1 JSON access

`json_get(json, key)` takes exactly two `str` arguments and returns a `str`. It performs a single deterministic scan of `json`, which must be a JSON object, and looks up `key` among the object's top-level members:

- A string value yields its decoded contents: the escapes `\"`, `\\`, `\/`, `\n`, `\t`, and `\r` are decoded; any other `\x` yields `x`.
- A number, `true`, `false`, or `null` value yields its verbatim token text (for example `"42"`, `"true"`).
- An object or array value yields the raw balanced sub-document text, so nested members are read by chaining `json_get(json_get(j, "outer"), "inner")`.
- A missing key, an input that is not a JSON object, or malformed input yields the empty string. `json_get` never raises a runtime error.

Key comparison is exact and byte-wise; keys in `json` containing escape sequences are not decoded before comparison. Wrong arity is rejected with E12 and non-`str` arguments with E09 at compile time.

JSON arrays are read with two companion builtins:

- `json_arr_len(json)` takes one `str` and returns the number of top-level elements of the JSON array `json` as `int`. An input that is not a JSON array or is malformed yields `0`.
- `json_arr_get(json, i)` takes a `str` and an `int` and returns the `i`-th top-level element (0-based) of the JSON array `json` as `str`, using the same value-extraction rules as `json_get` (decoded strings, verbatim scalar tokens, raw balanced sub-documents). An index outside `0 <= i < json_arr_len(json)`, a non-array input, or malformed input yields the empty string. Neither builtin raises a runtime error.

All three builtins reject wrong arity with E12 and wrong argument types with E09 at compile time.

`json_view(json, key)` takes exactly two `str` arguments and returns a `str` view (a non-owning `TiqSlice`). It performs the same single deterministic scan of `json` as `json_get` and looks up `key` among the object's top-level members, but returns a view into the original buffer with no allocation:

- A string value yields the raw bytes between the opening and closing quotes, without decoding escape sequences.
- A number, `true`, `false`, or `null` value yields its verbatim token text.
- An object or array value yields the raw balanced sub-document text.
- A missing key, an input that is not a JSON object, or malformed input yields an empty view (length zero).

The returned view aliases its `json` argument and must not outlive the string it was derived from. `json_view` is not a heap-allocating builtin and does not participate in the ownership rules of §16.4. Because the result aliases its argument, `json_view` is not a destruction-safe callee: temporaries passed to it are not hoisted and freed (they leak rather than dangle). Wrong arity is rejected with E12 and non-`str` arguments with E09 at compile time.

`json_has(json, key)` takes exactly two `str` arguments and returns a `bool`. It performs the same scan as `json_get` and returns `true` if `key` is present among the object's top-level members (regardless of the member's value), and `false` otherwise — including when `json` is not a JSON object or is malformed. It allocates nothing. Wrong arity is rejected with E12 and non-`str` arguments with E09 at compile time.

`json_set(json, key, val)` takes exactly three `str` arguments and returns a heap-allocated `str` (owned per §16.4). It produces a JSON object string with the member `key` set to the raw JSON value `val`:

- If `json` is not a JSON object (does not start with `{`), the result is `{"key":val}` (a fresh single-member object).
- If `key` already exists among the object's top-level members, its value is replaced in situ (the surrounding structure is copied verbatim).
- If `key` is absent, the member `"key":val` is appended before the closing `}`.

The `val` argument is inserted verbatim as raw JSON text; callers are responsible for quoting string values (e.g. `json_encode_str`). No validation of `val` is performed. Wrong arity is rejected with E12 and non-`str` arguments with E09 at compile time.

`json_del(json, key)` takes exactly two `str` arguments and returns a heap-allocated `str` (owned per §16.4). It produces a JSON object string with the member `key` removed:

- If `json` is not a JSON object or `key` is absent, the input is returned unchanged (as a fresh heap copy).
- If `key` is present, the member and its surrounding comma (leading or trailing) are removed.

Wrong arity is rejected with E12 and non-`str` arguments with E09 at compile time.

The pre-existing helpers `json_parse_int` (`str` → `int`, leading-integer parse) and `json_encode_str` (`str` → `str`, quoted and escaped) remain available.

### 19.2 HTTP fetch

`net_fetch(url)` takes exactly one `str` and returns a `str`. It performs a blocking HTTP/1.1 `GET` request with `Connection: close` and returns the response body — the bytes after the response's first blank line — regardless of status code. When the response headers declare `Transfer-Encoding: chunked` (header name and value matched case-insensitively), the chunked framing is decoded and the concatenated chunk data is returned; chunk-size extensions and trailers are ignored.

- The only supported URL form is `http://host[:port][/path]`; the port defaults to `80` and the path to `/`.
- Any failure yields the empty string and never a runtime error: a non-`http://` scheme, an empty host, name resolution failure, connection failure, a request or read error, a response without a complete header section, or malformed chunked framing.
- The bootstrap implementation uses POSIX sockets (`getaddrinfo`, `socket`, `connect`); it is a documented platform API dependency of generated programs.

Wrong arity is rejected with E12 and a non-`str` argument with E09 at compile time.

### 19.3 TCP socket primitives

Low-level blocking TCP socket builtins for building simple servers and clients. All operate on `127.0.0.1` (loopback) only. File descriptors are represented as `int`; invalid operations return `-1` (or the empty string for `net_recv`) and never raise a runtime error.

- `net_listen(port)` takes one `int` and returns a listening socket fd bound to `127.0.0.1:port`, or `-1`. Port `0` requests an OS-assigned ephemeral port (query it with `net_port`).
- `net_accept(fd)` takes one `int` (a listening socket) and returns a new connection fd, or `-1`. Blocks until a client connects.
- `net_connect(port)` takes one `int` and returns a connection fd to `127.0.0.1:port`, or `-1`.
- `net_recv(fd)` takes one `int` and returns a `str` (heap-allocated, owned per §16.4). Performs a single `read` of up to 4096 bytes; returns the bytes read, or the empty string on EOF or error.
- `net_send(fd, data)` takes an `int` and a `str` and returns the number of bytes written, or `-1`.
- `net_close(fd)` takes one `int` and returns `0` on success, `-1` on error.
- `net_port(fd)` takes one `int` and returns the actual port number of the socket, or `-1`.
- `net_shutdown(fd)` takes one `int` and shuts down the write end of the socket (the peer sees EOF), returning `0` on success, `-1` on error.

All reject wrong arity with E12 and non-`int` arguments with E09 at compile time (`net_send` checks `int`, `str`). The bootstrap implementation uses POSIX sockets (`socket`, `bind`, `listen`, `accept`, `connect`, `read`, `write`, `close`, `shutdown`, `getsockname`); these are documented platform API dependencies of generated programs.

Two companion builtins parse the request line of an HTTP request (the first line, `METHOD PATH HTTP/VERSION`):

- `http_method(req)` takes one `str` and returns the method token (for example `"GET"`) as a heap-allocated `str` (owned per §16.4). If the input contains no space, the entire input is returned; an empty input yields the empty string.
- `http_path(req)` takes one `str` and returns the path token (for example `"/index.html"`) as a heap-allocated `str` (owned per §16.4). If the input contains fewer than two spaces, the empty string is returned.

Both never raise a runtime error. Wrong arity is rejected with E12 and non-`str` arguments with E09 at compile time.

`http_header(req, name)` takes exactly two `str` arguments and returns the value of the named header as a heap-allocated `str` (owned per §16.4). It scans the request line-by-line (lines separated by `\r\n` or `\n`) for a line starting with `name` followed by `:` (case-insensitive match on the header name). The returned value is the text after the colon and optional leading whitespace, up to the end of the line. If the header is not found, the empty string is returned. Wrong arity is rejected with E12 and non-`str` arguments with E09 at compile time.

### 19.4 Event loop (kqueue)

A minimal readiness-notification loop for monitoring multiple file descriptors without threads. The bootstrap implementation uses `kqueue`/`kevent` (a documented macOS/BSD platform API); Linux `epoll` support is deferred.

- `ev_loop()` takes no arguments and returns an `int` event-loop handle (a kqueue fd). The handle is a process-wide singleton: repeated calls return the same fd. Returns `-1` on failure.
- `ev_add(loop, fd)` takes two `int` arguments and registers `fd` for read-readiness monitoring (`EVFILT_READ`, `EV_ADD`). Returns `0` on success, `-1` on error.
- `ev_wait(loop, timeout_ms)` takes two `int` arguments and blocks until at least one monitored fd is readable or `timeout_ms` milliseconds elapse (a negative timeout blocks indefinitely). Returns the number of ready fds (≥ 1), `0` on timeout, or `-1` on error.
- `ev_ready(loop, fd)` takes two `int` arguments and returns `1` if `fd` was reported readable by the most recent `ev_wait` on that loop, `0` otherwise.

All reject wrong arity with E12 and non-`int` arguments with E09 at compile time. Invalid operations return `-1` (or `0` for `ev_ready`) and never raise a runtime error.

### 19.5 String utilities

- `str_cat(a, b)` takes exactly two `str` arguments and returns their concatenation as a heap-allocated `str` (owned per §16.4). Either argument may be empty. The result is a fresh buffer of length `len(a) + len(b)`.
- `int_str(n)` takes exactly one `int` argument and returns its decimal string representation as a heap-allocated `str` (owned per §16.4). For example, `int_str(42)` yields `"42"` and `int_str(-7)` yields `"-7"`.
- `str_sub(s, start, end)` takes a `str` and two `int` arguments and returns the bytes of `s` in the half-open range `[start, end)` as a heap-allocated `str` (owned per §16.4). The range is valid exactly when `0 <= start` and `start <= end` and `end <= len(s)`; any invalid range (`start < 0`, `end < start`, or `end > len(s)`) yields the empty string — always fresh heap storage, never a runtime error.
- `str_eq(a, b)` takes exactly two `str` arguments and returns `true` if the two strings are byte-for-byte equal and `false` otherwise, as `bool`. It allocates nothing.

Wrong arity is rejected with E12 and wrong argument types with E09 at compile time (`int_str` checks `int`; `str_sub` checks `str`, `int`, `int`).

### 19.6 System utilities

- `eprint(s)` takes exactly one `str` argument and writes its bytes and a trailing newline to standard error, formatted exactly like `print` (§12) formats a `str` for standard output. It returns the number of bytes written as `int` (`-1` on a write error). It allocates nothing.
- `fs_list(dir)` takes exactly one `str` argument and returns the entry names of directory `dir` as a heap-allocated `str` (owned per §16.4). The entries `.` and `..` are excluded; the remaining names are sorted bytewise (`strcmp` order, mandatory for deterministic output) and joined with `\n`, with no trailing newline. A nonexistent or unreadable directory, or a directory with no entries, yields the empty string — always fresh heap storage, never a runtime error. The bootstrap implementation uses POSIX `opendir`/`readdir`/`closedir`; these are documented platform API dependencies of generated programs.
- `clock_ms()` takes no arguments and returns the number of milliseconds since an arbitrary, unspecified epoch as `int`. The clock is monotonic — consecutive reads never decrease, so the difference of two reads is always non-negative — and reads require no OS level-of-detail beyond that guarantee. It is intended for elapsed-time measurement (e.g. the `tiq bench` developer tool); because the epoch and rate are unspecified, an absolute value carries no meaning. The bootstrap implementation uses POSIX `clock_gettime(CLOCK_MONOTONIC)`; that is a documented platform API dependency of generated programs.

Wrong arity is rejected with E12 and non-`str` arguments with E09 at compile time.

### 19.7 Growable arrays (Vec)

A vec is a growable, heap-allocated array. Vec values are created and manipulated exclusively through builtin functions; the only vec type syntax is the `vec[T]` function annotation of §7/§19.10 (M13.1-P8) — there are no vec type expressions anywhere else:

- `vec_new()` takes no arguments and returns a new empty vec backed by the default general allocator.
- `vec_with_allocator(allocator)` takes exactly one `int` argument (allocator handle) and returns a new empty vec backed by the specified allocator.
- `vec_push(v, x)` appends `x` to the end of `v` and returns the new length as `int`.
- `vec_get(v, i)` returns the element at index `i` (0-based).
- `vec_set(v, i, x)` overwrites the element at index `i` with `x` and returns `0` as `int`.
- `vec_len(v)` returns the number of elements as `int`.
- `vec_pop(v)` removes and returns the last element.

A vec binding holds a handle with reference semantics: the builtins mutate the vec through the handle, so an immutable binding to a vec permits `vec_push`/`vec_set`/`vec_pop` on its contents (only rebinding the name is restricted).

**Element typing.** A vec's element type `T` is fixed by the first `vec_push` on that binding. `T` must be `int`, `str`, or a named struct; a first push of any other type is rejected with E09 ("vec_push element must be int, str, or a struct"). Every later `vec_push` and `vec_set` element must match `T` exactly — including the struct name for struct elements — and `vec_get`/`vec_pop` return `T`. A mismatched element is rejected with E09 at compile time. Only `vec_push` establishes `T`: calling `vec_get`, `vec_set`, or `vec_pop` on a vec whose element type was never established is rejected fail-closed with E09 ("... on a vec with no established element type (no vec_push yet)"). `vec_len` is permitted on an unestablished vec and returns `0` elements' worth of length. All builtins reject wrong arity with E12, a non-vec first argument with E09 (except constructors which check their allocator argument), and a non-`int` index with E09 at compile time.

**Runtime behavior.** Storage grows by doubling from an initial capacity of 8 elements; growth order is deterministic. Out-of-range access aborts deterministically instead of invoking undefined behavior: `vec_get` or `vec_set` with `i < 0` or `i >= vec_len(v)` prints `tiq: vec index <i> out of bounds for vec of length <len>` to standard error and exits with code 1; `vec_pop` on an empty vec prints `tiq: vec_pop on empty vec` to standard error and exits with code 1. Struct elements are copied into and out of the vec by value (a shallow copy of the struct's fields). `str` elements are copied on `vec_push`/`vec_set` (the vec owns copies allocated using its assigned allocator), so a vec never retains a pointer into a string it does not own; `vec_get`/`vec_pop` on a `str` vec return pointers into the vec's copies, which are not owned by the caller.

**Memory policy.** Vec storage follows leak-never-dangle: neither the vec handle, its element buffer, nor its `str` element copies are ever freed by generated code, and vecs do not participate in the §16.4 owned-string destruction rules (`vec_new` / `vec_with_allocator` are not owned-string builtins, and `vec_get`/`vec_pop` results are never destruction-owners). When created with `vec_with_allocator(a)`, destroying or resetting allocator `a` reclaims all backing memory at once.

### 19.8 String builder (StrBuf)

A strbuf is a growable, heap-allocated byte buffer for building strings incrementally in amortized linear time (repeated `str_cat` is O(n²); repeated `str_buf_append` is O(n)). Strbufs are created and manipulated exclusively through builtin functions; the only strbuf type syntax is the `strbuf` function annotation of §7/§19.10 (M13.1-P8):

- `str_buf_new()` takes no arguments and returns a new empty strbuf backed by the default general allocator.
- `str_buf_with_allocator(allocator)` takes exactly one `int` argument (allocator handle) and returns a new empty strbuf backed by the specified allocator.
- `str_buf_append(sb, s)` appends the bytes of the `str` argument `s` to the end of `sb` and returns the new length in bytes as `int`. Appending the empty string is a no-op that returns the current length. The argument's bytes are copied into the buffer; the strbuf never retains a pointer to `s`.
- `str_buf_to_str(sb)` returns the current contents of `sb` as a heap-allocated `str` (owned per §16.4). The result is a fresh snapshot copy: later appends to `sb` do not change a previously taken snapshot, and taking a snapshot does not disturb the buffer.
- `str_buf_len(sb)` returns the current length in bytes as `int`.

A strbuf binding holds a handle with reference semantics: the builtins mutate the buffer through the handle, so an immutable binding to a strbuf permits `str_buf_append` on its contents (only rebinding the name is restricted).

**Typing.** The handle type is `strbuf` (typed IR: `TYPE_STRBUF`); it is not parametrized. The first argument of `str_buf_append`, `str_buf_to_str`, and `str_buf_len` must be a strbuf, `str_buf_with_allocator` requires an `int` allocator handle, and the second argument of `str_buf_append` must be a `str`; anything else is rejected with E09 at compile time. All builtins reject wrong arity with E12.

**Runtime behavior.** The internal buffer is NUL-terminated and grows by doubling from an initial capacity of 16 bytes; growth order is deterministic and `str_buf_append` is amortized O(1) per appended byte. Allocation failure aborts deterministically through the runtime allocator: `tiq: out of memory` to standard error, exit code 1 — never undefined behavior. No strbuf operation raises any other runtime error.

**Memory policy.** The strbuf handle and its internal buffer follow leak-never-dangle: generated code never frees them, and the handle does not participate in the §16.4 owned-string destruction rules. The `str_buf_to_str` result, by contrast, is a fresh process-heap string owned per §16.4 exactly like `str_sub`: an immutable binding initialized from a direct `str_buf_to_str` call owns the snapshot and it is freed at scope end in reverse declaration order. When created with `str_buf_with_allocator(a)`, destroying or resetting allocator `a` reclaims the buffer and handle memory. See `MEMORY_MODEL.md`.

### 19.9 Hash map (Map)

A map is a growable, heap-allocated hash table from `str` keys to `int` values. Keys are always `str` and values are always `int` — there are no other key or value types; values beyond `int` are expressed as indices into vecs (§19.7) by user code. Maps are created and manipulated exclusively through builtin functions; the only map type syntax is the `map` function annotation of §7/§19.10 (M13.1-P8):

- `map_new()` takes no arguments and returns a new empty map backed by the default general allocator.
- `map_with_allocator(allocator)` takes exactly one `int` argument (allocator handle) and returns a new empty map backed by the specified allocator.
- `map_set(m, k, v)` associates the `str` key `k` with the `int` value `v` and returns the number of distinct keys after the operation as `int`. If `k` is already present, its value is overwritten and its iteration position is unchanged.
- `map_get(m, k)` returns the value associated with `k` as `int`, or `-1` if `k` is absent. A missing key is never a runtime error; programs that store `-1` as a legitimate value must use `map_has` to distinguish absence.
- `map_has(m, k)` returns `true` if `k` is present and `false` otherwise, as `bool`. It allocates nothing.
- `map_len(m)` returns the number of distinct keys as `int`.
- `map_key_at(m, i)` returns the key at insertion-order position `i` (0-based) as `str`.
- `map_val_at(m, i)` returns the value at insertion-order position `i` (0-based) as `int`.

A map binding holds a handle with reference semantics: the builtins mutate the map through the handle, so an immutable binding to a map permits `map_set` on its contents (only rebinding the name is restricted).

**Insertion-order iteration.** The first `map_set` of a key fixes that key's iteration position permanently; overwriting the value of an existing key never moves it. `map_key_at`/`map_val_at` with `0 <= i < map_len(m)` therefore visit the keys in exactly the order in which they were first inserted, identical across runs and platforms — iteration order never depends on hash values, bucket layout, or rehashing. There is no delete operation in v1: scoped tables (for example, compiler symbol tables) are expressed by snapshotting `map_len` at scope entry and iterating only positions below the snapshot after scope exit (the len-snapshot scoping idiom); stale keys above a snapshot are simply left in place.

**Typing.** The handle type is `map` (typed IR: `TYPE_MAP`); it is not parametrized — keys are fixed `str` and values fixed `int`. The first argument of every builtin except `map_new` must be a map, the key argument of `map_set`/`map_get`/`map_has` must be a `str`, the value argument of `map_set` must be an `int`, and the index argument of `map_key_at`/`map_val_at` must be an `int`; anything else is rejected with E09 at compile time ("<builtin> argument: expected map, found <T>", "<builtin> key: expected str, found <T>", "map_set value: expected int, found <T>", "<builtin> index: expected int, found <T>"). All seven builtins reject wrong arity with E12.

**Runtime behavior.** The table hashes keys with FNV-1a 64-bit using the standard fixed constants (offset basis `14695981039346656037`, prime `1099511628211`) — never seeded from the environment — and resolves collisions with open addressing and linear probing over a power-of-two bucket array (initial 8 buckets). Entries additionally live in a separate insertion-order array, so iteration never touches bucket order. The load factor is kept at or below 0.7: exceeding it doubles the bucket count and rehashes all entries in insertion order, which is fully deterministic. Key bytes are copied into a fresh heap allocation on first insert (the map owns its keys and never retains a pointer to a caller's string). Allocation failure aborts deterministically through the shared runtime allocator (`tiq: out of memory` to standard error, exit code 1). `map_key_at` or `map_val_at` with `i < 0` or `i >= map_len(m)` prints `tiq: map index <i> out of bounds for map of length <len>` to standard error and exits with code 1 — never undefined behavior.

**Ownership of `map_key_at` results.** `map_key_at` returns an interior pointer to the map's own key copy, exactly like `vec_get` on a `str` vec (§19.7) — not a fresh copy, and therefore *not* owned per §16.4 (a binding initialized from `map_key_at` never frees it). This is safe because map keys are never freed or moved after insert (leak-never-dangle), and it keeps the S3 symbol-table iteration hot path allocation-free.

**Memory policy.** The map handle, its bucket array, its entry arrays, and its key copies follow leak-never-dangle: generated code never frees them, and no map builtin participates in the §16.4 owned-string destruction rules. This is an accepted bootstrap leak, documented in `MEMORY_MODEL.md`.

### 19.10 Containers across function boundaries (M13.1-P8)

Container values cross function boundaries through the §7 annotation syntax: `vec[T]` (with `T` ∈ `int`, `str`, named struct), `strbuf`, and `map` are accepted in parameter position (`param:vec[int]`) and return position (`: vec[int] ->`). Annotations are required for containers: an unannotated parameter can never be inferred as a container type (fail closed).

**Handle semantics.** A container argument passes its handle by value: caller and callee share the same underlying container, so mutations made through builtins in the callee are visible to the caller afterwards. Because the handle is already a reference, borrow prefixes on container annotations are rejected with E23 ("container parameters are reference-semantics handles; '&' is not allowed").

**Vec element typing at boundaries.** An annotated `vec[T]` parameter is *established* with element type `T` inside the callee — `vec_get`/`vec_set`/`vec_pop` work immediately, and pushes of a different element type are E09 as usual. At a call site: an argument vec that is already established must match `T` exactly (nominal, struct name included) or the call is rejected with E09 ("argument N: expected vec<T>, found vec<U>") at the call's location; an argument vec that is *not* yet established (no `vec_push` before the call) is established as `vec<T>` by the call, exactly as a first `vec_push` would establish it — the P3 unestablished-vec rules (§19.7) remain coherent because the annotation supplies the missing element type. A non-vec argument for a `vec[T]` parameter is E09 ("argument N: expected vec<T>, found <U>").

**Vec returns.** A function annotated `: vec[T] ->` returns an established `vec<T>`: call results carry the full element type, so the caller may `vec_get` immediately. The function body's result is checked against the annotation; an expression body whose established vec element type differs from `T` is rejected with E09 ("return type mismatch: expected vec<T>, found vec<U>"). For block bodies the result is checked at kind level (a non-vec result is E09); the annotation is authoritative for callers.

**strbuf/map at boundaries.** `strbuf` and `map` are unparametrized: an argument for a `strbuf`/`map` parameter must be a strbuf/map (E09 otherwise), and `: strbuf ->` / `: map ->` returns type-check the body the same way. Shared-handle semantics apply: a `map_set` in the callee is visible via `map_get` in the caller.

**Arity.** Calls to container-typed functions check arity like any other user function: wrong argument count is E12.

### 19.11 Dynamic library loading (`std/dl.tiq`) (implemented)

Runtime dynamic loading of native libraries through four builtins, gated behind `import "std/dl.tiq"` (§17.7):

```text
dl_open(path: str) : u64
dl_sym(handle: u64, name: str) : u64
dl_error() : str
dl_call(sym: u64, a: i64, b: i64, c: i64, d: i64, e: i64, f: i64) : i64
```

- `dl_open` loads the library at `path` with `RTLD_NOW | RTLD_LOCAL` and returns the handle as a `u64`; a failed load returns `0`.
- `dl_sym` resolves the symbol `name` in `handle` and returns its address as a `u64`; a failed lookup returns `0` (POSIX caveat: a data symbol legitimately residing at address 0 is indistinguishable from failure; function symbols are never NULL, so function lookups are unambiguous).
- `dl_error` returns the text of the last loader error, or `""` when there is none. The string is Tiq-owned. It is a core builtin (no import required) because zero-parameter functions cannot be defined in Tiq, so it cannot be wrapped (§17.7).
- `dl_call` invokes the function at address `sym` through the integer register ABI: it casts the address to `i64(i64, i64, i64, i64, i64, i64)` and passes the six arguments. Callees with fewer parameters ignore the extra register arguments. `dl_call(0, ...)` returns `0` without calling anything (never calls NULL).

**Failure semantics.** Runtime failures never abort the program and produce no diagnostics: they surface as `0`/`""` returns, and the reason is available from `dl_error()` (the same convention as `fs_read`). Compile-time errors are the standard builtin checks: gating (E08, with the import hint), arity (E12), and argument types (E09).

**Pointer values.** Handles and symbol addresses cross the boundary as `u64` (§7.1 pointer decision); pass them between `dl_open`, `dl_sym`, and `dl_call` without conversion.

**Limitations.** `dl_call` covers integer/pointer signatures only (valid on x86-64 and ARM64): f64-returning symbols and struct-by-value parameters are out of scope. There is no `dl_close` in v0.1 — handles live for the process lifetime. No `-l dl` flag is appended automatically; macOS (libSystem) and glibc ≥ 2.34 resolve `dlopen` without it, and older glibc needs `-l dl` via the M16.2 link options.

Status: implemented — builtins, gating, the `std/dl.tiq` wrapper module, runtime helpers, and fail-closed tests in both compilers (M16.4).

## 20. Bootstrap conformance

The bootstrap compiler must reject all unsupported syntax with a non-zero exit code rather than silently generating incorrect code.

