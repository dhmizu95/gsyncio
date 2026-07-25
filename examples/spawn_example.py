"""
spawn() supports two input shapes:

  - list/tuple  -> default, fastest path (worker-tuned chunk size)
  - generator   -> streamed path, chunks of 4096, avoids materializing
                   the whole batch in memory at once
"""
import gsyncio as gs


def work(n):
    return sum(range(n))


def main():
    gs.init_scheduler(num_workers=0)  # 0 = auto-detect CPUs
    gs.sync()

    # Default: pass a list. Fastest for batches you already have in memory.
    gs.spawn([(work, (100,)) for _ in range(10_000)])
    gs.sync()
    print("list path done")

    # Streamed: pass a generator. Use this for very large N where building
    # the full list upfront would blow up memory (e.g. 10M+ tasks).
    gs.spawn((work, (i % 1000,)) for i in range(10_000))
    gs.sync()
    print("generator path done")

    gs.shutdown_scheduler(wait=True)


if __name__ == "__main__":
    main()
