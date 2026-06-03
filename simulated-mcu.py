#!/usr/bin/env python3

from __future__ import annotations

import mmap
import os
import random
import signal
import struct
import time


SHM_PATH = os.environ.get("TELEMETRY_SHM_PATH", "/dev/shm/data")
PACKET_SIZE = int(os.environ.get("TELEMETRY_PACKET_SIZE", "26"))
DROP_PERCENT = max(0, min(100, int(os.environ.get("MCU_DROP_PERCENT", "5"))))
WRITE_INTERVAL_SECONDS = float(os.environ.get("MCU_WRITE_INTERVAL_SECONDS", "0.1"))

MAGIC = 0xDEAD
SOFTWARE_VERSION = b"1.0.0"
PACKET_FORMAT = "<HIIffH6s"

running = True


def stop(_signum: int, _frame: object) -> None:
    global running
    running = False


def timestamp_ms_uint32() -> int:
    return int(time.time() * 1000) & 0xFFFFFFFF


def build_packet(seq: int) -> bytes:
    version = SOFTWARE_VERSION[:5].ljust(6, b"\0")
    return struct.pack(
        PACKET_FORMAT,
        MAGIC,
        seq & 0xFFFFFFFF,
        timestamp_ms_uint32(),
        40.0 + float(seq % 60),
        12.0 + float(seq % 20) * 0.05,
        0x0001 if seq % 50 == 0 else 0,
        version,
    )


def open_shared_memory() -> mmap.mmap:
    directory = os.path.dirname(SHM_PATH)
    if directory:
        os.makedirs(directory, exist_ok=True)
    fd = os.open(SHM_PATH, os.O_CREAT | os.O_RDWR, 0o600)
    try:
        os.ftruncate(fd, PACKET_SIZE)
        return mmap.mmap(fd, PACKET_SIZE)
    finally:
        os.close(fd)


def main() -> int:
    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)

    seq = 0
    print(f"simulated-mcu writing {PACKET_SIZE} byte packets to {SHM_PATH}", flush=True)

    with open_shared_memory() as shared_memory:
        while running:
            seq = (seq + 1) & 0xFFFFFFFF
            if random.randrange(100) >= DROP_PERCENT:
                shared_memory.seek(0)
                shared_memory.write(build_packet(seq))
            time.sleep(WRITE_INTERVAL_SECONDS)

    try:
        os.unlink(SHM_PATH)
    except FileNotFoundError:
        pass

    print("simulated-mcu stopped", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
