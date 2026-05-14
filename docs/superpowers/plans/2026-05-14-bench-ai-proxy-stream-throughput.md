# ai-proxy Single-CPU Streaming Throughput Benchmark — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a reproducible benchmark that measures the peak tokens/s an APISIX worker can pass through on a single CPU when running ai-proxy in OpenAI passthrough mode, and produce a CSV with a concurrency sweep.

**Architecture:** Single Go binary (`bench server` and `bench client` subcommands) + a bash harness (`run.sh`) that owns process startup, CPU pinning via `taskset`, ai-proxy route registration via admin API, and orchestration of the concurrency sweep. APISIX runs with `worker_processes=1`, `taskset -c 0`, and minimal plugins.

**Tech Stack:** Go (stdlib + HdrHistogram), bash, APISIX (existing tree), `pidstat`, `taskset`.

**Source spec:** `docs/superpowers/specs/2026-05-14-bench-ai-proxy-stream-throughput-design.md`

---

## File Structure

| Path | Responsibility |
|---|---|
| `benchmark/ai-proxy/bench/go.mod` | Go module declaration |
| `benchmark/ai-proxy/bench/go.sum` | Dependency checksums |
| `benchmark/ai-proxy/bench/main.go` | Subcommand router (`server` vs `client`) |
| `benchmark/ai-proxy/bench/server.go` | Fake OpenAI upstream (HTTP handler + main) |
| `benchmark/ai-proxy/bench/server_test.go` | Handler unit test |
| `benchmark/ai-proxy/bench/client.go` | Load generator (sweep loop, SSE counter, CPU sampling, CSV) |
| `benchmark/ai-proxy/bench/client_test.go` | pidstat parser + SSE counter tests |
| `benchmark/ai-proxy/conf/config.yaml.tpl` | APISIX config template (no variables; plain copy) |
| `benchmark/ai-proxy/run.sh` | Harness: build, pin, register route, run sweep, cleanup |
| `benchmark/ai-proxy/result/` | Output directory (CSV, logs, env.txt) — created at runtime, gitignored |
| `benchmark/ai-proxy/.gitignore` | Ignore `result/` and built `bench` binary |

The Go code stays in a single package `main` because the binary has tightly coupled subcommands. We split files by responsibility (server / client / shared main) — each file ≤ ~250 LoC.

---

## Task 0: Install Go toolchain

**Files:** none (system-level prep)

- [ ] **Step 1: Install Go via apt**

Run:
```bash
sudo apt update && sudo apt install -y golang-go
```

Expected: installs Go 1.22 or later (apt candidate is `2:1.26~1`).

- [ ] **Step 2: Verify Go is on PATH**

Run:
```bash
go version
```
Expected output: `go version go1.XX.Y linux/amd64` (any version ≥ 1.21 works).

- [ ] **Step 3: No commit (system-level change, not in repo)**

---

## Task 1: Bootstrap Go module skeleton

**Files:**
- Create: `benchmark/ai-proxy/bench/go.mod`
- Create: `benchmark/ai-proxy/bench/main.go`
- Create: `benchmark/ai-proxy/.gitignore`

- [ ] **Step 1: Initialize the module**

Run:
```bash
mkdir -p benchmark/ai-proxy/bench
cd benchmark/ai-proxy/bench
go mod init github.com/apache/apisix/benchmark/ai-proxy/bench
cd -
```

Verify `benchmark/ai-proxy/bench/go.mod` exists and starts with `module github.com/apache/apisix/benchmark/ai-proxy/bench`.

- [ ] **Step 2: Write the subcommand router**

Create `benchmark/ai-proxy/bench/main.go`:

```go
// Package main implements a benchmark harness for measuring ai-proxy
// single-CPU streaming throughput. The binary has two subcommands:
//
//	bench server   -- fake OpenAI upstream
//	bench client   -- load generator + CSV reporter
package main

import (
	"fmt"
	"os"
)

func main() {
	if len(os.Args) < 2 {
		usage()
		os.Exit(2)
	}
	switch os.Args[1] {
	case "server":
		runServer(os.Args[2:])
	case "client":
		runClient(os.Args[2:])
	case "-h", "--help":
		usage()
	default:
		fmt.Fprintf(os.Stderr, "unknown subcommand: %s\n", os.Args[1])
		usage()
		os.Exit(2)
	}
}

func usage() {
	fmt.Fprintln(os.Stderr, "usage: bench {server|client} [flags]")
}
```

- [ ] **Step 3: Stub out `runServer` and `runClient` so the build compiles**

Create `benchmark/ai-proxy/bench/server.go`:

```go
package main

func runServer(args []string) {
	panic("not implemented")
}
```

Create `benchmark/ai-proxy/bench/client.go`:

```go
package main

func runClient(args []string) {
	panic("not implemented")
}
```

- [ ] **Step 4: Verify it builds**

Run:
```bash
cd benchmark/ai-proxy/bench && go build -o /tmp/bench-skel . && cd -
ls -l /tmp/bench-skel
```
Expected: binary exists; no compile errors.

- [ ] **Step 5: Create `.gitignore`**

Create `benchmark/ai-proxy/.gitignore`:

```
result/
bench/bench
```

- [ ] **Step 6: Commit**

```bash
git add benchmark/ai-proxy/bench/go.mod benchmark/ai-proxy/bench/main.go \
        benchmark/ai-proxy/bench/server.go benchmark/ai-proxy/bench/client.go \
        benchmark/ai-proxy/.gitignore
git -c commit.gpgsign=false commit -m "feat(benchmark): bootstrap ai-proxy bench Go module skeleton"
```

---

## Task 2: bench server handler — failing test

**Files:**
- Create: `benchmark/ai-proxy/bench/server_test.go`

- [ ] **Step 1: Write the failing test**

Create `benchmark/ai-proxy/bench/server_test.go`:

```go
package main

import (
	"bufio"
	"context"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

// chatHandler should accept POST /v1/chat/completions, set SSE headers,
// and stream pre-encoded OpenAI-shaped chunks until the request context is
// cancelled.
func TestChatHandlerStreamsSSE(t *testing.T) {
	h := newChatHandler(" hello")
	srv := httptest.NewServer(h)
	defer srv.Close()

	ctx, cancel := context.WithTimeout(context.Background(), 500*time.Millisecond)
	defer cancel()

	req, _ := http.NewRequestWithContext(ctx, "POST", srv.URL+"/v1/chat/completions",
		strings.NewReader(`{"stream":true}`))
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatalf("request failed: %v", err)
	}
	defer resp.Body.Close()

	if got := resp.Header.Get("Content-Type"); got != "text/event-stream" {
		t.Fatalf("Content-Type = %q, want text/event-stream", got)
	}

	scanner := bufio.NewScanner(resp.Body)
	dataLines := 0
	for scanner.Scan() && dataLines < 5 {
		if strings.HasPrefix(scanner.Text(), "data: ") {
			line := scanner.Text()
			if !strings.Contains(line, `"delta":{"content":" hello"}`) {
				t.Fatalf("data line missing expected delta content: %q", line)
			}
			dataLines++
		}
	}
	if dataLines < 5 {
		t.Fatalf("expected at least 5 data: lines before context timeout, got %d", dataLines)
	}
}
```

- [ ] **Step 2: Run test to verify it fails**

Run:
```bash
cd benchmark/ai-proxy/bench && go test -run TestChatHandlerStreamsSSE -v
```
Expected: FAIL with `undefined: newChatHandler` (compile error).

---

## Task 3: bench server handler — implementation

**Files:**
- Modify: `benchmark/ai-proxy/bench/server.go`

- [ ] **Step 1: Implement `newChatHandler` + `runServer`**

Replace `benchmark/ai-proxy/bench/server.go` with:

```go
package main

import (
	"flag"
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"os/signal"
	"runtime"
	"syscall"
	"time"
)

// chatChunk is the pre-encoded SSE chunk reused for every event.
// It is built once at server startup and only written (no JSON encoding in
// the hot loop). Shape matches OpenAI chat.completion.chunk.
func buildChunk(tokenText string) []byte {
	// JSON-safe: tokenText is interpolated literally. We control input via
	// flag, so we don't escape — but reject newlines/quotes to keep it valid.
	for _, c := range tokenText {
		if c == '"' || c == '\\' || c == '\n' || c == '\r' {
			log.Fatalf("token-text must not contain quotes, backslashes, or newlines")
		}
	}
	body := fmt.Sprintf(
		`{"id":"chatcmpl-bench","object":"chat.completion.chunk","created":1715000000,"model":"gpt-4o","choices":[{"index":0,"delta":{"content":"%s"},"finish_reason":null}]}`,
		tokenText)
	return []byte("data: " + body + "\n\n")
}

func newChatHandler(tokenText string) http.Handler {
	chunk := buildChunk(tokenText)
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
			return
		}
		// Drain request body so the client doesn't stall on unread data.
		_, _ = io.Copy(io.Discard, r.Body)

		w.Header().Set("Content-Type", "text/event-stream")
		w.Header().Set("Cache-Control", "no-cache")
		w.Header().Set("X-Accel-Buffering", "no")
		w.Header().Set("Connection", "keep-alive")
		w.WriteHeader(http.StatusOK)

		flusher, ok := w.(http.Flusher)
		if !ok {
			http.Error(w, "streaming not supported", http.StatusInternalServerError)
			return
		}

		ctx := r.Context()
		for {
			if _, err := w.Write(chunk); err != nil {
				return
			}
			flusher.Flush()
			select {
			case <-ctx.Done():
				return
			default:
			}
		}
	})
}

func runServer(args []string) {
	fs := flag.NewFlagSet("server", flag.ExitOnError)
	listen := fs.String("listen", ":1981", "listen address")
	tokenText := fs.String("token-text", " hello", "literal delta.content per chunk")
	gomaxprocs := fs.Int("gomaxprocs", 2, "GOMAXPROCS for this process")
	_ = fs.Parse(args)

	runtime.GOMAXPROCS(*gomaxprocs)
	fmt.Printf("bench-server: pid=%d listen=%s gomaxprocs=%d\n",
		os.Getpid(), *listen, *gomaxprocs)

	mux := http.NewServeMux()
	mux.Handle("/v1/chat/completions", newChatHandler(*tokenText))

	srv := &http.Server{
		Addr:              *listen,
		Handler:           mux,
		ReadHeaderTimeout: 5 * time.Second,
	}

	stop := make(chan os.Signal, 1)
	signal.Notify(stop, syscall.SIGINT, syscall.SIGTERM)
	go func() {
		<-stop
		srv.Close()
	}()

	if err := srv.ListenAndServe(); err != nil && err != http.ErrServerClosed {
		log.Fatalf("server: %v", err)
	}
}
```

- [ ] **Step 2: Run test to verify it passes**

Run:
```bash
cd benchmark/ai-proxy/bench && go test -run TestChatHandlerStreamsSSE -v
```
Expected: PASS.

- [ ] **Step 3: Smoke test the binary end-to-end**

Run in one terminal:
```bash
cd benchmark/ai-proxy/bench && go run . server --listen :1981
```

In another terminal:
```bash
curl -N -s -X POST -H 'Content-Type: application/json' \
  http://127.0.0.1:1981/v1/chat/completions \
  -d '{"stream":true}' | head -3
```
Expected: 3 lines starting with `data: {...}`. Kill the server with Ctrl+C.

- [ ] **Step 4: Commit**

```bash
git add benchmark/ai-proxy/bench/server.go benchmark/ai-proxy/bench/server_test.go
git -c commit.gpgsign=false commit -m "feat(benchmark): bench server streams pre-encoded OpenAI SSE chunks"
```

---

## Task 4: pidstat output parser — test + implementation

**Files:**
- Modify: `benchmark/ai-proxy/bench/client.go`
- Create: `benchmark/ai-proxy/bench/client_test.go`

The parser is a pure function with no side effects — ideal place for a unit test that pins the parser against representative output.

- [ ] **Step 1: Write the failing test**

Create `benchmark/ai-proxy/bench/client_test.go`:

```go
package main

import (
	"math"
	"strings"
	"testing"
)

// Sample matches the `pidstat -p <pid> 1 N -u` output format on Linux
// (procps-ng 3.3+). Locale-neutral; we set LC_ALL=C when invoking.
const pidstatSample = `Linux 5.15.0-86-generic (host)  05/14/2026  _x86_64_  (8 CPU)

10:00:01      UID       PID    %usr %system  %guest   %wait    %CPU   CPU  Command
10:00:02     1000     12345    0.00    0.00    0.00    0.00    0.00     0  nginx
10:00:03     1000     12345   45.00   10.00    0.00    0.00   55.00     0  nginx
10:00:04     1000     12345   50.00   15.00    0.00    0.00   65.00     0  nginx
10:00:05     1000     12345   60.00   20.00    0.00    0.00   80.00     0  nginx
Average:     1000     12345   38.75   11.25    0.00    0.00   50.00     -  nginx
`

func TestParsePidstatCPU(t *testing.T) {
	mean, samples, err := parsePidstatCPU(strings.NewReader(pidstatSample))
	if err != nil {
		t.Fatalf("parsePidstatCPU: %v", err)
	}
	if samples != 3 {
		t.Fatalf("samples = %d, want 3 (first sample discarded; Average line ignored)", samples)
	}
	want := (55.0 + 65.0 + 80.0) / 3.0
	if math.Abs(mean-want) > 0.01 {
		t.Fatalf("mean = %v, want %v", mean, want)
	}
}

func TestParsePidstatCPUNoSamples(t *testing.T) {
	_, _, err := parsePidstatCPU(strings.NewReader("Linux header only\n\n"))
	if err == nil {
		t.Fatalf("expected error for empty pidstat output, got nil")
	}
}
```

- [ ] **Step 2: Run test to verify it fails**

Run:
```bash
cd benchmark/ai-proxy/bench && go test -run TestParsePidstat -v
```
Expected: FAIL with `undefined: parsePidstatCPU`.

- [ ] **Step 3: Implement `parsePidstatCPU` in `client.go`**

Replace `benchmark/ai-proxy/bench/client.go` with:

```go
package main

import (
	"bufio"
	"fmt"
	"io"
	"strconv"
	"strings"
)

// parsePidstatCPU reads `pidstat -p <pid> 1 N -u` output and returns the
// mean %CPU across samples, dropping the first sample (often a low outlier)
// and any "Average:" trailer line.
//
// pidstat output rows look like:
//
//	HH:MM:SS  UID  PID  %usr  %system  %guest  %wait  %CPU  CPU  Command
//
// We locate the %CPU column from the header line, then parse subsequent
// rows that start with a time-of-day token (HH:MM:SS).
func parsePidstatCPU(r io.Reader) (mean float64, samples int, err error) {
	sc := bufio.NewScanner(r)
	cpuCol := -1
	var values []float64

	for sc.Scan() {
		line := strings.TrimSpace(sc.Text())
		if line == "" {
			continue
		}
		fields := strings.Fields(line)
		if cpuCol < 0 {
			// Header: detect by presence of "%CPU"
			for i, f := range fields {
				if f == "%CPU" {
					cpuCol = i
					break
				}
			}
			continue
		}
		// Ignore "Average:" trailer
		if strings.HasPrefix(fields[0], "Average") {
			continue
		}
		// Must look like a time-of-day row: HH:MM:SS
		if !isTimeOfDay(fields[0]) {
			continue
		}
		if cpuCol >= len(fields) {
			continue
		}
		v, perr := strconv.ParseFloat(fields[cpuCol], 64)
		if perr != nil {
			continue
		}
		values = append(values, v)
	}
	if err := sc.Err(); err != nil {
		return 0, 0, err
	}
	if len(values) < 2 {
		return 0, 0, fmt.Errorf("pidstat: not enough samples (got %d)", len(values))
	}
	// Drop the first sample (pidstat's first interval can be a cold-start outlier).
	values = values[1:]
	sum := 0.0
	for _, v := range values {
		sum += v
	}
	return sum / float64(len(values)), len(values), nil
}

func isTimeOfDay(s string) bool {
	// Matches HH:MM:SS with optional AM/PM suffix not present in -u output.
	if len(s) != 8 {
		return false
	}
	if s[2] != ':' || s[5] != ':' {
		return false
	}
	for _, idx := range []int{0, 1, 3, 4, 6, 7} {
		if s[idx] < '0' || s[idx] > '9' {
			return false
		}
	}
	return true
}

func runClient(args []string) {
	panic("not implemented")
}
```

- [ ] **Step 4: Run test to verify it passes**

Run:
```bash
cd benchmark/ai-proxy/bench && go test -run TestParsePidstat -v
```
Expected: PASS (both subtests).

- [ ] **Step 5: Commit**

```bash
git add benchmark/ai-proxy/bench/client.go benchmark/ai-proxy/bench/client_test.go
git -c commit.gpgsign=false commit -m "feat(benchmark): bench client pidstat %CPU output parser"
```

---

## Task 5: SSE event counter — test + implementation

**Files:**
- Modify: `benchmark/ai-proxy/bench/client.go`
- Modify: `benchmark/ai-proxy/bench/client_test.go`

- [ ] **Step 1: Add failing test for `countSSEEvents`**

Add `"bytes"` to the existing `import (...)` block at the top of
`benchmark/ai-proxy/bench/client_test.go`. Then append:

```go
func TestCountSSEEvents(t *testing.T) {
	in := bytes.NewBufferString(strings.Join([]string{
		`data: {"id":"a"}`,
		``,
		`data: {"id":"b"}`,
		``,
		`data: [DONE]`,
		``,
		`: keepalive`,
		``,
	}, "\n"))
	count, err := countSSEEvents(in, func() {})
	if err != nil {
		t.Fatalf("countSSEEvents: %v", err)
	}
	if count != 2 {
		t.Fatalf("count = %d, want 2 (DONE and comments excluded)", count)
	}
}
```

- [ ] **Step 2: Run to confirm failure**

Run:
```bash
cd benchmark/ai-proxy/bench && go test -run TestCountSSEEvents -v
```
Expected: FAIL with `undefined: countSSEEvents`.

- [ ] **Step 3: Implement `countSSEEvents`**

Insert into `benchmark/ai-proxy/bench/client.go` (above `runClient`):

```go
// countSSEEvents reads an SSE stream line-by-line and returns the number of
// `data: …` events received, excluding `data: [DONE]` sentinel. onEvent is
// called once per counted event (used to record inter-event latency); pass
// a no-op when latency tracking is not needed.
func countSSEEvents(r io.Reader, onEvent func()) (int, error) {
	sc := bufio.NewScanner(r)
	// SSE lines can theoretically be long; bump buffer.
	sc.Buffer(make([]byte, 0, 8*1024), 1<<20)
	n := 0
	for sc.Scan() {
		line := sc.Text()
		if !strings.HasPrefix(line, "data: ") {
			continue
		}
		if line == "data: [DONE]" {
			continue
		}
		n++
		onEvent()
	}
	if err := sc.Err(); err != nil {
		return n, err
	}
	return n, nil
}
```

`io` already comes in via the existing `client.go` import (`io.Reader`); no
import edits needed for this step.

- [ ] **Step 4: Run to confirm pass**

Run:
```bash
cd benchmark/ai-proxy/bench && go test -run TestCountSSEEvents -v
```
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add benchmark/ai-proxy/bench/client.go benchmark/ai-proxy/bench/client_test.go
git -c commit.gpgsign=false commit -m "feat(benchmark): bench client SSE event counter"
```

---

## Task 6: HdrHistogram dependency + percentile helper

**Files:**
- Modify: `benchmark/ai-proxy/bench/go.mod`
- Modify: `benchmark/ai-proxy/bench/client.go`

- [ ] **Step 1: Add HdrHistogram dependency**

Run:
```bash
cd benchmark/ai-proxy/bench && go get github.com/HdrHistogram/hdrhistogram-go && cd -
```

Verify `go.mod` now contains `github.com/HdrHistogram/hdrhistogram-go v...`.

- [ ] **Step 2: Update the existing `import (...)` block in `client.go`**

Add this line inside the existing `import (...)` block at the top of
`benchmark/ai-proxy/bench/client.go`:

```go
hdrhist "github.com/HdrHistogram/hdrhistogram-go"
```

- [ ] **Step 3: Add latency-histogram helper**

Append to `benchmark/ai-proxy/bench/client.go` (above `runClient`):

```go
// newLatencyHist returns a histogram for inter-event latency between 1ns and
// 1s with 3 significant figures of precision (values are nanoseconds).
func newLatencyHist() *hdrhist.Histogram {
	return hdrhist.New(1, 1_000_000_000, 3)
}
```

No new test — this is a trivial wrapper around a library and is exercised
through the integration smoke test in Task 8.

- [ ] **Step 4: Verify the package still builds**

Run:
```bash
cd benchmark/ai-proxy/bench && go build ./...
```
Expected: clean build, exit 0.

- [ ] **Step 5: Commit**

```bash
git add benchmark/ai-proxy/bench/go.mod benchmark/ai-proxy/bench/go.sum benchmark/ai-proxy/bench/client.go
git -c commit.gpgsign=false commit -m "feat(benchmark): add HdrHistogram for inter-event latency"
```

---

## Task 7: Concurrency sweep loop + CSV writer

**Files:**
- Modify: `benchmark/ai-proxy/bench/client.go`

This task wires together the SSE counter, latency histogram, pidstat sampling, and CSV output. The flow is:

```
for each concurrency level c in CONCURRENCY:
    spawn c goroutines, each opening one POST stream to APISIX
    spawn pidstat -p APISIX_PID 1 duration to STDOUT (parsed via parsePidstatCPU)
    spawn pidstat -p SERVER_PID 1 duration -> server_cpu
    spawn pidstat -p OWN_PID  1 duration -> client_cpu
    sleep warmup
    reset per-goroutine counters and histograms; start measurement
    sleep duration
    cancel goroutines, wait for them to drain
    sum tokens, merge histograms, parse pidstats
    append one CSV row
    sleep cool
```

- [ ] **Step 1: Update the `import (...)` block in `client.go`**

Replace the existing `import (...)` block at the top of
`benchmark/ai-proxy/bench/client.go` with the full final set of imports:

```go
import (
	"bufio"
	"bytes"
	"context"
	"encoding/csv"
	"flag"
	"fmt"
	"io"
	"net/http"
	"os"
	"os/exec"
	"runtime"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	hdrhist "github.com/HdrHistogram/hdrhistogram-go"
)
```

- [ ] **Step 2: Replace `runClient` and add the supporting types/functions**

Replace the stubbed `runClient` in `benchmark/ai-proxy/bench/client.go` with the
following block (keeping the existing `parsePidstatCPU`, `isTimeOfDay`,
`countSSEEvents`, and `newLatencyHist` definitions in the file):

```go
type sweepRow struct {
	concurrency     int
	durationS       int
	totalTokens     int64
	totalTPS        float64
	apisixCPU       float64
	serverCPU       float64
	clientCPU       float64
	p50InterEventUs int64
	p99InterEventUs int64
}

func runClient(args []string) {
	fs := flag.NewFlagSet("client", flag.ExitOnError)
	apisixURL := fs.String("apisix-url", "http://127.0.0.1:9080/v1/chat/completions", "ai-proxy route URL")
	apisixPID := fs.Int("apisix-pid", 0, "APISIX worker PID (required)")
	serverPID := fs.Int("server-pid", 0, "bench server PID (required)")
	concStr := fs.String("concurrency", "1,2,4,8,16,32,64", "comma-separated sweep")
	warmup := fs.Duration("warmup", 5*time.Second, "warmup per level")
	dur := fs.Duration("duration", 60*time.Second, "measurement window per level")
	cool := fs.Duration("cool", 5*time.Second, "idle between levels")
	gomaxprocs := fs.Int("gomaxprocs", 5, "GOMAXPROCS for this process")
	outPath := fs.String("out", "bench-result.csv", "CSV output path")
	_ = fs.Parse(args)

	if *apisixPID == 0 || *serverPID == 0 {
		fmt.Fprintln(os.Stderr, "--apisix-pid and --server-pid are required")
		os.Exit(2)
	}
	runtime.GOMAXPROCS(*gomaxprocs)

	levels, err := parseConcurrency(*concStr)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(2)
	}

	f, err := os.Create(*outPath)
	if err != nil {
		fmt.Fprintf(os.Stderr, "open csv: %v\n", err)
		os.Exit(1)
	}
	defer f.Close()
	cw := csv.NewWriter(f)
	_ = cw.Write([]string{
		"concurrency", "duration_s", "total_tokens", "total_t_per_s",
		"apisix_cpu_pct", "server_cpu_pct", "client_cpu_pct",
		"p50_inter_event_us", "p99_inter_event_us",
	})
	cw.Flush()

	for _, c := range levels {
		row := runLevel(*apisixURL, c, *apisixPID, *serverPID, *warmup, *dur)
		_ = cw.Write([]string{
			strconv.Itoa(row.concurrency),
			strconv.Itoa(row.durationS),
			strconv.FormatInt(row.totalTokens, 10),
			strconv.FormatFloat(row.totalTPS, 'f', 1, 64),
			strconv.FormatFloat(row.apisixCPU, 'f', 1, 64),
			strconv.FormatFloat(row.serverCPU, 'f', 1, 64),
			strconv.FormatFloat(row.clientCPU, 'f', 1, 64),
			strconv.FormatInt(row.p50InterEventUs, 10),
			strconv.FormatInt(row.p99InterEventUs, 10),
		})
		cw.Flush()
		fmt.Printf("level c=%d: tokens=%d tps=%.1f apisix_cpu=%.1f%% p99=%dµs\n",
			row.concurrency, row.totalTokens, row.totalTPS, row.apisixCPU, row.p99InterEventUs)
		time.Sleep(*cool)
	}
}

func parseConcurrency(s string) ([]int, error) {
	parts := strings.Split(s, ",")
	out := make([]int, 0, len(parts))
	for _, p := range parts {
		n, err := strconv.Atoi(strings.TrimSpace(p))
		if err != nil || n <= 0 {
			return nil, fmt.Errorf("invalid concurrency %q", p)
		}
		out = append(out, n)
	}
	return out, nil
}

// local holds per-goroutine measurement state. Each streaming worker owns
// one of these to avoid lock contention; the runLevel coordinator merges
// them at the end of the measurement window.
type local struct {
	tokens int64
	hist   *hdrhist.Histogram
}

func runLevel(url string, c, apisixPID, serverPID int, warmup, dur time.Duration) sweepRow {
	// Build request body once; each goroutine reuses it.
	body := `{"model":"gpt-4o","messages":[{"role":"user","content":"hi"}],"stream":true}`

	locals := make([]*local, c)
	for i := range locals {
		locals[i] = &local{hist: newLatencyHist()}
	}

	// Phase 1: open all connections; let them run during warmup with counts
	// going into a discardable counter via atomic flag.
	measuring := int64(0)
	ctxAll, cancelAll := context.WithCancel(context.Background())
	defer cancelAll()

	var wg sync.WaitGroup
	for i := 0; i < c; i++ {
		wg.Add(1)
		go func(idx int) {
			defer wg.Done()
			streamWorker(ctxAll, url, body, locals[idx], &measuring)
		}(i)
	}

	// Phase 2: warmup
	time.Sleep(warmup)
	atomic.StoreInt64(&measuring, 1)

	// Phase 3: spawn pidstat probes concurrently with the measurement
	intervals := int(dur.Seconds())
	if intervals < 2 {
		intervals = 2 // pidstat parser needs ≥ 2 samples
	}
	apisixCh := make(chan float64, 1)
	serverCh := make(chan float64, 1)
	clientCh := make(chan float64, 1)
	go func() { apisixCh <- pidstatCPU(apisixPID, intervals) }()
	go func() { serverCh <- pidstatCPU(serverPID, intervals) }()
	go func() { clientCh <- pidstatCPU(os.Getpid(), intervals) }()

	// Phase 4: measure for `dur`
	time.Sleep(dur)
	atomic.StoreInt64(&measuring, 0)

	// Phase 5: stop streams
	cancelAll()
	wg.Wait()

	// Phase 6: aggregate
	var totalTokens int64
	merged := newLatencyHist()
	for _, l := range locals {
		totalTokens += atomic.LoadInt64(&l.tokens)
		merged.Merge(l.hist)
	}

	apisix := <-apisixCh
	server := <-serverCh
	client := <-clientCh

	return sweepRow{
		concurrency:     c,
		durationS:       int(dur.Seconds()),
		totalTokens:     totalTokens,
		totalTPS:        float64(totalTokens) / dur.Seconds(),
		apisixCPU:       apisix,
		serverCPU:       server,
		clientCPU:       client,
		p50InterEventUs: merged.ValueAtQuantile(50) / 1000,
		p99InterEventUs: merged.ValueAtQuantile(99) / 1000,
	}
}

func streamWorker(ctx context.Context, url, body string, l *local, measuring *int64) {
	for ctx.Err() == nil {
		req, err := http.NewRequestWithContext(ctx, "POST", url, strings.NewReader(body))
		if err != nil {
			return
		}
		req.Header.Set("Content-Type", "application/json")
		req.Header.Set("Accept", "text/event-stream")

		resp, err := http.DefaultClient.Do(req)
		if err != nil {
			if ctx.Err() != nil {
				return
			}
			time.Sleep(50 * time.Millisecond)
			continue
		}
		readSSEStream(resp.Body, l, measuring)
		resp.Body.Close()
	}
}

func readSSEStream(r io.Reader, l *local, measuring *int64) {
	var last time.Time
	_, _ = countSSEEvents(r, func() {
		if atomic.LoadInt64(measuring) != 1 {
			return
		}
		atomic.AddInt64(&l.tokens, 1)
		now := time.Now()
		if !last.IsZero() {
			_ = l.hist.RecordValue(now.Sub(last).Nanoseconds())
		}
		last = now
	})
}

func pidstatCPU(pid, seconds int) float64 {
	cmd := exec.Command("pidstat", "-p", strconv.Itoa(pid), "1", strconv.Itoa(seconds), "-u")
	cmd.Env = append(os.Environ(), "LC_ALL=C")
	out, err := cmd.Output()
	if err != nil {
		fmt.Fprintf(os.Stderr, "pidstat pid=%d: %v\n", pid, err)
		return -1
	}
	mean, samples, err := parsePidstatCPU(bytes.NewReader(out))
	if err != nil {
		fmt.Fprintf(os.Stderr, "pidstat parse pid=%d: %v\n", pid, err)
		return -1
	}
	if samples < 2 {
		fmt.Fprintf(os.Stderr, "pidstat pid=%d: only %d samples\n", pid, samples)
	}
	return mean
}
```

- [ ] **Step 3: Verify the package builds**

Run:
```bash
cd benchmark/ai-proxy/bench && go build ./...
```
Expected: no errors. If there are import collisions, fix in place.

- [ ] **Step 4: Run all existing tests to make sure nothing regressed**

Run:
```bash
cd benchmark/ai-proxy/bench && go test ./... -v
```
Expected: all tests pass (TestChatHandlerStreamsSSE, TestParsePidstatCPU, TestParsePidstatCPUNoSamples, TestCountSSEEvents).

- [ ] **Step 5: Commit**

```bash
git add benchmark/ai-proxy/bench/client.go
git -c commit.gpgsign=false commit -m "feat(benchmark): bench client concurrency sweep + CSV output"
```

---

## Task 8: Client–server integration smoke test

**Files:**
- Modify: `benchmark/ai-proxy/bench/client_test.go`

This validates the full client→handler loop without needing APISIX or pidstat.

- [ ] **Step 1: Update the `import (...)` block in `client_test.go`**

Add these entries to the existing `import (...)` block at the top of
`benchmark/ai-proxy/bench/client_test.go`:

```
context
net/http/httptest
sync
sync/atomic
time
```

- [ ] **Step 2: Add the integration test**

Append to `benchmark/ai-proxy/bench/client_test.go`:

```go
func TestStreamWorkerCountsAgainstFakeServer(t *testing.T) {
	srv := httptest.NewServer(newChatHandler(" hello"))
	defer srv.Close()

	ctx, cancel := context.WithTimeout(context.Background(), 600*time.Millisecond)
	defer cancel()

	l := &local{hist: newLatencyHist()}
	measuring := int64(1)
	var wg sync.WaitGroup
	wg.Add(1)
	go func() {
		defer wg.Done()
		streamWorker(ctx, srv.URL+"/v1/chat/completions",
			`{"stream":true}`, l, &measuring)
	}()
	wg.Wait()

	got := atomic.LoadInt64(&l.tokens)
	if got < 100 {
		t.Fatalf("expected ≥ 100 events in 600ms, got %d", got)
	}
}
```

- [ ] **Step 3: Run the test**

Run:
```bash
cd benchmark/ai-proxy/bench && go test -run TestStreamWorker -v
```
Expected: PASS, with the event count well into the thousands.

- [ ] **Step 4: Commit**

```bash
git add benchmark/ai-proxy/bench/client_test.go
git -c commit.gpgsign=false commit -m "test(benchmark): bench client/server integration smoke test"
```

---

## Task 9: APISIX config template

**Files:**
- Create: `benchmark/ai-proxy/conf/config.yaml.tpl`

- [ ] **Step 1: Write the template**

Create `benchmark/ai-proxy/conf/config.yaml.tpl`:

```yaml
# APISIX config for ai-proxy single-CPU streaming benchmark.
# Run-time copy of this file is placed at conf/config.yaml by run.sh.
# Hand-edit this file (not the copy) and rerun run.sh.

deployment:
  role: traditional
  role_traditional:
    config_provider: yaml
  admin:
    admin_key:
      - name: admin
        key: edd1c9f034335f136f87ad84b625c8f1
        role: admin

nginx_config:
  worker_processes: 1
  error_log_level: warn
  http:
    access_log: 'off'

plugins:
  - ai-proxy
```

(The admin key is the APISIX default for the local-yaml deployment, used here only because run.sh needs an admin key to register the route over the admin API.)

- [ ] **Step 2: Lint the YAML**

Run:
```bash
python3 -c 'import yaml,sys; yaml.safe_load(open("benchmark/ai-proxy/conf/config.yaml.tpl"))' \
  && echo OK
```
Expected: `OK`.

- [ ] **Step 3: Commit**

```bash
git add benchmark/ai-proxy/conf/config.yaml.tpl
git -c commit.gpgsign=false commit -m "feat(benchmark): APISIX config template for ai-proxy bench"
```

---

## Task 10: Harness script — skeleton + cleanup

**Files:**
- Create: `benchmark/ai-proxy/run.sh`

- [ ] **Step 1: Write the skeleton (no actual measurement yet)**

Create `benchmark/ai-proxy/run.sh`:

```bash
#!/usr/bin/env bash
#
# ai-proxy single-CPU streaming throughput benchmark harness.
# See docs/superpowers/specs/2026-05-14-bench-ai-proxy-stream-throughput-design.md
#
set -euo pipefail

# Repo root (the harness is meant to be run from anywhere).
ROOT="$(git rev-parse --show-toplevel)"
cd "$ROOT"

DURATION="${DURATION:-60}"
WARMUP="${WARMUP:-5}"
CONCURRENCY="${CONCURRENCY:-1,2,4,8,16,32,64}"
APISIX_CORE="${APISIX_CORE:-0}"
SERVER_CORES="${SERVER_CORES:-1-2}"
CLIENT_CORES="${CLIENT_CORES:-3-7}"

BENCH_DIR="benchmark/ai-proxy"
RESULT_DIR="$BENCH_DIR/result"
mkdir -p "$RESULT_DIR"

BENCH_BIN="$RESULT_DIR/bench"
CSV="$RESULT_DIR/bench-result.csv"
SERVER_LOG="$RESULT_DIR/server.log"
ENV_FILE="$RESULT_DIR/env.txt"
ERR_COPY="$RESULT_DIR/apisix-error.log"

SERVER_PID=""
WORKER_PID=""

cleanup() {
  set +e
  if [ -n "${WORKER_PID:-}" ]; then
    : # nothing to do for the worker beyond `make stop`
  fi
  if [ -n "${SERVER_PID:-}" ]; then
    kill "$SERVER_PID" 2>/dev/null
  fi
  make stop 2>/dev/null
  sudo killall pidstat 2>/dev/null
  # Snapshot APISIX error log if present.
  if [ -f logs/error.log ]; then
    cp logs/error.log "$ERR_COPY" 2>/dev/null || true
  fi
}
trap cleanup INT TERM EXIT

# Cache sudo creds so `taskset -cp` doesn't prompt mid-run.
sudo -v
```

Make it executable:
```bash
chmod +x benchmark/ai-proxy/run.sh
```

- [ ] **Step 2: Sanity check (script parses, env loads)**

Run:
```bash
bash -n benchmark/ai-proxy/run.sh && echo "syntax OK"
```
Expected: `syntax OK`.

- [ ] **Step 3: Commit**

```bash
git add benchmark/ai-proxy/run.sh
git -c commit.gpgsign=false commit -m "feat(benchmark): harness skeleton with env vars and cleanup trap"
```

---

## Task 11: Harness — build + start fake server

**Files:**
- Modify: `benchmark/ai-proxy/run.sh`

- [ ] **Step 1: Append build + server startup**

Append to `benchmark/ai-proxy/run.sh` (after `sudo -v`):

```bash

# --- Step: build bench binary -----------------------------------------------
echo "==> building bench binary"
(cd "$BENCH_DIR/bench" && go build -o "$ROOT/$BENCH_BIN" .)

# --- Step: record environment -----------------------------------------------
{
  echo "date: $(date -Is)"
  echo "git_sha: $(git rev-parse HEAD)"
  echo "kernel: $(uname -r)"
  echo "cpu_model: $(awk -F: '/model name/{print $2; exit}' /proc/cpuinfo | sed 's/^ //')"
  echo "go: $(go version)"
  echo "concurrency: $CONCURRENCY"
  echo "warmup_s: $WARMUP"
  echo "duration_s: $DURATION"
  echo "apisix_core: $APISIX_CORE"
  echo "server_cores: $SERVER_CORES"
  echo "client_cores: $CLIENT_CORES"
} > "$ENV_FILE"

# --- Step: start fake OpenAI upstream ---------------------------------------
echo "==> starting bench server on cores $SERVER_CORES"
taskset -c "$SERVER_CORES" "$BENCH_BIN" server --listen :1981 --gomaxprocs 2 \
  > "$SERVER_LOG" 2>&1 &
SERVER_PID=$!
# Wait until the server prints its startup banner (means the listener is up).
for _ in $(seq 1 30); do
  if grep -q '^bench-server: pid=' "$SERVER_LOG"; then break; fi
  sleep 0.1
done
if ! grep -q '^bench-server: pid=' "$SERVER_LOG"; then
  echo "bench server failed to start; see $SERVER_LOG" >&2
  exit 1
fi
echo "    bench server pid=$SERVER_PID"
```

- [ ] **Step 2: Smoke-run just the prefix**

Run:
```bash
( bash benchmark/ai-proxy/run.sh & ) ; sleep 5
curl -N -s -X POST -H 'Content-Type: application/json' \
  http://127.0.0.1:1981/v1/chat/completions -d '{"stream":true}' | head -2
sleep 1
kill %1 2>/dev/null || true
# Make sure leftover processes are gone.
pkill -f 'bench server' 2>/dev/null || true
```

Note: the harness will fail downstream because the rest isn't written yet — that's expected. We only care that `bench-server` shows up on :1981 and serves `data:` lines.

- [ ] **Step 3: Commit**

```bash
git add benchmark/ai-proxy/run.sh
git -c commit.gpgsign=false commit -m "feat(benchmark): harness builds binary and starts fake upstream"
```

---

## Task 12: Harness — start + pin APISIX, register route

**Files:**
- Modify: `benchmark/ai-proxy/run.sh`

- [ ] **Step 1: Append APISIX startup + route**

Append to `benchmark/ai-proxy/run.sh`:

```bash

# --- Step: write APISIX config ----------------------------------------------
echo "==> installing APISIX config"
cp "$BENCH_DIR/conf/config.yaml.tpl" conf/config.yaml

# --- Step: start APISIX ------------------------------------------------------
echo "==> starting APISIX"
make init
make run

# --- Step: locate the worker and pin it to APISIX_CORE ----------------------
echo "==> waiting for APISIX worker to come up"
for _ in $(seq 1 30); do
  WORKER_PID="$(pgrep -f 'nginx: worker process' | head -1 || true)"
  [ -n "$WORKER_PID" ] && break
  sleep 1
done
if [ -z "$WORKER_PID" ]; then
  echo "APISIX worker did not start within 30s" >&2
  exit 1
fi
echo "    APISIX worker pid=$WORKER_PID"
sudo taskset -cp "$APISIX_CORE" "$WORKER_PID"
taskset -cp "$WORKER_PID"

# --- Step: register the ai-proxy route --------------------------------------
echo "==> registering ai-proxy route"
ADMIN_KEY="edd1c9f034335f136f87ad84b625c8f1"
curl -sS -o /dev/null -w "    admin route response: %{http_code}\n" \
  -H "X-API-KEY: $ADMIN_KEY" -X PUT \
  http://127.0.0.1:9180/apisix/admin/routes/1 \
  -d '{
    "uri": "/v1/chat/completions",
    "plugins": {
      "ai-proxy": {
        "provider": "openai",
        "auth": { "header": { "Authorization": "Bearer sk-bench" } },
        "override": { "endpoint": "http://127.0.0.1:1981/v1/chat/completions" }
      }
    }
  }'

# Wait a beat for the route to be picked up.
sleep 1
```

- [ ] **Step 2: Run the script and verify the route works end-to-end**

Run:
```bash
DURATION=2 WARMUP=1 CONCURRENCY=1 bash benchmark/ai-proxy/run.sh &
BENCH_PID=$!
sleep 10
# probe through APISIX (port 9080) — should return SSE proxied from bench server
curl -N -s -X POST -H 'Content-Type: application/json' \
  http://127.0.0.1:9080/v1/chat/completions \
  -d '{"model":"gpt-4o","messages":[{"role":"user","content":"hi"}],"stream":true}' \
  | head -2
# Let the harness's trap clean up on its own; just wait briefly then stop.
wait $BENCH_PID 2>/dev/null || true
```

The harness will still exit non-zero because the client step doesn't exist yet — that's fine. Expected output from curl: two `data: …` SSE lines.

If `curl` does not return `data:` lines, inspect `logs/error.log` and
`benchmark/ai-proxy/result/server.log`.

- [ ] **Step 3: Commit**

```bash
git add benchmark/ai-proxy/run.sh
git -c commit.gpgsign=false commit -m "feat(benchmark): harness pins APISIX worker and registers ai-proxy route"
```

---

## Task 13: Harness — run client sweep + sanity check

**Files:**
- Modify: `benchmark/ai-proxy/run.sh`

- [ ] **Step 1: Append client invocation + sanity check**

Append to `benchmark/ai-proxy/run.sh`:

```bash

# --- Step: run the client concurrency sweep ---------------------------------
echo "==> starting client sweep (concurrency=$CONCURRENCY)"
taskset -c "$CLIENT_CORES" "$BENCH_BIN" client \
  --apisix-url http://127.0.0.1:9080/v1/chat/completions \
  --apisix-pid "$WORKER_PID" \
  --server-pid "$SERVER_PID" \
  --concurrency "$CONCURRENCY" \
  --warmup "${WARMUP}s" \
  --duration "${DURATION}s" \
  --cool 5s \
  --gomaxprocs 5 \
  --out "$CSV"

# --- Step: sanity check the CSV ---------------------------------------------
echo "==> sanity checks"
python3 - "$CSV" <<'PY' || { echo "    sanity check failed"; exit 1; }
import csv, sys
rows = list(csv.DictReader(open(sys.argv[1])))
if not rows:
    print("no rows in CSV", file=sys.stderr); sys.exit(1)

# 1. peak row apisix_cpu_pct >= 90
peak = max(rows, key=lambda r: float(r["total_t_per_s"]))
cpu = float(peak["apisix_cpu_pct"])
print(f"peak: concurrency={peak['concurrency']} t/s={peak['total_t_per_s']} cpu={cpu}")
if cpu < 90:
    print(f"    WARN: APISIX CPU at peak is {cpu}%, not saturated", file=sys.stderr)

# 2. server/client cpu < 60 across rows
for r in rows:
    s, c = float(r["server_cpu_pct"]), float(r["client_cpu_pct"])
    if s >= 60 or c >= 60:
        print(f"    WARN: load-side near saturation: server={s}% client={c}%", file=sys.stderr)

# 3. throughput non-strictly monotonic up to peak
tps = [float(r["total_t_per_s"]) for r in rows]
peak_idx = tps.index(max(tps))
prev = 0.0
for i, v in enumerate(tps[:peak_idx+1]):
    if v + 1e-6 < prev:
        print(f"    WARN: t/s dropped before peak at row {i}", file=sys.stderr)
        break
    prev = v
print("    sanity checks complete")
PY

echo "==> done. CSV: $CSV"
```

- [ ] **Step 2: Run a tiny full-pipeline smoke test**

Run:
```bash
DURATION=5 WARMUP=2 CONCURRENCY=1,2 bash benchmark/ai-proxy/run.sh
```
Expected:
- Script completes with exit code 0
- `benchmark/ai-proxy/result/bench-result.csv` has 1 header + 2 data rows
- Each row has plausible numbers (tokens > 1000, apisix_cpu_pct between 0 and 100)
- "sanity checks complete" printed

If `apisix_cpu_pct` shows -1, `pidstat` failed for the worker — check that the worker PID is correct and that `pidstat` is on PATH.

- [ ] **Step 3: Commit**

```bash
git add benchmark/ai-proxy/run.sh
git -c commit.gpgsign=false commit -m "feat(benchmark): harness runs sweep and validates CSV sanity"
```

---

## Task 14: Real bench run — collect numbers

**Files:**
- Update: spec doc with measured peak

- [ ] **Step 1: Run the default sweep (≈ 8 min for 60s × 7 levels + cool)**

Run:
```bash
bash benchmark/ai-proxy/run.sh 2>&1 | tee benchmark/ai-proxy/result/run.log
```

- [ ] **Step 2: Inspect the CSV**

Run:
```bash
column -t -s, benchmark/ai-proxy/result/bench-result.csv
```

Expected: 7 rows with throughput climbing then plateauing, peak row showing apisix_cpu_pct ≥ 90.

- [ ] **Step 3: Append measured numbers to the spec**

Open `docs/superpowers/specs/2026-05-14-bench-ai-proxy-stream-throughput-design.md` and append a new section at the bottom:

```markdown
## Measured Result (run on YYYY-MM-DD)

Hardware: <CPU model from env.txt>
Kernel:   <uname -r>

Peak: **<T> tokens/s** at concurrency=<N>, APISIX CPU=<P>%.
Full CSV: `benchmark/ai-proxy/result/bench-result.csv`.
```

Fill in the actuals from `result/env.txt` and `result/bench-result.csv`.

- [ ] **Step 4: Commit**

```bash
git add docs/superpowers/specs/2026-05-14-bench-ai-proxy-stream-throughput-design.md
git -c commit.gpgsign=false commit -m "docs(spec): record measured ai-proxy single-CPU streaming throughput"
```

---

## Done

After Task 14 the worktree contains:
- A working `benchmark/ai-proxy/run.sh` that reproduces the measurement
- A `bench-result.csv` with the concurrency sweep
- The spec doc updated with the measured peak

To re-run later just `bash benchmark/ai-proxy/run.sh`. To extend the sweep: `CONCURRENCY=1,2,4,8,16,32,64,128 bash …`.
