package main

import (
	"fmt"
	"runtime"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

// ============================================================================
// Simple Task Benchmarks (Goroutines)
// ============================================================================

func simpleTask(n int) int {
	sum := 0
	for i := 0; i < n; i++ {
		sum += i
	}
	return sum
}

func ioSimulatedTask(n int) int {
	result := 0
	for i := 0; i < n; i++ {
		result += i * i
	}
	// Simulate I/O with sleep
	time.Sleep(1 * time.Millisecond)
	return result
}

// Benchmark: Spawn goroutines and wait using sync.WaitGroup
func benchmarkGoTaskSpawn(numTasks int, iterations int) float64 {
	times := make([]float64, iterations)
	
	for i := 0; i < iterations; i++ {
		var wg sync.WaitGroup
		
		start := time.Now()
		
		for j := 0; j < numTasks; j++ {
			wg.Add(1)
			go func() {
				defer wg.Done()
				simpleTask(100)
			}()
		}
		
		wg.Wait()
		end := time.Now()
		times[i] = end.Sub(start).Seconds()
	}
	
	avg := sumFloat64(times) / float64(len(times))
	return avg
}

// Benchmark: Spawn goroutines with channel results
func benchmarkGoTaskWithChannel(numTasks int, iterations int) float64 {
	times := make([]float64, iterations)
	
	for i := 0; i < iterations; i++ {
		start := time.Now()
		
		results := make(chan int, numTasks)
		
		for j := 0; j < numTasks; j++ {
			go func() {
				results <- simpleTask(100)
			}()
		}
		
		// Wait for all results
		for j := 0; j < numTasks; j++ {
			<-results
		}
		
		end := time.Now()
		times[i] = end.Sub(start).Seconds()
	}
	
	avg := sumFloat64(times) / float64(len(times))
	return avg
}

// ============================================================================
// Sleep Benchmarks
// ============================================================================

func sleepTask(delayMs int) int {
	time.Sleep(time.Duration(delayMs) * time.Millisecond)
	return delayMs
}

func benchmarkGoSleep(numTasks int, delayMs int, iterations int) float64 {
	times := make([]float64, iterations)
	
	for i := 0; i < iterations; i++ {
		var wg sync.WaitGroup
		
		start := time.Now()
		
		for j := 0; j < numTasks; j++ {
			wg.Add(1)
			go func() {
				defer wg.Done()
				sleepTask(delayMs)
			}()
		}
		
		wg.Wait()
		end := time.Now()
		times[i] = end.Sub(start).Seconds()
	}
	
	avg := sumFloat64(times) / float64(len(times))
	return avg
}

// ============================================================================
// Gather-like Benchmarks (sync.WaitGroup)
// ============================================================================

func benchmarkGoGather(numTasks int, iterations int) float64 {
	times := make([]float64, iterations)
	
	for i := 0; i < iterations; i++ {
		var wg sync.WaitGroup
		
		start := time.Now()
		
		for j := 0; j < numTasks; j++ {
			wg.Add(1)
			go func() {
				defer wg.Done()
				ioSimulatedTask(100)
			}()
		}
		
		wg.Wait()
		end := time.Now()
		times[i] = end.Sub(start).Seconds()
	}
	
	avg := sumFloat64(times) / float64(len(times))
	return avg
}

// ============================================================================
// Chain Benchmarks (Goroutine chains with channel communication)
// ============================================================================

func chainTask(start int, length int, done chan int) {
	result := start
	for i := 0; i < length; i++ {
		time.Sleep(100 * time.Microsecond)
		result++
	}
	done <- result
}

func benchmarkGoChain(numChains int, chainLength int, iterations int) float64 {
	times := make([]float64, iterations)
	
	for i := 0; i < iterations; i++ {
		start := time.Now()
		
		done := make(chan int, numChains)
		
		for j := 0; j < numChains; j++ {
			go chainTask(j, chainLength, done)
		}
		
		// Wait for all chains
		for j := 0; j < numChains; j++ {
			<-done
		}
		
		end := time.Now()
		times[i] = end.Sub(start).Seconds()
	}
	
	avg := sumFloat64(times) / float64(len(times))
	return avg
}

// ============================================================================
// Batch Processing Benchmarks
// ============================================================================

func batchWorker(batchId int, batchSize int, wg *sync.WaitGroup, results chan int) {
	defer wg.Done()
	sum := 0
	for i := 0; i < batchSize; i++ {
		time.Sleep(1 * time.Millisecond)
		sum += batchId*batchSize + i
	}
	results <- sum
}

func benchmarkGoBatch(numTasks int, batchSize int, iterations int) float64 {
	times := make([]float64, iterations)
	
	for i := 0; i < iterations; i++ {
		start := time.Now()
		
		var wg sync.WaitGroup
		results := make(chan int, numTasks/batchSize)
		
		batches := numTasks / batchSize
		for j := 0; j < batches; j++ {
			wg.Add(1)
			go batchWorker(j, batchSize, &wg, results)
		}
		
		wg.Wait()
		close(results)
		
		for range results {
			// Drain channel
		}
		
		end := time.Now()
		times[i] = end.Sub(start).Seconds()
	}
	
	avg := sumFloat64(times) / float64(len(times))
	return avg
}

// ============================================================================
// Mutex-protected Counter Benchmarks (simulates shared state)
// ============================================================================

func benchmarkGoMutex(numTasks int, iterations int) float64 {
	times := make([]float64, iterations)
	
	for i := 0; i < iterations; i++ {
		var mu sync.Mutex
		counter := 0
		
		start := time.Now()
		
		var wg sync.WaitGroup
		for j := 0; j < numTasks; j++ {
			wg.Add(1)
			go func() {
				defer wg.Done()
				mu.Lock()
				counter++
				mu.Unlock()
			}()
		}
		
		wg.Wait()
		end := time.Now()
		times[i] = end.Sub(start).Seconds()
	}
	
	avg := sumFloat64(times) / float64(len(times))
	return avg
}

// ============================================================================
// Atomic Counter Benchmarks
// ============================================================================

func benchmarkGoAtomic(numTasks int, iterations int) float64 {
	times := make([]float64, iterations)
	
	for i := 0; i < iterations; i++ {
		var counter int64
		
		start := time.Now()
		
		var wg sync.WaitGroup
		for j := 0; j < numTasks; j++ {
			wg.Add(1)
			go func() {
				defer wg.Done()
				atomic.AddInt64(&counter, 1)
			}()
		}
		
		wg.Wait()
		end := time.Now()
		times[i] = end.Sub(start).Seconds()
	}
	
	avg := sumFloat64(times) / float64(len(times))
	return avg
}

// ============================================================================
// Channel Communication Benchmarks
// ============================================================================

func benchmarkGoChannelComm(numTasks int, iterations int) float64 {
	times := make([]float64, iterations)
	
	for i := 0; i < iterations; i++ {
		start := time.Now()
		
		ch := make(chan int, numTasks)
		
		// Sender goroutines
		for j := 0; j < numTasks; j++ {
			go func(val int) {
				ch <- val
			}(j)
		}
		
		// Receiver
		for j := 0; j < numTasks; j++ {
			<-ch
		}
		
		end := time.Now()
		times[i] = end.Sub(start).Seconds()
	}
	
	avg := sumFloat64(times) / float64(len(times))
	return avg
}

// ============================================================================
// Helper Functions
// ============================================================================

func sumFloat64(slice []float64) float64 {
	sum := 0.0
	for _, v := range slice {
		sum += v
	}
	return sum
}

func printResults(name string, results map[string]float64) {
	fmt.Println("Running", name, "benchmarks...")
	fmt.Println("-" + strings.Repeat("-", 48))
	
	for key, value := range results {
		fmt.Printf("%s: %.4fs\n", key, value)
	}
	fmt.Println("-" + strings.Repeat("-", 48))
}

// ============================================================================
// Main
// ============================================================================

func main() {
	// Set GOMAXPROCS to use all available CPUs
	runtime.GOMAXPROCS(runtime.NumCPU())
	
	fmt.Println("Go Routine Benchmarks")
	fmt.Println("=====================")
	fmt.Printf("GOMAXPROCS: %d\n", runtime.NumCPU())
	fmt.Println()
	
	results := make(map[string]float64)
	iterations := 10
	
	// Task spawn benchmarks
	fmt.Println("\n=== Task Spawn ===")
	for _, numTasks := range []int{100, 1000, 10000} {
		key := fmt.Sprintf("go_task_spawn_%d", numTasks)
		results[key] = benchmarkGoTaskSpawn(numTasks, iterations)
		fmt.Printf("Task spawn (%d tasks): %.4fs\n", numTasks, results[key])
	}
	
	// Task with channel
	for _, numTasks := range []int{100, 1000, 10000} {
		key := fmt.Sprintf("go_task_channel_%d", numTasks)
		results[key] = benchmarkGoTaskWithChannel(numTasks, iterations)
		fmt.Printf("Task channel (%d tasks): %.4fs\n", numTasks, results[key])
	}
	
	// Sleep benchmarks
	fmt.Println("\n=== Sleep ===")
	for _, numTasks := range []int{100, 1000, 10000} {
		key := fmt.Sprintf("go_sleep_%d", numTasks)
		results[key] = benchmarkGoSleep(numTasks, 10, iterations)
		fmt.Printf("Sleep (%d tasks): %.4fs\n", numTasks, results[key])
	}
	
	// Gather benchmarks
	fmt.Println("\n=== Gather ===")
	for _, numTasks := range []int{100, 1000, 5000} {
		key := fmt.Sprintf("go_gather_%d", numTasks)
		results[key] = benchmarkGoGather(numTasks, iterations)
		fmt.Printf("Gather (%d tasks): %.4fs\n", numTasks, results[key])
	}
	
	// Chain benchmarks
	fmt.Println("\n=== Chain ===")
	for _, numChains := range []int{10, 100, 1000} {
		key := fmt.Sprintf("go_chain_%d", numChains)
		results[key] = benchmarkGoChain(numChains, 10, iterations)
		fmt.Printf("Chain (%d chains): %.4fs\n", numChains, results[key])
	}
	
	// Batch benchmarks
	fmt.Println("\n=== Batch ===")
	for _, numTasks := range []int{100, 1000} {
		key := fmt.Sprintf("go_batch_%d", numTasks)
		results[key] = benchmarkGoBatch(numTasks, 10, iterations)
		fmt.Printf("Batch (%d tasks): %.4fs\n", numTasks, results[key])
	}
	
	// Mutex benchmarks
	fmt.Println("\n=== Mutex ===")
	for _, numTasks := range []int{100, 1000, 10000} {
		key := fmt.Sprintf("go_mutex_%d", numTasks)
		results[key] = benchmarkGoMutex(numTasks, iterations)
		fmt.Printf("Mutex (%d tasks): %.4fs\n", numTasks, results[key])
	}
	
	// Atomic benchmarks
	fmt.Println("\n=== Atomic ===")
	for _, numTasks := range []int{100, 1000, 10000} {
		key := fmt.Sprintf("go_atomic_%d", numTasks)
		results[key] = benchmarkGoAtomic(numTasks, iterations)
		fmt.Printf("Atomic (%d tasks): %.4fs\n", numTasks, results[key])
	}
	
	// Channel communication benchmarks
	fmt.Println("\n=== Channel Comm ===")
	for _, numTasks := range []int{100, 1000, 10000} {
		key := fmt.Sprintf("go_channel_%d", numTasks)
		results[key] = benchmarkGoChannelComm(numTasks, iterations)
		fmt.Printf("Channel (%d tasks): %.4fs\n", numTasks, results[key])
	}
	
	fmt.Println("\n" + strings.Repeat("=", 50))
}
