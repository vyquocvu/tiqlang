/* BENCH_ITERATIONS=5000000 BENCH_EXPECTED=497907426 */
#include <stdint.h>
#include <stdio.h>

int main(void) {
    int64_t state = 1;
    int64_t checksum = 0;
    for (int64_t i = 0; i < 5000000; i++) {
        state = (state * 48271) % 2147483647;
        checksum = (checksum + state % 1000) % 1000000007;
    }
    printf("%lld\n", (long long)checksum);
    return 0;
}
