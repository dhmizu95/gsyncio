// side_by_side.go - the Go half of a matched pair.
//
// Same program as side_by_side.py: fan out N workers, each computes a
// checksum over a slice of the range, push results down a channel, and
// have a collector fold them into a total.
//
//	go run side_by_side.go
package main

import (
	"fmt"
	"sync"
	"time"
)

const (
	workers   = 8
	chunkSize = 200_000
)

// worker computes a checksum for one chunk and sends it on results.
func worker(id int, results chan<- int64, wg *sync.WaitGroup) {
	defer wg.Done()

	start := int64(id) * chunkSize
	var sum int64
	for i := start; i < start+chunkSize; i++ {
		sum += i * i
	}
	results <- sum
}

func main() {
	t0 := time.Now()

	results := make(chan int64, workers)
	var wg sync.WaitGroup

	// Fan out: one goroutine per chunk.
	for i := 0; i < workers; i++ {
		wg.Add(1)
		go worker(i, results, &wg)
	}

	// Close the channel once every producer is done, so the range below
	// terminates instead of blocking forever.
	go func() {
		wg.Wait()
		close(results)
	}()

	// Fan in.
	var total int64
	count := 0
	for sum := range results {
		total += sum
		count++
	}

	fmt.Printf("go:      %d workers, %d results, total=%d, %.1f ms\n",
		workers, count, total, float64(time.Since(t0).Microseconds())/1000)
}
