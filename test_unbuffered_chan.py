import gsyncio as gs
import asyncio

async def producer(ch):
    for i in range(1, 6):
        print(f"Producer trying to send {i}...")
        await gs.send(ch, i)
        print(f"Sent: {i}")
    gs.close(ch)

async def main():
    ch = gs.chan() # Unbuffered
    gs.task(producer, ch)
    print("Receiver starting loop...")
    while not ch.closed:
        try:
            num = await gs.recv(ch)
            print(f"Received: {num}")
        except StopAsyncIteration:
            break
    print("Channel closed.")

if __name__ == "__main__":
    gs.run(main)
