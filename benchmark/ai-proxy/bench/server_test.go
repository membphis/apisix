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
