// BENCH_ITERATIONS=5000000 BENCH_EXPECTED=37816615
package main
import "fmt"
func main() {
	var state int64 = 1
	var checksum int64
	for i := int64(0); i < 5000000; i++ {
		state = (state * 48271) % 2147483647
		if state%7 < 3 { checksum = (checksum + state%1000) % 1000000007
		} else if state%11 < 5 { checksum = (checksum*3 + state%97) % 1000000007
		} else { checksum = (checksum + i%31) % 1000000007 }
	}
	fmt.Println(checksum)
}
