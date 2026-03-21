// Test 2,000 goroutines with WaitGroup synchronization
// Equivalent to test_2000_waitgroup.py

package main

import (
	"fmt"
	"sync"
	"time"
)

func worker(wg *sync.WaitGroup, workerID int) {
	defer wg.Done()
	// Simulate some work (empty in this case)
}

func main() {
	var wg sync.WaitGroup

	fmt.Println("Adding 2,000 to WaitGroup...")
	wg.Add(2000)

	fmt.Println("Spawning 2,000 goroutines...")
	start := time.Now()

	for i := 0; i < 2000; i++ {
		go worker(&wg, i)
	}

	spawnTime := time.Since(start)
	fmt.Printf("All goroutines spawned in %v\n", spawnTime)

	fmt.Println("Waiting for WaitGroup...")
	wg.Wait()

	elapsed := time.Since(start)
	fmt.Printf("\n2,000 goroutines completed in %v\n", elapsed)
	fmt.Printf("Throughput: %.0f goroutines/sec\n", float64(2000)/elapsed.Seconds())
	fmt.Println("\n✓ All goroutines completed successfully!")
}
