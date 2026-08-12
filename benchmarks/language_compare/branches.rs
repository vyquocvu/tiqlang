// BENCH_ITERATIONS=5000000 BENCH_EXPECTED=37816615
fn main() {
    let mut state = 1_i64;
    let mut checksum = 0_i64;
    for i in 0..5_000_000_i64 {
        state = (state * 48_271) % 2_147_483_647;
        if state % 7 < 3 { checksum = (checksum + state % 1000) % 1_000_000_007; }
        else if state % 11 < 5 { checksum = (checksum * 3 + state % 97) % 1_000_000_007; }
        else { checksum = (checksum + i % 31) % 1_000_000_007; }
    }
    println!("{checksum}");
}
