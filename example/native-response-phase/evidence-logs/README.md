# Recorded feasibility experiment transcripts

These are unchanged text copies of the local experiment logs. They contain
synthetic localhost requests, not production traffic. The `.txt` extension makes
the archived transcripts directly browsable in the research PR.

| Transcript | Purpose |
| --- | --- |
| [main.txt](main.txt) | Main HTTP run plus the additional resource-cancellation probes |
| [replay.txt](replay.txt) | Independent runtime built from the exported native patch |
| [build.txt](build.txt) | Main prototype compilation |
| [replay-build.txt](replay-build.txt) | Clean patch replay compilation |
| [replay-prepare.txt](replay-prepare.txt) | Archive checksum verification and native patch dry-run/application |

From the APISIX repository root, the recorded C traces can be checked without
starting Docker:

```bash
python3 example/native-response-phase/verify-trace.py \
  example/native-response-phase/evidence-logs/main.txt
python3 example/native-response-phase/verify-trace.py \
  example/native-response-phase/evidence-logs/replay.txt
```

The two expected error lines per runtime log come from the intentional Lua
exception and the upstream-close fixture. The checker rejects other error lines,
duplicate header commits, missing cleanup and worker crashes. Structured summaries
are in the parent directory's trace JSON files; the reproduction guide explains
how to repeat the live HTTP and FD measurements.
