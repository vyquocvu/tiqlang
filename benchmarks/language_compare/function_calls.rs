// BENCH_ITERATIONS=1000000 BENCH_EXPECTED=7270106
fn gcd(a: i64, b: i64) -> i64 { if b == 0 { a } else { gcd(b, a % b) } }
fn main() {
    let mut state = 1_i64;
    let mut checksum = 0_i64;
    for _ in 0..1_000_000 {
        state = (state * 48_271) % 2_147_483_647;
        checksum = (checksum + gcd(state, state % 10_000 + 12_345)) % 1_000_000_007;
    }
    println!("{checksum}");
}
