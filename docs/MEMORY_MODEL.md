# Tiq Memory Model

Tiq aims for deterministic memory management without a mandatory garbage collector.

## v0.x direction

- Scalars and small aggregates use value semantics.
- Owned heap values are destroyed at the end of their scope.
- Assignment copies; `move` transfers ownership and invalidates the source (LANGUAGE_SPEC §16.1). Use of a moved binding is a compile-time error.
- Borrowed views cannot outlive their owner.
- Shared ownership is opt-in and implemented by library types.
- Allocation is never inserted merely to satisfy an interface or closure.

## Planned categories

```text
value      copied directly
owned      one owner, movable
borrowed   temporary non-owning view
shared     explicit reference-counted owner
raw        unsafe pointer, opt-in only
```

## Determinism

Destruction order is reverse declaration order within a scope. Early return and error propagation run all applicable destructors. There is no finalizer thread.

## Strings

`str` is an immutable UTF-8 byte view. Owned strings will use a separate type or ownership qualifier. String operations must not assume NUL termination by language contract; the bootstrap backend currently deviates (see `TYPE_SYSTEM.md`).

## Concurrency

No value is implicitly thread-safe. Transfer across tasks requires move, copy, or an explicitly synchronized shared type.

## Explicit allocators

Epic 1 introduces the first user-facing allocator contract through `std/alloc.tiq` (see `SYSTEM_PROGRAMMING_FOUNDATION.md`). Allocators are opaque `u64` handles at the bootstrap C ABI boundary and expose one allocation/deallocation/reset/destroy interface with three strategies:

- General: process heap, per-allocation deallocation.
- Arena: monotonic region, individual deallocation is a no-op, reset invalidates the whole region at once.
- Pool: fixed-size reusable blocks with deterministic exhaustion and double-free rejection.

Allocator creation and allocation are fallible. The raw ABI returns `0`; the stdlib wraps fallible operations in `Result`, so OOM is explicit and does not use exception unwinding. Alignment must be a non-zero power of two.

This allocator slice intentionally does not rewrite current `vec[T]` or `map` allocation yet. Their existing ownership behavior below remains normative until allocator-aware container constructors and destruction semantics are implemented together. No container silently changes allocator in this package.

## Bootstrap

The current compiler slice implements the explicit `move` keyword with use-after-move detection, `defer`, stack arrays, non-owning `TiqSlice` views, and M9.1 borrowed parameters: `&T` / `&mut T` parameter annotations with `&x` / `&mut x` call arguments (LANGUAGE_SPEC §16.3). These borrows live only for the duration of one call and cannot be stored, returned, or re-borrowed, so they cannot outlive their owner; aliasing is checked per call (any number of shared borrows, at most one mutable, never mixed). M9.2-A adds the first scope-bound destruction slice (LANGUAGE_SPEC §16.4): an immutable binding whose initializer is a direct call to a heap-allocating builtin (`fs_read`, `json_encode_str`, `json_get`, `json_arr_get`, `net_fetch`) owns the string, and the emitter frees it at scope end in reverse declaration order, after that scope's defers, in blocks, bracket-loop bodies, and the top-level scope. Mutable bindings are covered by M9.2-D (2026-07-29) under a conservative escape test — a mutable owner whose every assignment is a direct owned-builtin call and whose name otherwise appears only as a builtin argument frees its previous string on reassignment and dies at scope end; bindings that fail the test leak, never dangle. Unbound temporaries are covered by M9.2-F (2026-07-29) in unconditionally evaluated positions of simple statements — an owned-builtin call whose result is not bound is hoisted into a hidden binding and freed at the end of the containing statement when it is the bare statement expression or a builtin argument nested through builtins; temporaries in conditional positions or user-function arguments leak, never dangle. M9.2-B (2026-07-29) makes `break` and `skip` destroy the owned strings of every scope they exit (innermost first, through the enclosing loop body), and M9.2-C (2026-07-29) makes scalar-result functions destroy their body's owners after the result is computed, extended by M9.2-G (2026-07-29) to `str`-result functions whose final expression is a string literal or a direct owned-builtin call (storage provably distinct from every owner), and by M9.2-H (2026-07-29) to `str`-result functions whose final expression is a bare identifier naming a body owner — the named owner transfers to the caller and every other owner is freed after the result is computed; alias-identifier, composite, and conditional results may alias an owner and still leak instead of dangling, and callers do not yet destroy transferred strings. M9.2-I (2026-07-29) closes the call site for the provable cases: a function whose `str` result expression is a heap-builtin call or a bare identifier naming a body owner is *fresh-result*, and a binding initialized from a direct call to one owns the returned string and dies at scope end like a builtin owner (classification happens once before emission and never depends on another function's classification; string-literal results stay static and unowned). M9.2-J (2026-07-29) extends temporary destruction to unbound fresh-result calls: such a call in an unconditionally evaluated position of a simple statement is hoisted and freed at statement end, and the arguments of a fresh-result call are unconditionally evaluated positions too, because its result cannot alias them; calls to functions that are not fresh-result stay outside the rule and leak, never dangle. M9.2-E (2026-07-29) makes a statement-level `proc_exit` destroy the owned strings of every lexically enclosing scope before terminating (the exit code is computed first; owners in calling functions and embedded `proc_exit` uses still leak, never double-free). There is still no general borrow storage or shared ownership. Full ownership checking continues in the remaining M9 packages.

M13.1-P3 vecs (LANGUAGE_SPEC §19.7) are explicitly outside the destruction rules above: the vec handle, its element buffer, and its `str` element copies are never freed by generated code (leak, never dangle), and vec builtins do not participate in the M9.2 owned-string classification.

M13.1-P4 strbufs (LANGUAGE_SPEC §19.8) split the same way: the strbuf handle and its internal byte buffer are never freed by generated code (leak, never dangle), but `str_buf_to_str` returns a fresh heap snapshot that joins the M9.2 owned-string classification like `str_sub` — a binding initialized from a direct `str_buf_to_str` call owns the snapshot and frees it at scope end.

M13.1-P5 maps (LANGUAGE_SPEC §19.9) are entirely outside the destruction rules: the map handle, its bucket array, its insertion-order entry arrays, and its heap key copies are never freed by generated code (leak, never dangle). `map_key_at` returns an interior pointer to the map's own key copy — like `vec_get` on a `str` vec, it is never a destruction-owner and no map builtin participates in the M9.2 owned-string classification.
