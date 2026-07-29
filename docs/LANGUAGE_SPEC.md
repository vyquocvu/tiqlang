# Tiq Language Specification v0.1

Status: Draft, normative for the bootstrap compiler.

## 1. Scope

Tiq is a statically typed, ahead-of-time compiled language for small command-line tools and network services. Version 0.1 deliberately defines a small core. Features not described here are unsupported.

## 2. Source files

- Extension: `.tiq`
- Encoding: UTF-8; v0.1 identifiers are ASCII only.
- Newlines separate statements unless grouping is open.
- `//` starts a line comment.
- Top-level statements execute in source order inside an implicit program entry point.

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
break chan defer false match move mut skip spawn struct true until while
```

`while` and `until` are clause keywords only: they appear in stream generator bounds and predicate slicing (§14). There are no `while`/`for` statement forms (§10). `chan`, `spawn`, `match`, `struct`, and `mut` are reserved for provisional or rejected constructs (§17).

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
->   function definition
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

## 7. Functions

Single-expression function:

```tiq
add a b -> a + b
```

**Type annotations (M12.4)**: parameters may have optional type annotations using `param:type` syntax. An optional return type may follow the parameter list:

```tiq
add a:i32 b:i32 -> i32 -> a + b
```

When annotations are omitted, types are inferred from use. A program whose recursive or exported function type cannot be inferred is rejected.

Supported type annotations:
- Primitive types: `i8`, `i16`, `i32`, `i64`, `u8`, `u16`, `u32`, `u64`, `f32`, `f64`, `bool`, `str`
- Array types: `[T; N]` (e.g., `[int; 3]`)
- Slice types: `[]T` (e.g., `[]int`)

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

## 8. Conditional expressions

```tiq
label = age >= 18 ? "adult" : "minor"
```

The condition must be `bool`. Both branches must have a compatible type.

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

Range iteration:

```tiq
[0..10] { print(i) }
```

Range loops bind an implicit index `i`. An optional binder names the loop variable instead (the binder replaces `i`):

```tiq
[j <- 0..10] { print(j) }
```

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
- `break`: Terminate loop execution immediately (`[0..10] { print(i); break }`).
- `skip`: Skip the remainder of the current iteration (`[1..4] { print(x); skip; x += 100 }`).

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

Omitting the start index defaults to `0`. Omitting the end index defaults to `len(xs)` (or string length for `str`).

Slice bounds are evaluated at runtime. Slicing dynamically enforces 0 <= start <= end <= len. If a slice bound is violated, execution halts deterministically.

The built-in `len()` returns the runtime length of arrays, array slices, and string views:

```tiq
print(len(xs))       // prints 3
print(len(xs[1..3])) // prints 2
```

`len()` accepts seeds of exactly one argument, which must be an array, slice, or string view.

## 14. Stream Generators

Stream generators define infinite or lazy recursive sequences via initial seed values followed by `...` and a windowed combination expression:

```tiq
fib = [0, 1, ... a + b]
first = fib[0]   // 0
tenth = fib[10]  // 55

// Bounded stream generator
powers = [1, ... x * 2 while x < 100]

// Predicate slicing
print(fib[while x < 100])
```

The number of seed elements determines the window size bound to preceding terms in the generator expression (e.g. `a + b` binds the previous two terms). Generators can specify inline termination bounds (`while condition` or `until condition`). Indexing or predicate slicing evaluates or retrieves terms in $O(k)$ time using $O(1)$ state.

The generator expression evaluates in a context that binds exactly these names:

- `a` — the previous term, and `b` — the term before it (generators with two or more seeds);
- `x` — the previous term (single-seed generators);
- `i` — the zero-based index of the term being computed (always available).

No other context names exist. v0.1 windows are limited to the two preceding terms even when more than two seeds are given.

## 15. Errors

Tiq has no exceptions. Fallible functions return a result value.

### 15.1 Option and Result types

`T?` and `T!E` are postfix **type constructors**: `T?` is an optional (`T` or absent), `T!E` is a result (`T` or an error of type `E`).

```tiq
// Option: may hold a value or be absent
find xs:i64 key:i64 -> i64? -> {
  [i <- 0..len(xs)] { xs[i] == key ? i : skip }
  none
}

// Result: may hold a value or an error
parse s:str -> i64!str -> {
  // ... parsing logic ...
  ok(42)   // or err("invalid")
}
```

**Constructors:**
- `some(x)` wraps a value in an Option.
- `none` is the absent Option value.
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
process s:str -> i64? -> {
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

## 17. Provisional constructs and surface status

This section is normative for the bootstrap compiler's observable boundary. Every surface feature falls into exactly one tier:

| Tier | Meaning | Example |
|------|---------|---------|
| **Implemented** | Fully specified, compiled, and tested | `[0..10] { print(i) }`, `move x`, `defer`, `struct`, `f(&mut x)` |
| **Provisional** | Parsed and partially checked; semantics may change | `match` |
| **Fail-closed** | Parsed but rejected at semantic analysis; no code produced | `spawn`, `chan`, `b = &x` |
| **Reserved** | Keyword exists in lexer; no parse path exists yet | `mut` (standalone) |

The bootstrap compiler must reject anything that is not **Implemented** or **Provisional** before code generation, producing a non-zero exit code and a diagnostic with source location.

### 17.1 Match expressions (provisional)

`match` selects the first arm whose pattern compares equal to the scrutinee:

```tiq
x = 10
res = match x { 10 => 100, 20 => 200, _ => 0 }
```

Patterns are equality-compared expressions; there are no binding or destructuring patterns in v0.1. All arm bodies must have the same type. The wildcard pattern `_` matches any value and must be present as the last arm (E07: "match must have a wildcard arm ('_ => ...')"). Unmatched scrutinees without a wildcard are rejected at semantic analysis.

Status: provisional — full pattern matching (guards, destructuring, exhaustiveness for non-wildcard arms) is deferred to M12.6 and M8.

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

## 18. Program entry

Top-level executable statements form the implicit entry point. Libraries may contain definitions only. A future explicit `main` function may be supported but is not required for scripts and tools.

### 18.1 Command-line arguments

Programs read their command-line arguments through two builtins:

- `cli_arg_count()` takes no arguments and returns the number of arguments passed after the program name, as `int`.
- `cli_arg(i)` takes exactly one `int` and returns the `i`-th argument after the program name (0-based) as `str`. An index outside `0 <= i < cli_arg_count()` yields the empty string; it is not a runtime error.

Both are ordinary builtin calls: wrong arity is rejected with E12 and a non-`int` index with E09 at compile time.

## 19. Standard library builtins

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

The pre-existing helpers `json_parse_int` (`str` → `int`, leading-integer parse) and `json_encode_str` (`str` → `str`, quoted and escaped) remain available.

### 19.2 HTTP fetch

`net_fetch(url)` takes exactly one `str` and returns a `str`. It performs a blocking HTTP/1.0 `GET` request with `Connection: close` and returns the response body — the bytes after the response's first blank line — regardless of status code.

- The only supported URL form is `http://host[:port][/path]`; the port defaults to `80` and the path to `/`.
- Any failure yields the empty string and never a runtime error: a non-`http://` scheme, an empty host, name resolution failure, connection failure, a request or read error, or a response without a complete header section.
- The bootstrap implementation uses POSIX sockets (`getaddrinfo`, `socket`, `connect`); it is a documented platform API dependency of generated programs.

Wrong arity is rejected with E12 and a non-`str` argument with E09 at compile time.

## 20. Bootstrap conformance

The bootstrap compiler must reject all unsupported syntax with a non-zero exit code rather than silently generating incorrect code.

