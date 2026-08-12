# BENCH_ITERATIONS=1000000 BENCH_EXPECTED=7270106
def gcd(a, b):
    return a if b == 0 else gcd(b, a % b)
state = 1
checksum = 0
for _ in range(1_000_000):
    state = (state * 48_271) % 2_147_483_647
    checksum = (checksum + gcd(state, state % 10_000 + 12_345)) % 1_000_000_007
print(checksum)
