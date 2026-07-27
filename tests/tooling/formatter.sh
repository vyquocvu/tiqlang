#!/bin/sh
# Test the Tiq formatter

set -e

TIQ="${TIQ:-./build/tiq}"
TMP="${TMPDIR:-/tmp}/tiq-fmt-test-$$"
mkdir -p "$TMP"

echo "=== Testing formatter ==="

# Test basic formatting
echo 'print( "Hello" )' > "$TMP/basic.tiq"
$TIQ fmt "$TMP/basic.tiq" > "$TMP/basic_out.tiq"

# Test formatter with stdin
echo 'x = 1' | $TIQ fmt > "$TMP/stdin_out.tiq"

# Comments are data: fmt must never delete them (plan 1.1).
cat > "$TMP/comments.tiq" << 'EOF'
// leading comment
x = 1 // trailing comment
{
    // inside block
    y = 2
}
EOF
$TIQ fmt "$TMP/comments.tiq" > "$TMP/comments_out.tiq"
for pattern in '// leading comment' '// trailing comment' '// inside block'; do
    if ! grep -qF "$pattern" "$TMP/comments_out.tiq"; then
        echo "formatter lost comment: $pattern" >&2
        cat "$TMP/comments_out.tiq" >&2
        exit 1
    fi
done
echo "formatter preserves comments: passed"

# Trailing comment must stay on the same line as its statement
if ! grep -qE 'x = 1 // trailing comment' "$TMP/comments_out.tiq"; then
    echo "trailing comment not kept on its line" >&2
    cat "$TMP/comments_out.tiq" >&2
    exit 1
fi
echo "formatter keeps trailing comment position: passed"

# Stdin path must preserve comments too
printf 'a = 1 // c\n' | $TIQ fmt > "$TMP/stdin_comment.tiq"
if ! grep -qF '// c' "$TMP/stdin_comment.tiq"; then
    echo "stdin formatter lost comment" >&2
    exit 1
fi
echo "stdin formatter preserves comments: passed"

# Stdin and file paths must produce identical bytes (plan 2.3: one engine)
cat > "$TMP/equiv.tiq" << 'EOF'
// header
add a b -> a + b
x = 1 // trailing
{
    y = add(x, 2)
}
[0..3 | print(y)]
EOF
$TIQ fmt "$TMP/equiv.tiq" > "$TMP/equiv_file.tiq"
$TIQ fmt < "$TMP/equiv.tiq" > "$TMP/equiv_stdin.tiq"
if ! cmp -s "$TMP/equiv_file.tiq" "$TMP/equiv_stdin.tiq"; then
    echo "stdin and file formatting differ" >&2
    diff "$TMP/equiv_file.tiq" "$TMP/equiv_stdin.tiq" >&2 || true
    exit 1
fi
echo "stdin/file equivalence: passed"

# Stdin input of exactly 4096 bytes (initial buffer capacity) must not
# overflow the NUL terminator slot (regression, plan 2.3).
awk 'BEGIN { for (i = 0; i < 512; i++) printf "x = 111\n" }' > "$TMP/boundary.tiq"
if [ "$(wc -c < "$TMP/boundary.tiq" | tr -d ' ')" != "4096" ]; then
    echo "boundary fixture is not 4096 bytes" >&2
    exit 1
fi
$TIQ fmt < "$TMP/boundary.tiq" > "$TMP/boundary_out.tiq"
echo "stdin 4096-byte boundary: passed"

# Test --check mode (should exit 0 for valid code)
echo 'print("valid")' > "$TMP/valid.tiq"
if $TIQ fmt --check "$TMP/valid.tiq" 2>&1; then
    echo "check mode: passed"
else
    echo "check mode failed unexpectedly"
    exit 1
fi

# Call syntax must stay tight: no space between callee and '('
printf 'add a b -> a + b\nprint(add(1, 2))\n' > "$TMP/call_tight.tiq"
$TIQ fmt "$TMP/call_tight.tiq" > "$TMP/call_tight_out.tiq"
if ! grep -qF 'print(add(1, 2))' "$TMP/call_tight_out.tiq"; then
    echo "formatter split a call from its argument list" >&2
    cat "$TMP/call_tight_out.tiq" >&2
    exit 1
fi
echo "formatter keeps calls tight: passed"

# Test formatter preserves strings correctly
echo 'print("test string")' > "$TMP/string.tiq"
$TIQ fmt "$TMP/string.tiq" > "$TMP/string_out.tiq"
if grep -q '"test string"' "$TMP/string_out.tiq"; then
    echo "formatter preserves strings: passed"
else
    echo "formatter preserves strings: FAILED"
    exit 1
fi

# Test formatter with loops
cat > "$TMP/loop.tiq" << 'EOF'
x <- 0
[0..10 | x += i]
print(x)
EOF
$TIQ fmt "$TMP/loop.tiq" > "$TMP/loop_out.tiq"
echo "formatter handles loops: passed"

# Test formatter with functions
cat > "$TMP/fn.tiq" << 'EOF'
add a b -> a + b
print(add(1, 2))
EOF
$TIQ fmt "$TMP/fn.tiq" > "$TMP/fn_out.tiq"
echo "formatter handles functions: passed"

# Test formatter with stream generators
cat > "$TMP/stream.tiq" << 'EOF'
fib = [0, 1, ... a + b]
print(fib[10])
EOF
$TIQ fmt "$TMP/stream.tiq" > "$TMP/stream_out.tiq"
echo "formatter handles streams: passed"

# Test --use-tabs option
$TIQ fmt --use-tabs "$TMP/basic.tiq" > "$TMP/tabs_out.tiq"
echo "formatter --use-tabs: passed"

# Test --indent-width option
$TIQ fmt --indent-width 2 "$TMP/fn.tiq" > "$TMP/indent2_out.tiq"
echo "formatter --indent-width: passed"

# Test --output option
$TIQ fmt "$TMP/basic.tiq" --output "$TMP/output.tiq"
if test -f "$TMP/output.tiq"; then
    echo "formatter --output: passed"
else
    echo "formatter --output: FAILED"
    exit 1
fi

# Test that formatted output is valid Tiq code
if $TIQ check "$TMP/basic_out.tiq" 2>/dev/null; then
    echo "formatter produces valid output: passed"
else
    echo "formatter produces valid output: FAILED"
    exit 1
fi

if $TIQ check "$TMP/loop_out.tiq" 2>/dev/null; then
    echo "formatter produces valid loop code: passed"
else
    echo "formatter produces valid loop code: FAILED"
    exit 1
fi

if $TIQ check "$TMP/fn_out.tiq" 2>/dev/null; then
    echo "formatter produces valid function code: passed"
else
    echo "formatter produces valid function code: FAILED"
    exit 1
fi

if $TIQ check "$TMP/stream_out.tiq" 2>/dev/null; then
    echo "formatter produces valid stream code: passed"
else
    echo "formatter produces valid stream code: FAILED"
    exit 1
fi

rm -rf "$TMP"
echo "formatter tests: ok"
