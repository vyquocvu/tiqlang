// BENCH_ITERATIONS=20 BENCH_EXPECTED=362194569
package main
import "fmt"
func main() {
	var a, b [4096]int64
	var checksum int64
	for i := int64(0); i < 4096; i++ { a[i] = (i*17+3)%101; b[i] = (i*29+7)%103 }
	for rep := 0; rep < 20; rep++ { for i := 0; i < 64; i++ { for j := 0; j < 64; j++ {
		var cell int64
		for k := 0; k < 64; k++ { cell += a[i*64+k] * b[k*64+j] }
		checksum = (checksum + cell%1000003) % 1000000007
	} } }
	fmt.Println(checksum)
}
