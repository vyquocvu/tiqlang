# BENCH_ITERATIONS=5000000 BENCH_EXPECTED=461852210
xs = [(i * 17 + 3) % 1000 for i in range(1024)]
state = 1
checksum = 0
for _ in range(5_000_000):
    state = (state * 48_271) % 2_147_483_647
    checksum = (checksum + xs[state % 1024]) % 1_000_000_007
print(checksum)
