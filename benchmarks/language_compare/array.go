// BENCH_ITERATIONS=5000000 BENCH_EXPECTED=461852210
package main
import "fmt"
func main() {
	var xs [1024]int64
	for i := int64(0); i < 1024; i++ { xs[i] = (i*17 + 3) % 1000 }
	var state int64 = 1
	var checksum int64
	for i := 0; i < 5000000; i++ {
		state = (state * 48271) % 2147483647
		checksum = (checksum + xs[state%1024]) % 1000000007
	}
	fmt.Println(checksum)
}
