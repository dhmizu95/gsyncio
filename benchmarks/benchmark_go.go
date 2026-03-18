package main

import (
	"fmt"
	"runtime"
	"sync"
	"time"
)

// Benchmark task spawning (similar to gsyncio.task)
func benchmarkTaskSpawn(numTasks int) {
	fmt.Printf("\n=== Go Task Spawn Benchmark (%d tasks) ===\n", numTasks)

	counter := 0
	var mu sync.Mutex

	worker := func() {
		mu.Lock()
		counter++
		mu.Unlock()
	}

	start := time.Now()
	var wg sync.WaitGroup
	for i := 0; i < numTasks; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			worker()
		}()
	}
	wg.Wait()
	elapsed := time.Since(start)

	// Show appropriate time unit
	if elapsed.Milliseconds() > 0 {
		fmt.Printf("  Total time: %.2fms\n", float64(elapsed.Milliseconds()))
	} else {
		fmt.Printf("  Total time: %.2fµs\n", float64(elapsed.Microseconds()))
	}
	fmt.Printf("  Tasks/sec: %.0f\n", float64(numTasks)/float64(elapsed.Seconds()))
	fmt.Printf("  Per task: %.2fµs\n", float64(elapsed.Microseconds())/float64(numTasks))
	fmt.Printf("  Completed: %d/%d\n", counter, numTasks)
}

// Benchmark goroutine with sleep (similar to gsyncio.sleep)
func benchmarkGoroutineSleep(numTasks int) {
	fmt.Printf("\n=== Go Goroutine Sleep Benchmark (%d tasks) ===\n", numTasks)

	worker := func() {
		time.Sleep(1 * time.Millisecond)
	}

	start := time.Now()
	var wg sync.WaitGroup
	for i := 0; i < numTasks; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			worker()
		}()
	}
	wg.Wait()
	elapsed := time.Since(start)

	fmt.Printf("  Total time: %.2fms\n", float64(elapsed.Milliseconds()))
	fmt.Printf("  Tasks: %d\n", numTasks)
}

// Benchmark WaitGroup synchronization
func benchmarkWaitGroup(numWorkers int) {
	fmt.Printf("\n=== Go WaitGroup Benchmark (%d workers) ===\n", numWorkers)

	counter := 0
	var mu sync.Mutex

	start := time.Now()
	var wg sync.WaitGroup
	for i := 0; i < numWorkers; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for j := 0; j < 100; j++ {
				mu.Lock()
				counter++
				mu.Unlock()
			}
		}()
	}
	wg.Wait()
	elapsed := time.Since(start)

	// Show appropriate time unit
	if elapsed.Milliseconds() > 0 {
		fmt.Printf("  Total time: %.2fms\n", float64(elapsed.Milliseconds()))
	} else {
		fmt.Printf("  Total time: %.2fµs\n", float64(elapsed.Microseconds()))
	}
	totalOps := numWorkers * 100
	fmt.Printf("  Operations: %d\n", totalOps)
	fmt.Printf("  Ops/sec: %.0f\n", float64(totalOps)/float64(elapsed.Seconds()))
}

// Benchmark context switching (via runtime.Gosched)
func benchmarkContextSwitch(numYields int) {
	fmt.Printf("\n=== Go Context Switch Benchmark (%d yields) ===\n", numYields)

	start := time.Now()
	var wg sync.WaitGroup
	for i := 0; i < 10; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for j := 0; j < numYields/10; j++ {
				runtime.Gosched()
			}
		}()
	}
	wg.Wait()
	elapsed := time.Since(start)

	// Show appropriate time unit
	if elapsed.Milliseconds() > 0 {
		fmt.Printf("  Total time: %.2fms\n", float64(elapsed.Milliseconds()))
	} else {
		fmt.Printf("  Total time: %.2fµs\n", float64(elapsed.Microseconds()))
	}
	fmt.Printf("  Yields/sec: %.0f\n", float64(numYields)/float64(elapsed.Seconds()))
	fmt.Printf("  Per yield: %.2fµs\n", float64(elapsed.Microseconds())/float64(numYields))
}

func main() {
	fmt.Println("============================================================")
	fmt.Println("Go Goroutine Performance Benchmark")
	fmt.Println("============================================================")
	fmt.Printf("Go Version: %s\n", runtime.Version())
	fmt.Printf("CPU Cores: %d\n", runtime.NumCPU())
	fmt.Println()

	benchmarkTaskSpawn(1000)
	benchmarkGoroutineSleep(100)
	benchmarkWaitGroup(10)
	benchmarkContextSwitch(10000)

	fmt.Println("\n============================================================")
	fmt.Println("Summary")
	fmt.Println("============================================================")
	fmt.Println("Note: Go uses M:N goroutine scheduling with stack management.")
}
