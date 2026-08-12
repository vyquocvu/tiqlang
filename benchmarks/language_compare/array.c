/* BENCH_ITERATIONS=5000000 BENCH_EXPECTED=461852210 */
#include <stdint.h>
#include <stdio.h>
int main(void) {
    int64_t xs[1024];
    for (int64_t i = 0; i < 1024; i++) xs[i] = (i * 17 + 3) % 1000;
    int64_t state = 1, checksum = 0;
    for (int64_t i = 0; i < 5000000; i++) {
        state = (state * 48271) % 2147483647;
        checksum = (checksum + xs[state % 1024]) % 1000000007;
    }
    printf("%lld\n", (long long)checksum);
    return 0;
}
