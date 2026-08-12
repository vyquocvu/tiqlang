# BENCH_ITERATIONS=20 BENCH_EXPECTED=362194569
n = 64
a = [(i * 17 + 3) % 101 for i in range(n * n)]
b = [(i * 29 + 7) % 103 for i in range(n * n)]
checksum = 0
for _ in range(20):
    for i in range(n):
        for j in range(n):
            cell = 0
            for k in range(n):
                cell += a[i*n+k] * b[k*n+j]
            checksum = (checksum + cell % 1_000_003) % 1_000_000_007
print(checksum)
