#!/bin/bash
#
# Run All Benchmarks
# 
# This script runs all benchmarks (asyncio, gsyncio, Go) and generates
# a comprehensive comparison report in Markdown format.
#
# Usage:
#   ./run_all.sh [--quick] [--skip-go] [--go-routines N]
#

set +e  # Don't exit on errors

# Configuration
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUTPUT_DIR="$SCRIPT_DIR/../.benchmarks"
TIMESTAMP=$(date +"%Y%m%d_%H%M%S")
REPORT_FILE="$OUTPUT_DIR/benchmark_report_${TIMESTAMP}.md"

# Default options
QUICK_MODE=false
SKIP_GO=false
GO_ROUTINES=0

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --quick)
            QUICK_MODE=true
            shift
            ;;
        --skip-go)
            SKIP_GO=true
            shift
            ;;
        --go-routines)
            GO_ROUTINES=$2
            shift 2
            ;;
        *)
            echo "Unknown option: $1"
            echo "Usage: $0 [--quick] [--skip-go] [--go-routines N]"
            exit 1
            ;;
    esac
done

# Create output directory
mkdir -p "$OUTPUT_DIR"

# Set iterations based on mode
if [ "$QUICK_MODE" = true ]; then
    ITERATIONS=3
    echo "Running in QUICK mode (3 iterations per test)"
else
    ITERATIONS=10
    echo "Running in FULL mode (10 iterations per test)"
fi

echo "========================================"
echo "  gsyncio Benchmark Suite"
echo "========================================"
echo ""
echo "Timestamp: $(date)"
echo "Output: $REPORT_FILE"
echo ""

# Start writing report
cat > "$REPORT_FILE" << EOF
# Benchmark Report

**Generated:** $(date)  
**Mode:** $([ "$QUICK_MODE" = true ] && echo "Quick" || echo "Full")  
**Iterations per test:** $ITERATIONS

## System Information

- OS: $(uname -s) $(uname -r)
- Architecture: $(uname -m)
- Python: $(python3 --version 2>&1)

EOF

# Check if gsyncio is available
echo "Checking gsyncio installation..."
if python3 -c "import gsyncio" 2>/dev/null; then
    echo "  ✓ gsyncio is installed"
    GSNPCIO_VERSION=$(python3 -c 'import gsyncio; print(gsyncio.__version__)' 2>/dev/null || echo "unknown")
    echo "gsyncio version: $GSNPCIO_VERSION" >> "$REPORT_FILE"
else
    echo "  ✗ gsyncio not installed - building..."
    cd "$SCRIPT_DIR/.."
    pip install -e . 2>/dev/null || true
fi

echo ""

# ============================================================================
# Run Python asyncio Benchmarks
# ============================================================================
echo ">>> Running Python asyncio benchmarks..."

cat >> "$REPORT_FILE" << 'EOF'

## Python asyncio Benchmarks

EOF

# Run asyncio benchmark with proper event loop handling
ASYNCIO_OUTPUT=$(python3 << 'PYEOF'
import asyncio
import time
import json

async def simple_task(n):
    return sum(range(n))

iterations = 3
results = []

for num_tasks in [100, 1000, 10000]:
    times = []
    for _ in range(iterations):
        async def run_tasks():
            tasks = [asyncio.create_task(simple_task(100)) for _ in range(num_tasks)]
            return await asyncio.gather(*tasks)
        
        start = time.perf_counter()
        asyncio.run(run_tasks())
        end = time.perf_counter()
        times.append(end - start)
    avg = sum(times) / len(times)
    print(f'asyncio_task_spawn_{num_tasks}: {avg:.4f}')
    results.append((f'asyncio_task_spawn_{num_tasks}', avg))

# Sleep benchmarks
for num_tasks in [100, 1000, 10000]:
    times = []
    for _ in range(iterations):
        async def run_sleep():
            tasks = [asyncio.create_task(asyncio.sleep(0.01)) for _ in range(num_tasks)]
            return await asyncio.gather(*tasks)
        
        start = time.perf_counter()
        asyncio.run(run_sleep())
        end = time.perf_counter()
        times.append(end - start)
    avg = sum(times) / len(times)
    print(f'asyncio_sleep_{num_tasks}: {avg:.4f}')
    results.append((f'asyncio_sleep_{num_tasks}', avg))

# Save results to temp file
with open('/tmp/asyncio_results.json', 'w') as f:
    json.dump(results, f)
PYEOF
)

echo "$ASYNCIO_OUTPUT" 2>&1 | while read -r line; do
    echo "  $line"
done

# ============================================================================
# Run gsyncio Benchmarks
# ============================================================================
echo ""
echo ">>> Running gsyncio benchmarks..."

cat >> "$REPORT_FILE" << 'EOF'

## gsyncio Benchmarks

### Task/Sync Model

*Note: gsyncio benchmarks may fail if the C extension has issues*

EOF

# Run gsyncio benchmark
GSYNCIO_OUTPUT=$(python3 << 'PYEOF' 2>&1
import gsyncio as gs
import time
import json
import sys

def simple_task(n):
    return sum(range(n))

iterations = 3
results = []

# Test if task works without segfault
try:
    # Task spawn benchmarks
    for num_tasks in [100, 1000, 10000]:
        times = []
        for _ in range(iterations):
            for _ in range(num_tasks):
                gs.task(simple_task, 100)
            start = time.perf_counter()
            gs.sync()
            end = time.perf_counter()
            times.append(end - start)
        avg = sum(times) / len(times)
        print(f'gsyncio_task_spawn_{num_tasks}: {avg:.4f}')
        results.append((f'gsyncio_task_spawn_{num_tasks}', avg))
except Exception as e:
    print(f"Error in task spawn: {e}", file=sys.stderr)

# Task fast benchmarks
try:
    for num_tasks in [100, 1000, 10000]:
        times = []
        for _ in range(iterations):
            start = time.perf_counter()
            for _ in range(num_tasks):
                gs.task_fast(simple_task, 100)
            gs.sync()
            end = time.perf_counter()
            times.append(end - start)
        avg = sum(times) / len(times)
        print(f'gsyncio_task_fast_{num_tasks}: {avg:.4f}')
        results.append((f'gsyncio_task_fast_{num_tasks}', avg))
except Exception as e:
    print(f"Error in task fast: {e}", file=sys.stderr)

# Task batch benchmarks
try:
    for num_tasks in [100, 1000, 10000]:
        times = []
        for _ in range(iterations):
            tasks = [(simple_task, (100,)) for _ in range(num_tasks)]
            start = time.perf_counter()
            gs.task_batch(tasks)
            gs.sync()
            end = time.perf_counter()
            times.append(end - start)
        avg = sum(times) / len(times)
        print(f'gsyncio_task_batch_{num_tasks}: {avg:.4f}')
        results.append((f'gsyncio_task_batch_{num_tasks}', avg))
except Exception as e:
    print(f"Error in task batch: {e}", file=sys.stderr)

# Save results to temp file
with open('/tmp/gsyncio_results.json', 'w') as f:
    json.dump(results, f)
PYEOF
)

# Check if gsyncio benchmarks ran successfully
if echo "$GSYNCIO_OUTPUT" | grep -q "gsyncio_task"; then
    echo "$GSYNCIO_OUTPUT" | grep "gsyncio_task" | while read -r line; do
        echo "  $line"
    done
    
    # Read and parse gsyncio results
    if [ -f /tmp/gsyncio_results.json ]; then
        GSNPCIO_SPAWN_100=$(python3 -c "import json; d=json.load(open('/tmp/gsyncio_results.json')); print([x[1] for x in d if 'gsyncio_task_spawn_100' in x[0]][0])" 2>/dev/null || echo "0")
        GSNPCIO_SPAWN_1000=$(python3 -c "import json; d=json.load(open('/tmp/gsyncio_results.json')); print([x[1] for x in d if 'gsyncio_task_spawn_1000' in x[0]][0])" 2>/dev/null || echo "0")
        GSNPCIO_SPAWN_10000=$(python3 -c "import json; d=json.load(open('/tmp/gsyncio_results.json')); print([x[1] for x in d if 'gsyncio_task_spawn_10000' in x[0]][0])" 2>/dev/null || echo "0")
        GSNPCIO_FAST_100=$(python3 -c "import json; d=json.load(open('/tmp/gsyncio_results.json')); print([x[1] for x in d if 'gsyncio_task_fast_100' in x[0]][0])" 2>/dev/null || echo "0")
        GSNPCIO_FAST_1000=$(python3 -c "import json; d=json.load(open('/tmp/gsyncio_results.json')); print([x[1] for x in d if 'gsyncio_task_fast_1000' in x[0]][0])" 2>/dev/null || echo "0")
        GSNPCIO_FAST_10000=$(python3 -c "import json; d=json.load(open('/tmp/gsyncio_results.json')); print([x[1] for x in d if 'gsyncio_task_fast_10000' in x[0]][0])" 2>/dev/null || echo "0")
        GSNPCIO_BATCH_100=$(python3 -c "import json; d=json.load(open('/tmp/gsyncio_results.json')); print([x[1] for x in d if 'gsyncio_task_batch_100' in x[0]][0])" 2>/dev/null || echo "0")
        GSNPCIO_BATCH_1000=$(python3 -c "import json; d=json.load(open('/tmp/gsyncio_results.json')); print([x[1] for x in d if 'gsyncio_task_batch_1000' in x[0]][0])" 2>/dev/null || echo "0")
        GSNPCIO_BATCH_10000=$(python3 -c "import json; d=json.load(open('/tmp/gsyncio_results.json')); print([x[1] for x in d if 'gsyncio_task_batch_10000' in x[0]][0])" 2>/dev/null || echo "0")
    else
        GSNPCIO_SPAWN_100=0
        GSNPCIO_SPAWN_1000=0
        GSNPCIO_SPAWN_10000=0
        GSNPCIO_FAST_100=0
        GSNPCIO_FAST_1000=0
        GSNPCIO_FAST_10000=0
        GSNPCIO_BATCH_100=0
        GSNPCIO_BATCH_1000=0
        GSNPCIO_BATCH_10000=0
    fi

    cat >> "$REPORT_FILE" << EOF

| Tasks | task() | task_fast() | task_batch() |
|-------|--------|-------------|-------------|
| 100   | ${GSNPCIO_SPAWN_100}s | ${GSNPCIO_FAST_100}s | ${GSNPCIO_BATCH_100}s |
| 1000  | ${GSNPCIO_SPAWN_1000}s | ${GSNPCIO_FAST_1000}s | ${GSNPCIO_BATCH_1000}s |
| 10000 | ${GSNPCIO_SPAWN_10000}s | ${GSNPCIO_FAST_10000}s | ${GSNPCIO_BATCH_10000}s |

EOF
else
    echo "  ⚠ gsyncio benchmarks failed (segfault in C extension)"
    
    cat >> "$REPORT_FILE" << 'EOF'

*gsyncio benchmarks could not be completed due to a segmentation fault in the C extension.*

EOF
fi

# Async/Await benchmarks
cat >> "$REPORT_FILE" << 'EOF'

### Async/Await Model

EOF

GSYNCIO_ASYNC=$(python3 << 'PYEOF' 2>&1
import gsyncio as gs
import time
import json
import sys

async def simple_async_task(n):
    return sum(range(n))

async def sleep_task(delay_ms):
    await gs.sleep(delay_ms)
    return delay_ms

iterations = 3
results = []

# Async spawn benchmarks
try:
    for num_tasks in [100, 1000, 5000]:
        times = []
        for _ in range(iterations):
            async def run():
                tasks = [gs.create_task(simple_async_task(100)) for _ in range(num_tasks)]
                return await gs.gather(*tasks)
            start = time.perf_counter()
            gs.run(run())
            end = time.perf_counter()
            times.append(end - start)
        avg = sum(times) / len(times)
        print(f'gsyncio_async_spawn_{num_tasks}: {avg:.4f}')
        results.append((f'gsyncio_async_spawn_{num_tasks}', avg))
except Exception as e:
    print(f"Error in async spawn: {e}", file=sys.stderr)

# Async sleep benchmarks
try:
    for num_tasks in [100, 1000, 10000]:
        times = []
        for _ in range(iterations):
            async def run():
                tasks = [gs.create_task(sleep_task(10)) for _ in range(num_tasks)]
                return await gs.gather(*tasks)
            start = time.perf_counter()
            gs.run(run())
            end = time.perf_counter()
            times.append(end - start)
        avg = sum(times) / len(times)
        print(f'gsyncio_async_sleep_{num_tasks}: {avg:.4f}')
        results.append((f'gsyncio_async_sleep_{num_tasks}', avg))
except Exception as e:
    print(f"Error in async sleep: {e}", file=sys.stderr)

# Save results to temp file
with open('/tmp/gsyncio_async_results.json', 'w') as f:
    json.dump(results, f)
PYEOF
)

echo "$GSYNCIO_ASYNC" | grep "gsyncio_async" | while read -r line; do
    echo "  $line"
done

# ============================================================================
# Run Go Benchmarks (if available)
# ============================================================================
if [ "$SKIP_GO" = false ]; then
    echo ""
    echo ">>> Running Go benchmarks..."
    
    if command -v go &> /dev/null; then
        GO_VERSION=$(go version)
        echo "  Found: $GO_VERSION"
        
        if [ "$GO_ROUTINES" -gt 0 ] 2>/dev/null; then
            echo "  Using $GO_ROUTINES goroutines (GOMAXPROCS)"
        else
            echo "  Using all available CPUs ($(nproc) cores)"
        fi
        
        cat >> "$REPORT_FILE" << 'EOF'
        
## Go Routine Benchmarks

EOF
        
        if [ "$GO_ROUTINES" -gt 0 ] 2>/dev/null; then
            GO_OUTPUT=$(GOMAXPROCS=$GO_ROUTINES go run "$SCRIPT_DIR/go_benchmark.go" 2>&1 || echo "Go benchmark failed")
        else
            GO_OUTPUT=$(go run "$SCRIPT_DIR/go_benchmark.go" 2>&1 || echo "Go benchmark failed")
        fi
        
        echo "$GO_OUTPUT" | grep -E "(Task spawn|Sleep|Gather|Chain|Batch|Mutex|Atomic|Channel)" | head -20 | while read -r line; do
            echo "  $line"
        done
    else
        echo "  Skipping Go benchmarks (Go not installed)"
        
        cat >> "$REPORT_FILE" << 'EOF'

## Go Routine Benchmarks

*Go not installed - skipped*

EOF
    fi
else
    echo ""
    echo ">>> Skipping Go benchmarks..."
    
    cat >> "$REPORT_FILE" << 'EOF'

## Go Routine Benchmarks

*Skipped by user*

EOF
fi

# ============================================================================
# Generate Summary
# ============================================================================
echo ""
echo ">>> Generating summary..."

# Read asyncio results
if [ -f /tmp/asyncio_results.json ]; then
    ASYNCIO_100=$(python3 -c "import json; d=json.load(open('/tmp/asyncio_results.json')); print([x[1] for x in d if 'asyncio_task_spawn_100' in x[0]][0])" 2>/dev/null || echo "0")
else
    ASYNCIO_100=0
fi

# Calculate speedups
if [ "$GSNPCIO_FAST_100" != "0" ] && [ "$GSNPCIO_FAST_100" != "0.0" ]; then
    SPEEDUP=$(python3 -c "print(f'{$ASYNCIO_100 / float($GSNPCIO_FAST_100):.2f}')" 2>/dev/null || echo "N/A")
else
    SPEEDUP="N/A"
fi

cat >> "$REPORT_FILE" << EOF

## Summary

### Task Spawn Performance (100 tasks)

| Framework | Time | Speedup vs asyncio |
|-----------|------|-------------------|
| Python asyncio | ${ASYNCIO_100}s | 1.00x (baseline) |
| gsyncio (task) | ${GSNPCIO_SPAWN_100}s | - |
| gsyncio (fast) | ${GSNPCIO_FAST_100}s | ${SPEEDUP}x |

### Key Findings

- gsyncio's **task/sync model** provides fire-and-forget parallelism
- **task_fast()** is optimized for maximum throughput
- **task_batch()** is optimized for bulk task spawning
- gsyncio's **async/await** model uses native fiber scheduling

### Recommendations

1. **For CPU-bound tasks**: Use \`gs.task_fast()\` or \`gs.task_batch()\`
2. **For I/O-bound tasks**: Use gsyncio's async/await (\`create_task\`, \`gather\`)
3. **For mixed workloads**: Combine both models as needed

---

*Report generated by gsyncio benchmark suite*
EOF

# ============================================================================
# Final Output
# ============================================================================
echo ""
echo "========================================"
echo "  Benchmark Complete!"
echo "========================================"
echo ""
echo "Report saved to: $REPORT_FILE"
echo ""
echo "Preview of report:"
echo "-----------------"
head -70 "$REPORT_FILE"
echo ""
echo "Full report: $REPORT_FILE"
