#!/usr/bin/env python3
"""Finite cancellation/FD experiment; this is not a long-duration leak test."""
import argparse
import http.client
import json
from pathlib import Path
import socket
import subprocess
import time


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--port", type=int, default=19090)
    p.add_argument("--container", default="apisix-native-response-prototype")
    p.add_argument("--output", type=Path)
    args = p.parse_args()

    def stats():
        c = http.client.HTTPConnection("127.0.0.1", args.port, timeout=2)
        try:
            c.request("GET", "/stats", headers={"Connection": "close"})
            response = c.getresponse()
            assert response.status == 200
            return json.loads(response.read())
        finally:
            c.close()

    def wait_for(predicate, timeout=0.7):
        until = time.monotonic() + timeout
        while True:
            state = stats()
            if predicate(state):
                return state
            assert time.monotonic() < until, state
            time.sleep(0.01)

    def fds(pid):
        command = ["env", "-u", "DOCKER_HOST", "-u", "DOCKER_CONTEXT", "-u", "DOCKER_TLS",
                   "-u", "DOCKER_TLS_VERIFY", "-u", "DOCKER_CERT_PATH", "docker",
                   "--host=unix:///var/run/docker.sock", "exec", args.container,
                   "sh", "-c", f"ls /proc/{int(pid)}/fd | wc -l"]
        return int(subprocess.check_output(command, text=True))

    initial = wait_for(lambda s: s["upstream_active"] == s["audit_active"] == 0)
    pid = initial["worker_pid"]
    before = fds(pid)
    worst = 0
    for stage, case in (("upstream", "slow-upstream"), ("audit", "allow")):
        for _ in range(10):
            state = stats()
            with socket.create_connection(("127.0.0.1", args.port), timeout=2) as sock:
                sock.sendall(f"GET /native/{case} HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n".encode())
                wait_for(lambda s: s[stage + "_active"] == 1)
            start = time.monotonic()
            wait_for(lambda s: s[stage + "_active"] == 0
                     and s[stage + "_cancelled"] > state[stage + "_cancelled"])
            worst = max(worst, time.monotonic() - start)
    final = wait_for(lambda s: s["upstream_active"] == s["audit_active"] == 0)
    # Let any already-posted close event finish before observing process FDs.
    time.sleep(0.1)
    after = fds(pid)
    assert pid == final["worker_pid"], final
    assert after == before, (before, after)
    assert final["upstream_cancelled"] - initial["upstream_cancelled"] == 10
    assert final["audit_cancelled"] - initial["audit_cancelled"] == 10
    result = {"passed": True, "worker_pid": pid, "fd_before": before, "fd_after": after,
              "extra_client_aborts": 20, "cancelled_per_stage": 10,
              "max_observed_cleanup_s": round(worst, 4),
              "upstream_active": final["upstream_active"], "audit_active": final["audit_active"]}
    if args.output:
        args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))
    print("PASS: finite cancellation and file-descriptor check")


if __name__ == "__main__":
    main()
