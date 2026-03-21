import gsyncio as gs
import asyncio
import time

async def worker():
    ch = gs.chan(1)
    await gs.send(ch, 42)
    val = await gs.recv(ch)
    print(f"Value: {val}")

if __name__ == "__main__":
    gs.run(worker)
