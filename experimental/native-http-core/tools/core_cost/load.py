#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Fixed scheduled arrivals over bounded keepalive connections, no dropped slots.

Records latency from scheduled arrival as well as actual send; late driver work
is retained. This is a diagnostic generator, not wrk throughput evidence.
"""
import argparse
import asyncio
import json
import math
from pathlib import Path
import time


def percentile(values, fraction):
    return values[max(0, math.ceil(len(values) * fraction) - 1)] if values else None


async def run(rate, seconds, connections):
    count = rate * seconds
    latency, wire, lateness = [], [], []
    errors = []
    sent = b"GET /limit/benchmark HTTP/1.1\r\nHost: localhost\r\napikey: demo-key\r\nConnection: keep-alive\r\n\r\n"
    sockets = await asyncio.gather(*(asyncio.open_connection("127.0.0.1", 9080) for _ in range(connections)))
    start = time.perf_counter() + 0.2

    async def worker(index, reader, writer):
        try:
            for sequence in range(index, count, connections):
                due = start + sequence / rate
                pause = due - time.perf_counter()
                if pause > 0:
                    await asyncio.sleep(pause)
                sending = time.perf_counter()
                writer.write(sent)
                await writer.drain()
                raw = await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), timeout=10)
                lines = raw.split(b"\r\n")
                status = int(lines[0].split()[1])
                headers = dict(line.split(b":", 1) for line in lines[1:] if b":" in line)
                headers = {k.lower(): v.strip().lower() for k, v in headers.items()}
                if b"content-length" in headers:
                    body = await reader.readexactly(int(headers[b"content-length"]))
                elif headers.get(b"transfer-encoding") == b"chunked":
                    chunks = []
                    while True:
                        size = int((await reader.readline()).split(b";", 1)[0], 16)
                        if not size:
                            while True:
                                trailer = await reader.readline()
                                if not trailer:
                                    raise EOFError("truncated chunk trailer")
                                if trailer == b"\r\n":
                                    break
                            break
                        chunks.append(await reader.readexactly(size))
                        assert await reader.readexactly(2) == b"\r\n"
                    body = b"".join(chunks)
                else:
                    raise AssertionError("unknown response framing")
                done = time.perf_counter()
                assert status == 200 and body == b"ok\n", (status, body[:200])
                latency.append((done - due) * 1e6)
                wire.append((done - sending) * 1e6)
                lateness.append(max(0, sending - due) * 1e6)
                if headers.get(b"connection") == b"close":
                    writer.close()
                    await writer.wait_closed()
                    if sequence + connections < count:
                        reader, writer = await asyncio.open_connection("127.0.0.1", 9080)
        except Exception as error:
            errors.append(repr(error))
        finally:
            writer.close()
            await writer.wait_closed()

    await asyncio.gather(*(worker(i, *pair) for i, pair in enumerate(sockets)))
    wall = time.perf_counter() - start
    latency.sort(); wire.sort(); lateness.sort()
    def describe(values):
        return {"mean": sum(values) / len(values) if values else None,
                "p50": percentile(values, .50), "p95": percentile(values, .95),
                "p99": percentile(values, .99), "max": values[-1] if values else None}
    return {"requested_rate": rate, "duration_seconds": seconds, "connections": connections,
            "target_requests": count, "completed": len(latency), "wall_seconds": wall,
            "observed_rps": len(latency) / wall,
            "scheduled_latency_us": describe(latency), "wire_latency_us": describe(wire),
            "driver_lateness_us": describe(lateness), "errors": errors,
            "all_slots_completed": len(latency) == count and not errors,
            "schedule_drained_within_one_second": wall < seconds + 1}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rate", type=int, default=8000)
    parser.add_argument("--seconds", type=int, default=60)
    parser.add_argument("--connections", type=int, default=32)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    result = asyncio.run(run(args.rate, args.seconds, args.connections))
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result), flush=True)
    assert result["all_slots_completed"] and result["schedule_drained_within_one_second"], "driver failed its fixed-load contract"
