#!/usr/bin/env python3
"""
Go Producer/Consumer - gsyncio equivalent of Go's channel producer/consumer

Go code:
    func producer(ch chan<- int) {
        for i := 1; i <= 5; i++ {
            ch <- i
            fmt.Printf("Sent: %d\n", i)
        }
        close(ch)
    }

    func main() {
        ch := make(chan int)
        go producer(ch)
        for num := range ch {
            fmt.Printf("Received: %d\n", num)
        }
        fmt.Println("Channel closed.")
    }

Key: both producer and consumer must be gs.task() fibers so that
fiber parking / scheduling works correctly.
"""

import gsyncio as gs


def producer(ch):
    """Producer fiber - sends values 1-5 then closes the channel."""
    for i in range(1, 6):
        ch.send_nowait(i)
        print(f"Sent: {i}")
    gs.close(ch)


def consumer(ch):
    """Consumer fiber - drains all values, mirrors Go's 'for range ch'."""
    while True:
        val = ch.recv_nowait()
        if val is not None:
            print(f"Received: {val}")
        elif ch.size == 0 and ch.closed:
            # Buffer empty AND channel closed - done (like Go's range ending)
            break
    print("Channel closed.")


def main():
    # Buffered channel so producer never blocks waiting for consumer
    ch = gs.chan(5)  # capacity 5: producer writes all, consumer drains
    gs.task(producer, ch)
    gs.task(consumer, ch)
    gs.sync()


if __name__ == "__main__":
    gs.run(main)

