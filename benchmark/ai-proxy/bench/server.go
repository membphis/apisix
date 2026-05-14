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
