import asyncio
import errno
import os


async def ready(fd, writing=False):
    loop = asyncio.get_running_loop()
    future = loop.create_future()
    def wake():
        if not future.done():
            future.set_result(None)
    add = loop.add_writer if writing else loop.add_reader
    remove = loop.remove_writer if writing else loop.remove_reader
    add(fd, wake)
    try:
        await future
    finally:
        remove(fd)


async def read_fd(fd):
    while True:
        try:
            return os.read(fd, 65536)
        except BlockingIOError:
            await ready(fd)
        except OSError as error:
            if error.errno == errno.EIO:
                return b""
            raise


async def write_fd(fd, data):
    data = memoryview(data)
    while data:
        try:
            data = data[os.write(fd, data):]
        except BlockingIOError:
            await ready(fd, writing=True)
