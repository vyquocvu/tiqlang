/* BENCH_ITERATIONS=5000000 BENCH_EXPECTED=37816615 */
#include <stdint.h>
#include <stdio.h>
int main(void) {
    int64_t state = 1, checksum = 0;
    for (int64_t i = 0; i < 5000000; i++) {
        state = (state * 48271) % 2147483647;
        if (state % 7 < 3) checksum = (checksum + state % 1000) % 1000000007;
        else if (state % 11 < 5) checksum = (checksum * 3 + state % 97) % 1000000007;
        else checksum = (checksum + i % 31) % 1000000007;
    }
    printf("%lld\n", (long long)checksum);
    return 0;
}
