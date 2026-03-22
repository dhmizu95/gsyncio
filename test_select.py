import gsyncio as gs
import time

async def producer(c1, c2):
    await gs.sleep(0.1)
    await c1.send("msg1")
    await gs.sleep(0.1)
    await c2.send("msg2")

async def consumer(c1, c2):
    for _ in range(2):
        print("Waiting in select...")
        result = await gs.select(
            gs.recv(c1),
            gs.recv(c2)
        )
        print(f"Selected index {result.case_index}, value: {result.value}")
        if result.case_index == 0:
            assert result.value == "msg1"
        else:
            assert result.value == "msg2"

async def test_select():
    c1 = gs.chan(0)
    c2 = gs.chan(0)
    
    gs.create_task(producer(c1, c2))
    await consumer(c1, c2)
    print("Select test passed!")

if __name__ == "__main__":
    loop = gs.EventLoop()
    gs.set_current_loop(loop)
    loop.run_main(test_select())
