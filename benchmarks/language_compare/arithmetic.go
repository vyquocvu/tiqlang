// BENCH_ITERATIONS=5000000 BENCH_EXPECTED=497907426
package main

import "fmt"

func main() {
	var state int64 = 1
	var checksum int64
	for i := int64(0); i < 5000000; i++ {
		state = (state * 48271) % 2147483647
		checksum = (checksum + state%1000) % 1000000007
	}
	fmt.Println(checksum)
}
