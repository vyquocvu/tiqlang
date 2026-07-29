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

## Bootstrap

The current compiler slice implements the explicit `move` keyword with use-after-move detection, `defer`, stack arrays, non-owning `TiqSlice` views, and M9.1 borrowed parameters: `&T` / `&mut T` parameter annotations with `&x` / `&mut x` call arguments (LANGUAGE_SPEC §16.3). These borrows live only for the duration of one call and cannot be stored, returned, or re-borrowed, so they cannot outlive their owner; aliasing is checked per call (any number of shared borrows, at most one mutable, never mixed). M9.2-A adds the first scope-bound destruction slice (LANGUAGE_SPEC §16.4): an immutable binding whose initializer is a direct call to a heap-allocating builtin (`fs_read`, `json_encode_str`, `json_get`, `json_arr_get`, `net_fetch`) owns the string, and the emitter frees it at scope end in reverse declaration order, after that scope's defers, in blocks, bracket-loop bodies, and the top-level scope. Mutable bindings are covered by M9.2-D (2026-07-29) under a conservative escape test — a mutable owner whose every assignment is a direct owned-builtin call and whose name otherwise appears only as a builtin argument frees its previous string on reassignment and dies at scope end; bindings that fail the test leak, never dangle. Unbound temporaries are covered by M9.2-F (2026-07-29) in unconditionally evaluated positions of simple statements — an owned-builtin call whose result is not bound is hoisted into a hidden binding and freed at the end of the containing statement when it is the bare statement expression or a builtin argument nested through builtins; temporaries in conditional positions or user-function arguments leak, never dangle. M9.2-B (2026-07-29) makes `break` and `skip` destroy the owned strings of every scope they exit (innermost first, through the enclosing loop body), and M9.2-C (2026-07-29) makes scalar-result functions destroy their body's owners after the result is computed, extended by M9.2-G (2026-07-29) to `str`-result functions whose final expression is a string literal or a direct owned-builtin call (storage provably distinct from every owner), and by M9.2-H (2026-07-29) to `str`-result functions whose final expression is a bare identifier naming a body owner — the named owner transfers to the caller and every other owner is freed after the result is computed; alias-identifier, composite, and conditional results may alias an owner and still leak instead of dangling, and callers do not yet destroy transferred strings. M9.2-E (2026-07-29) makes a statement-level `proc_exit` destroy the owned strings of every lexically enclosing scope before terminating (the exit code is computed first; owners in calling functions and embedded `proc_exit` uses still leak, never double-free). There is still no general borrow storage or shared ownership. Full ownership checking continues in the remaining M9 packages.
