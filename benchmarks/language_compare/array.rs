// BENCH_ITERATIONS=5000000 BENCH_EXPECTED=461852210
fn main() {
    let mut xs = [0_i64; 1024];
    for i in 0..1024 { xs[i] = ((i * 17 + 3) % 1000) as i64; }
    let mut state = 1_i64;
    let mut checksum = 0_i64;
    for _ in 0..5_000_000 {
        state = (state * 48_271) % 2_147_483_647;
        checksum = (checksum + xs[(state % 1024) as usize]) % 1_000_000_007;
    }
    println!("{checksum}");
}
