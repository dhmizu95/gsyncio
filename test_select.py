
import gsyncio as gs
import time

async def test_select_close():
    c1 = gs.chan(1)
    
    async def closer():
        await gs.sleep(0.1)
        print("Closing c1...")
        c1.close()
    
    gs.create_task(closer())
    
    print("Waiting for closed channel in select...")
    res = await gs.select(gs.recv(c1))
    
    if res.channel == c1 and res.value is None:
        print("Select correctly handled closed channel!")
    else:
        print(f"FAILED: res.channel={res.channel}, res.value={res.value}")

async def main():
    # Previous tests
    c1 = gs.chan(10)
    c2 = gs.chan(10)

    async def p1():
        await gs.sleep(0.1)
        await c1.send("msg1")

    async def p2():
        await gs.sleep(0.2)
        await c2.send("msg2")

    gs.create_task(p1())
    gs.create_task(p2())

    print("Waiting in select...")
    result = await gs.select(gs.recv(c1), gs.recv(c2))
    print(f"Selected index {result.case_index}, value: {result.value}")

    print("Waiting in select...")
    result = await gs.select(gs.recv(c1), gs.recv(c2))
    print(f"Selected index {result.case_index}, value: {result.value}")

    # New close test
    await test_select_close()
    
    print("All tests passed!")

if __name__ == "__main__":
    gs.run(main())
