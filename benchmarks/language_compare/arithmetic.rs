// BENCH_ITERATIONS=5000000 BENCH_EXPECTED=497907426
fn main() {
    let mut state: i64 = 1;
    let mut checksum: i64 = 0;
    for _ in 0..5_000_000 {
        state = (state * 48_271) % 2_147_483_647;
        checksum = (checksum + state % 1_000) % 1_000_000_007;
    }
    println!("{checksum}");
}
