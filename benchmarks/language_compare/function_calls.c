/* BENCH_ITERATIONS=1000000 BENCH_EXPECTED=7270106 */
#include <stdint.h>
#include <stdio.h>
static int64_t gcd(int64_t a, int64_t b) { return b == 0 ? a : gcd(b, a % b); }
int main(void) {
    int64_t state = 1, checksum = 0;
    for (int64_t i = 0; i < 1000000; i++) {
        state = (state * 48271) % 2147483647;
        checksum = (checksum + gcd(state, state % 10000 + 12345)) % 1000000007;
    }
    printf("%lld\n", (long long)checksum);
    return 0;
}
