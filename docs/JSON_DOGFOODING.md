# Tiq JSON dogfooding report

Status: correctness artifact implemented for issue #16; performance comparison remains a separate beta-gate activity.

## Reproduction

```sh
make clean
make
make tool-json
make test
make clean
make CFLAGS='-std=c11 -Wall -Wextra -Wpedantic -Werror -O1 -g -fsanitize=address,undefined'
make CFLAGS='-std=c11 -Wall -Wextra -Wpedantic -Werror -O1 -g -fsanitize=address,undefined' tool-json
```

`tests/json_tool.sh` checks the checked-in corpus against Python's independent
JSON implementation, malformed input diagnostics, UTF-8 rejection, the nesting
boundary, traversal, replacement, insertion, escaping, and regeneration.

## Representation and policy

The parser first validates and compacts input, then constructs a flat arena of
`JsonNode` records. Each record stores its kind, source span, parent index,
object key, optional replacement, and active state. Parent indices represent
recursive data without recursive struct types. Arena order preserves object
member order. Duplicate keys are preserved; `--get` selects the last matching
member and `--set-string` deactivates that member before appending its
replacement. The generator walks parent links and never uses the JSON runtime
builtins.

The command contract is:

```text
tiq-json FILE
tiq-json FILE --get KEY
tiq-json FILE --set-string KEY VALUE
```

All forms parse the document. The first regenerates it from the arena, the
second traverses a root object, and the third mutates a root object and
regenerates it. Input nesting is limited to 128 container levels.

## Friction record

- **Language:** recursive structs are unnecessary when an indexed flat arena is
  available, but lack of sum types makes integer kind tags necessary.
- **Library:** `vec[JsonNode]` gives deterministic storage and parent links;
  ordered object lookup is linear because `map` cannot represent duplicate keys.
- **Diagnostics:** carrying byte offset in a shared state vec is concise, while
  calculating line/column at the application boundary avoids work on success.
- **Ownership:** vec and strbuf handles use the documented leak-never-dangle
  bootstrap policy. Consequently this artifact cannot honestly demonstrate a
  leak-free container lifecycle; that requires the planned container ownership
  work rather than an application-specific C escape hatch.
- **Pattern matching:** integer kind dispatch is workable but less exhaustive
  than the language's future fully typed recursive sum representation.
- **Option/Result:** current container-return constraints make a shared error
  state more practical; no syntax change was introduced solely for this tool.

## Performance scope

No simdjson or RapidJSON numbers are claimed here. A fair comparison must use
the same corpus, validation semantics, allocator accounting, and separate
parse/generate/parse-modify-generate modes. Process-start timing of this CLI
would mostly measure executable startup and is intentionally not presented as
parser throughput.
