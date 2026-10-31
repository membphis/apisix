# Native response phase feasibility research

**Research archive. Do not merge as a production feature.**

This directory records the background, design, executable prototype and evidence
for a new native response stage before the response header filter chain. It
requires a separately patched and rebuilt OpenResty runtime; stock APISIX cannot
load these C changes directly.

In the tested HTTP/1.1, small-text, native `proxy_pass` case, Lua TCP cosockets and
sleep timers yield and resume in a distinct `response` context. Headers and body
are committed only after the decision. Both the original build and a clean patch
replay passed 33 checks. These results establish bounded feasibility, not general
protocol coverage, production readiness or backward compatibility.

Start with the [research report](../../docs/research/native-response-phase.zh-CN.md)
and [reproduction guide](README.zh-CN.md). The [core patch](native-response.patch)
covers all 25 changed native files; [source examples](source-examples/) expose the
complete newly added C/H files from the tested tree. [Recorded transcripts](evidence-logs/)
and the JSON evidence remain available for inspection without rebuilding.

No production APISIX source, default configuration or CI workflow is changed.
