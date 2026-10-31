#!/usr/bin/env python3
"""Black-box checks against real, single-worker APISIX. Python stdlib only."""
import argparse
import http.client
import json
from pathlib import Path
import socket
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=19090)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    results = []

    def fetch(path, method="GET"):
        conn = http.client.HTTPConnection("127.0.0.1", args.port, timeout=5)
        start = time.monotonic()
        try:
            conn.request(method, path, headers={"Connection": "close"})
            response = conn.getresponse()
            first = time.monotonic() - start
            headers = response.getheaders()
            body = response.read()
            return response.status, headers, body, first, time.monotonic() - start
        finally:
            conn.close()

    def stats():
        status, _, body, _, _ = fetch("/stats")
        assert status == 200, (status, body)
        return json.loads(body)

    def wait_for(predicate, timeout=2):
        end = time.monotonic() + timeout
        while True:
            state = stats()
            if predicate(state):
                return state
            if time.monotonic() > end:
                raise AssertionError(f"stats deadline: {state}")
            time.sleep(0.02)

    def raw_request(path):
        sock = socket.create_connection(("127.0.0.1", args.port), timeout=5)
        sock.sendall(f"GET {path} HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n".encode())
        return sock

    def record(name, **values):
        entry = {"test": name, "passed": True, **values}
        results.append(entry)
        print(json.dumps(entry, ensure_ascii=False), flush=True)

    # Wait for standalone YAML to load (not just for the TCP port to listen).
    for attempt in range(100):
        try:
            if fetch("/plain")[0] == 200:
                break
        except (OSError, http.client.HTTPException):
            pass
        time.sleep(0.1)
    else:
        raise AssertionError("APISIX did not become ready")

    status, _, body, _, _ = fetch("/capabilities")
    caps = json.loads(body)
    assert status == 200 and caps["ngx_lua"] == 10028, caps
    for phase in ("header_filter", "body_filter"):
        assert "API disabled in the context of" in caps[phase], caps
    record("runtime filter cosocket probes", **caps)

    status, pairs, body, first, _ = fetch("/block")
    headers = {k.lower(): v for k, v in pairs}
    assert (status, body) == (202, b"native block response\n")
    assert headers["x-block-lua"] == "response" and headers["x-block-observer"] == "header_filter"
    assert int(headers["content-length"]) == len(body) and first >= 0.04
    record("inline Lua injection and timer resume", status=status, bytes=len(body), first_header_s=round(first, 4))

    # Synchronize on the reviewer actually waiting. No byte, including a
    # status line or a header, may reach this socket while the verdict is pending.
    start = time.monotonic()
    with raw_request("/native/allow") as sock:
        wait_for(lambda s: s["audit_active"] == 1)
        sock.settimeout(0.3)
        try:
            leaked = sock.recv(1)
        except socket.timeout:
            leaked = None
        assert leaked is None, f"premature downstream byte: {leaked!r}"
        status, _, plain, _, plain_time = fetch("/plain")
        assert status == 200 and plain == b"plain ok\n" and plain_time < 0.4
        assert stats()["audit_active"] == 1, "plain request must finish during review"
        sock.settimeout(3)
        raw = bytearray()
        while True:
            chunk = sock.recv(65536)
            if not chunk:
                break
            raw.extend(chunk)
        duration = time.monotonic() - start
        assert b"HTTP/1.1 201" in raw and raw.endswith(b"hello from upstream\n"), raw
        assert duration >= 0.9, duration
        record("no early bytes and concurrent plain request", silent_window_ms=300,
               audit_total_s=round(duration, 4), plain_total_s=round(plain_time, 4))

    expected = [
        ("allow", 201, b"hello from upstream\n"),
        ("deny", 403, b"response rejected by audit\n"),
        ("normalized-path", 403, b"response rejected by audit\n"),
        ("chunked", 201, b"hello from upstream\n"),
        ("deny-chunked", 403, b"response rejected by audit\n"),
        ("utf8", 201, "小文本审核通过\n".encode()),
        ("empty", 201, b""),
        ("status-404", 404, b"small upstream not found\n"),
        ("status-503", 503, b"small upstream unavailable\n"),
        ("exact", 201, b"e" * 4096),
        ("sleep", 201, b"LOCAL_SLEEP\n"),
        ("output-guard", 201, b"OUTPUT_GUARD\n"),
        ("lua-error", 500, b"response Lua failure\n"),
        ("large-length", 502, b"upstream body too large\n"),
        ("large-chunked", 502, b"upstream body too large\n"),
        ("timeout", 504, b"response phase timeout\n"),
        ("audit-error", 502, b"audit failure\n"),
        ("audit-invalid", 502, b"audit failure\n"),
        ("audit-oversize", 502, b"audit failure\n"),
        ("audit-trickle", 504, b"response phase timeout\n"),
        ("audit-disconnect", 502, b"audit failure\n"),
        ("slow-upstream", 504, b"upstream collection timeout\n"),
        ("upstream-trickle", 504, b"upstream collection timeout\n"),
        ("broken", 502, b"upstream read failure\n"),
        ("disconnect", 502, None),
        ("not-text", 502, b"unsupported response for native phase\n"),
        ("encoded", 502, b"unsupported response for native phase\n"),
    ]
    pre_audit_failures = {"large-length", "large-chunked", "slow-upstream",
                          "upstream-trickle", "broken", "disconnect", "not-text", "encoded"}
    for case, wanted_status, wanted_body in expected:
        before = stats()
        path = "/%6Eative/deny" if case == "normalized-path" else "/native/" + case
        status, pairs, body, first, total = fetch(path)
        headers = {k.lower(): v for k, v in pairs}
        assert status == wanted_status and (wanted_body is None or body == wanted_body), (case, status, body)
        assert int(headers["content-length"]) == len(body), (case, headers, len(body))
        assert "transfer-encoding" not in headers, (case, headers)
        if case in {"allow", "chunked", "utf8", "empty", "exact", "status-404", "status-503", "sleep", "output-guard"}:
            assert headers.get("x-origin") == "retained", (case, headers)
            assert [v for k, v in pairs if k.lower() == "set-cookie"] == ["demo_a=1", "demo_b=2"]
            if case != "output-guard":
                assert first >= 0.9, (case, first)
        else:
            assert "x-origin" not in headers and "set-cookie" not in headers
        if case not in pre_audit_failures and case not in {"lua-error", "output-guard"}:
            if case != "output-guard":
                assert first >= 0.9, (case, first)
        if case in pre_audit_failures:
            assert stats()["audit_started"] == before["audit_started"], case
        if case in ("timeout", "audit-trickle"):
            assert 1.35 <= total < 2.3, (case, total)
        if case in ("slow-upstream", "upstream-trickle"):
            assert 1.05 <= total < 2, (case, total)
        assert headers.get("x-header-observer") == str(status), (case, headers)
        if wanted_status == 201 or case in {"deny", "normalized-path", "deny-chunked", "status-404", "status-503"}:
            assert headers.get("x-native-phase") == "response", (case, headers)
            assert headers.get("x-native-upstream-status") == str(404 if case == "status-404" else 503 if case == "status-503" else 201), (case, headers)
        record(case, status=status, bytes=len(body), first_header_s=round(first, 4),
               total_s=round(total, 4), content_length=int(headers["content-length"]))

    # Close while reading upstream and while awaiting review. The mock's active
    # counters must promptly fall to zero, with cancelled work rather than
    # waiting for its normal 1s/3s completion. Repeat to expose retained sockets.
    for stage, case in (("upstream", "slow-upstream"), ("audit", "allow")):
        initial = wait_for(lambda s: s["audit_active"] == 0 and s["upstream_active"] == 0)
        for _ in range(5):
            before = stats()
            with raw_request("/native/" + case):
                wait_for(lambda s: s[stage + "_active"] == 1)
            closed = time.monotonic()
            final = wait_for(lambda s: s[stage + "_active"] == 0
                             and s[stage + "_cancelled"] > before[stage + "_cancelled"], timeout=0.7)
            assert time.monotonic() - closed < 0.7
        assert final[stage + "_cancelled"] - initial[stage + "_cancelled"] == 5
        record("client abort during " + stage, attempts=5, cleanup_deadline_s=0.7,
               cancelled_delta=5)

    final = wait_for(lambda s: s["audit_active"] == 0 and s["upstream_active"] == 0)
    assert final["worker_pid"] == caps["worker_pid"], (final, caps)
    record("all mock requests released", **final)
    report = {"passed": True, "checks": len(results), "results": results}
    if args.output:
        args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    print(f"PASS: {len(results)} checks", flush=True)


if __name__ == "__main__":
    main()
