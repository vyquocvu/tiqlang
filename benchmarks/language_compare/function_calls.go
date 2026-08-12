// BENCH_ITERATIONS=1000000 BENCH_EXPECTED=7270106
package main
import "fmt"
func gcd(a, b int64) int64 { if b == 0 { return a }; return gcd(b, a%b) }
func main() {
	var state int64 = 1
	var checksum int64
	for i := 0; i < 1000000; i++ {
		state = (state * 48271) % 2147483647
		checksum = (checksum + gcd(state, state%10000+12345)) % 1000000007
	}
	fmt.Println(checksum)
}
