package main

import (
	"bytes"
	"context"
	"math"
	"net/http/httptest"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"
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
