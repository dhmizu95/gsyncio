// Go side of the cross-runtime 1M-task comparison.
// Mirrors benchmarks/compare_1m.py: same workloads, same JSON output shape.
//
//	go run compare_1m.go -n 1000000 -workload noop
package main

import (
	"encoding/json"
	"flag"
	"fmt"
	"os"
	"runtime"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

// Mirrors WORK_N in compare_1m.py. The Python side is ~100x slower per
// iteration; this is not an apples-to-apples per-op comparison, it exists
// to show whether a runtime can put work on more than one core at all.
const workN = 2000

var sink atomic.Uint64

func peakRSSMB() float64 {
	data, err := os.ReadFile("/proc/self/status")
	if err != nil {
		return 0
	}
	for _, line := range strings.Split(string(data), "\n") {
		if strings.HasPrefix(line, "VmHWM:") {
			f := strings.Fields(line)
			if len(f) >= 2 {
				kb, _ := strconv.ParseFloat(f[1], 64)
				return kb / 1024.0
			}
		}
	}
	return 0
}

func main() {
	n := flag.Int("n", 1000000, "number of goroutines")
	workload := flag.String("workload", "noop", "noop|work")
	procs := flag.Int("procs", 0, "GOMAXPROCS override (0 = default)")
	flag.Parse()

	if *procs > 0 {
		runtime.GOMAXPROCS(*procs)
	}

	doWork := *workload == "work"

	var wg sync.WaitGroup
	wg.Add(*n)

	t0 := time.Now()
	for i := 0; i < *n; i++ {
		go func() {
			defer wg.Done()
			if doWork {
				s := 0
				for j := 0; j < workN; j++ {
					s += j
				}
				sink.Add(uint64(s))
			}
		}()
	}
	createS := time.Since(t0).Seconds()
	wg.Wait()
	totalS := time.Since(t0).Seconds()

	var ms runtime.MemStats
	runtime.ReadMemStats(&ms)

	out := map[string]any{
		"runtime":     "go_goroutines",
		"n":           *n,
		"workload":    *workload,
		"create_s":    createS,
		"total_s":     totalS,
		"rate":        float64(*n) / totalS,
		"rss_mb":      peakRSSMB(),
		"go_sys_mb":   float64(ms.Sys) / (1 << 20),
		"go_total_mb": float64(ms.TotalAlloc) / (1 << 20),
		"workers":     runtime.GOMAXPROCS(0),
	}
	b, _ := json.Marshal(out)
	fmt.Println(string(b))
}
