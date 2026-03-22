import gsyncio as gs
import time

async def worker(n):
    print(f"Worker {n} starting")
    return n * 2

def test_task_tracking():
    loop = gs.EventLoop()
    gs.set_current_loop(loop)
    
    t1 = gs.create_task(worker(10))
    t2 = gs.create_task(worker(20))
    
    print("Running loop...")
    loop.run()
    
    print(f"Task 1 done: {t1.done()}, result: {t1.result()}")
    print(f"Task 2 done: {t2.done()}, result: {t2.result()}")
    
    assert t1.result() == 20
    assert t2.result() == 40
    print("Task tracking test passed!")

if __name__ == "__main__":
    test_task_tracking()
