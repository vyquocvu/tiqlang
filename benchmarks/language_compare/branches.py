# BENCH_ITERATIONS=5000000 BENCH_EXPECTED=37816615
state = 1
checksum = 0
for i in range(5_000_000):
    state = (state * 48_271) % 2_147_483_647
    if state % 7 < 3:
        checksum = (checksum + state % 1000) % 1_000_000_007
    elif state % 11 < 5:
        checksum = (checksum * 3 + state % 97) % 1_000_000_007
    else:
        checksum = (checksum + i % 31) % 1_000_000_007
print(checksum)
