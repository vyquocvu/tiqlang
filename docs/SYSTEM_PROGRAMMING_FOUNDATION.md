# Epic 1 — System Programming Foundation

This document records Tiq's first systems-programming contract: explicit failures, direct C interop, and explicit allocation policy.

## 1. Error handling

Tiq has no hidden exception unwinder. Fallible values use the existing tagged `Option` / `Result` representation:

```tiq
value = some(42)
fallback = value ?? 0

result = ok(42)
x = result ?? 0

unwrapped = ?result
```

`some(x)` / `none` represent optional values. `ok(x)` / `err(e)` represent success and failure. `??` handles a failure locally with a fallback; `?value` is the explicit propagation / unwrap operator supported by the bootstrap compiler.

The systems API in this epic follows the same rule: raw C-ABI helpers report failure as `0` or a status code, while the Tiq stdlib converts fallible constructors and allocations into `Result` values. There is no allocation exception and no implicit retry.

## 2. C FFI

The canonical foreign declaration is already:

```tiq
extern "C" strlen s:str : i64
extern "C" getpid : i64
```

The ABI is deliberately thin: supported scalar and named-struct types lower directly to C-compatible types, calls have no trampoline, and `-l` / `-L` are forwarded to the host linker. Unsupported FFI types fail closed during semantic checking.

This epic reuses that boundary for allocators rather than introducing allocator-specific compiler syntax.

## 3. Allocator interface

Import the allocator module:

```tiq
import "std/alloc.tiq"
```

Allocator values are opaque `u64` handles at the bootstrap ABI boundary. Three strategies are provided:

- **General** — process heap allocation using the platform C allocator.
- **Arena** — monotonic allocation from one fixed-capacity region; individual deallocation is a no-op and `allocator_reset` releases the whole region logically in O(1).
- **Pool** — fixed-block allocation with deterministic reuse and double-free rejection.

### General allocator

```tiq
general = tiq_allocator_general()
ptr = allocator_alloc(general, 64, 8) ?? 0
allocator_dealloc(general, ptr, 64, 8)
```

The general allocator is a process-lifetime singleton. Destroy/reset are no-ops.

### Arena allocator

```tiq
arena_handle = arena(64 * 1024) ?? 0
ptr = allocator_alloc(arena_handle, 256, 8) ?? 0
allocator_reset(arena_handle)
allocator_destroy(arena_handle)
```

Arena creation and allocation are fallible. `allocator_reset` invalidates every pointer previously returned by that arena. Individual `allocator_dealloc` calls intentionally do nothing.

### Pool allocator

```tiq
pool_handle = pool(256, 1024) ?? 0
ptr = allocator_alloc(pool_handle, 128, 8) ?? 0
allocator_dealloc(pool_handle, ptr, 128, 8)
allocator_destroy(pool_handle)
```

The pool has a fixed number of blocks. Exhaustion returns `err(2)` through `allocator_alloc`. Invalid pointers and double frees return a non-zero status instead of corrupting the free list.

## ABI contract

`std/alloc.tiq` is intentionally a thin layer over these C ABI functions:

```text
tiq_allocator_general() -> u64
tiq_arena_create(capacity) -> u64
tiq_pool_create(block_size, block_count) -> u64
tiq_allocator_alloc(handle, size, alignment) -> u64
tiq_allocator_dealloc(handle, ptr, size, alignment) -> i64
tiq_allocator_reset(handle) -> i64
tiq_allocator_destroy(handle) -> i64
```

Raw creation/allocation functions return `0` on failure. The stdlib wrappers convert that to `Result`:

```text
arena(...)           -> ok(handle) | err(1)
pool(...)            -> ok(handle) | err(1)
allocator_alloc(...) -> ok(ptr)    | err(2)
```

The public wrappers therefore compose with Tiq's existing `??` and `?` operators and keep failure visible in source.

## Alignment and safety

Alignment must be a non-zero power of two. The bootstrap general/pool allocators support fundamental C alignment up to `_Alignof(max_align_t)`. Arena allocation supports any power-of-two alignment representable by `size_t` as long as capacity permits it.

Allocator handles and returned addresses are represented as `u64` because v0.1 intentionally does not expose first-class raw pointers. They must be treated as opaque values. Pointer arithmetic remains outside the safe core language.

## Container integration boundary

This PR establishes the allocator ABI and stdlib contract without silently changing existing container ownership. Current `vec[T]` and `map` builtins retain their existing allocation behavior.

The next allocator package should add explicit allocator-aware constructors, for example:

```tiq
items = vec_with_allocator[int](arena_handle)
index = map_with_allocator(arena_handle)
```

or an equivalent surface approved by the syntax budget. Existing constructors should keep a General allocator default for source compatibility, while the allocator-aware path remains explicit at the call site. Container migration must include destruction semantics and backend parity before being marked complete.

## Design invariants

1. No hidden exceptions.
2. No mandatory GC or allocator runtime thread.
3. Allocation strategy is visible in the program.
4. C ABI calls remain direct and zero-trampoline.
5. OOM and invalid allocator operations fail predictably.
6. Arena reset/destroy and pool destroy have deterministic lifetime boundaries.
7. Unsupported or ambiguous allocator behavior fails closed rather than guessing.
