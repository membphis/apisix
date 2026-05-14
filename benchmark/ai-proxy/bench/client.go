package main

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

// newLatencyHist returns a histogram for inter-event latency between 1ns and
// 1s with 3 significant figures of precision (values are nanoseconds).
func newLatencyHist() *hdrhist.Histogram {
	return hdrhist.New(1, 1_000_000_000, 3)
}

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
		intervals = 2 // pidstat parser needs >= 2 samples
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
