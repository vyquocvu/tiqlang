// BENCH_ITERATIONS=20 BENCH_EXPECTED=362194569
fn main() {
    let (mut a, mut b) = ([0_i64; 4096], [0_i64; 4096]);
    let mut checksum = 0_i64;
    for i in 0..4096 { a[i] = (i as i64 * 17 + 3) % 101; b[i] = (i as i64 * 29 + 7) % 103; }
    for _ in 0..20 { for i in 0..64 { for j in 0..64 {
        let mut cell = 0_i64;
        for k in 0..64 { cell += a[i*64+k] * b[k*64+j]; }
        checksum = (checksum + cell % 1_000_003) % 1_000_000_007;
    } } }
    println!("{checksum}");
}
