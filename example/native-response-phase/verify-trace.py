#!/usr/bin/env python3
"""Check C trace ordering, single commit, and cancelled-request cleanup."""
import argparse
import json
from pathlib import Path
import re


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    pattern = re.compile(r"native_response r=(\w+) seq=(\d+) event=(\w+) state=(\d+) "
                         r"header_sent=(\d+) header_calls=(\d+) count=(\d+) bytes=(\d+) uri=(\S+)")
    groups, current = [], {}
    text = args.log.read_text()
    for line in text.splitlines():
        match = pattern.search(line)
        if not match:
            continue
        pointer, seq, event, state, sent, calls, count, size, uri = match.groups()
        entry = {"event": event, "state": int(state), "header_sent": int(sent),
                 "header_calls": int(calls), "count": int(count), "bytes": int(size)}
        if int(seq) == 1:
            assert event == "collect_start", line
            group = {"uri": uri, "events": []}
            groups.append(group)
            current[pointer] = group
        group = current[pointer]
        assert int(seq) == len(group["events"]) + 1, line
        group["events"].append(entry)
    assert groups, "no native C-stage evidence found"
    passed, cancelled, errors, yields, resumes = 0, 0, 0, 0, 0
    for group in groups:
        rows = group["events"]
        names = [e["event"] for e in rows]
        assert names[-1] == "cleanup", group
        assert names.count("cleanup") == 1, group
        assert names.count("header_chain_enter") <= 1, group
        assert names.count("commit") <= 1, group
        yields += names.count("yield")
        resumes += names.count("resume")
        for entry in rows:
            if entry["event"] == "cleanup":
                # Nginx may run cleanup before or after dropping the final
                # connection reference (keepalive vs Connection: close).
                assert entry["count"] in (0, 1), group
            else:
                assert entry["count"] == 1, group
            if entry["event"] not in {"header_chain_enter", "cleanup"}:
                assert entry["header_calls"] == 0 and entry["header_sent"] == 0, group
        if "header_chain_enter" not in names:
            cancelled += 1
            assert "commit" not in names and rows[-1]["header_sent"] == 0, group
            continue
        assert names.index("commit") < names.index("header_chain_enter") < names.index("cleanup"), group
        assert rows[-1]["header_calls"] == 1, group
        if "response_leave" in names:
            passed += 1
            assert names.index("body_complete") < names.index("response_enter") < names.index("response_leave"), group
            assert names.index("response_leave") < names.index("header_chain_enter"), group
            if group["uri"] != "/native/output-guard":
                assert names.index("response_enter") < names.index("yield") < names.index("resume"), group
                assert names.index("resume") < names.index("response_leave"), group
        else:
            errors += 1
            assert names.index("fail_closed") < names.index("commit"), group
    assert cancelled >= 10 and passed >= 10 and errors >= 5 and resumes >= 10, (passed, cancelled, errors, resumes)
    assert "native_response_lua resumed_after_sleep phase=response" in text
    assert "native_header_observer phase=header_filter status=403 upstream_status=201" in text
    for severity in ("[alert]", "[crit]", "[emerg]"):
        assert severity not in text, severity
    assert "exited on signal" not in text and "header already sent" not in text
    # One intentional Lua exception is expected; any other error needs diagnosis.
    error_lines = [line for line in text.splitlines() if "[error]" in line]
    assert error_lines and all("intentional response-stage exception" in line
                               or "upstream prematurely closed connection" in line
                               for line in error_lines), error_lines
    result = {"passed": True, "native_requests": len(groups), "completed_lua_phases": passed,
              "fail_closed_requests": errors, "cancelled_before_headers": cancelled,
              "yield_events": yields, "resume_events": resumes,
              "header_chain_called_at_most_once": True, "pending_request_count_always_one": True,
              "intentional_or_fixture_error_lines": len(error_lines), "traces": groups}
    if args.output:
        args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({k: v for k, v in result.items() if k != "traces"}, indent=2))
    print("PASS: native C-stage ordering and lifecycle evidence")


if __name__ == "__main__":
    main()
