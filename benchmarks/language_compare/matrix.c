/* BENCH_ITERATIONS=20 BENCH_EXPECTED=362194569 */
#include <stdint.h>
#include <stdio.h>
int main(void) {
    int64_t a[4096], b[4096], checksum = 0;
    for (int64_t i = 0; i < 4096; i++) { a[i] = (i * 17 + 3) % 101; b[i] = (i * 29 + 7) % 103; }
    for (int64_t rep = 0; rep < 20; rep++)
        for (int64_t i = 0; i < 64; i++)
            for (int64_t j = 0; j < 64; j++) {
                int64_t cell = 0;
                for (int64_t k = 0; k < 64; k++) cell += a[i * 64 + k] * b[k * 64 + j];
                checksum = (checksum + cell % 1000003) % 1000000007;
            }
    printf("%lld\n", (long long)checksum);
    return 0;
}
