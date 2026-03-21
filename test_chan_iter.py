import gsyncio as gs
import asyncio

async def producer(ch):
    for i in range(1, 6):
        await gs.send(ch, i)
        print(f"Sent: {i}")
    gs.close(ch)

async def main():
    ch = gs.chan()
    gs.task(producer, ch)
    try:
        # Check if async for works
        async for num in ch:
            print(f"Received: {num}")
    except TypeError:
        print("async for not supported, using while loop")
        while not ch.closed:
            try:
                num = await gs.recv(ch)
                print(f"Received: {num}")
            except StopAsyncIteration:
                break
    print("Channel closed.")

if __name__ == "__main__":
    gs.run(main())
