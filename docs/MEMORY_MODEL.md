# Tiq Memory Model

Tiq aims for deterministic memory management without a mandatory garbage collector.

## v0.x direction

- Scalars and small aggregates use value semantics.
- Owned heap values are destroyed at the end of their scope.
- Assignment moves owned values unless the type is explicitly copyable.
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

`str` is an immutable UTF-8 byte view. Owned strings will use a separate type or ownership qualifier. String operations must not assume NUL termination.

## Concurrency

No value is implicitly thread-safe. Transfer across tasks requires move, copy, or an explicitly synchronized shared type.

## Bootstrap

The current compiler slice emits C string literals only and performs no heap allocation. Ownership checking begins after the typed AST milestone.
