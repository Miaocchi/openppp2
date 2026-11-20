package instance

import (
	"os/exec"
	"sync"
	"time"
)

type instance struct {
	mu    sync.RWMutex
	logMu sync.RWMutex

	cfg Config

	cmd       *exec.Cmd
	exited    chan struct{}
	running   bool
	pid       int
	startedAt *time.Time
	stoppedAt *time.Time
	lastExit  *ExitState

	restartCount    int
	restartWindowAt time.Time
	pendingRestart  chan struct{}
	manualStop      bool
	shutdown        bool
	healthCheck     *HealthChecker
	logs            *ringBuffer
	logSubscribers  map[chan LogEntry]struct{}
	runtimeStats    map[string]string
}

// Caller holds i.mu. Closing the token wakes a worker still in backoff.
func (i *instance) cancelRestartLocked() {
	if i.pendingRestart != nil {
		close(i.pendingRestart)
		i.pendingRestart = nil
	}
}

type ringBuffer struct {
	items []LogEntry
	start int
	count int
}

func newRingBuffer(size int) *ringBuffer {
	if size <= 0 {
		size = 1
	}
	return &ringBuffer{items: make([]LogEntry, size)}
}

func (r *ringBuffer) add(entry LogEntry) {
	if len(r.items) == 0 {
		return
	}
	idx := (r.start + r.count) % len(r.items)
	if r.count == len(r.items) {
		r.items[r.start] = entry
		r.start = (r.start + 1) % len(r.items)
		return
	}
	r.items[idx] = entry
	r.count++
}

func (r *ringBuffer) list(n int, stream string) []LogEntry {
	if n <= 0 || n > r.count {
		n = r.count
	}
	if n == 0 {
		return nil
	}
	if stream == "" || stream == "all" {
		first := (r.start + r.count - n) % len(r.items)
		result := make([]LogEntry, n)
		copied := copy(result, r.items[first:])
		copy(result[copied:], r.items)
		return result
	}

	// Find only the suffix containing the last n matching entries.
	first, matches := r.count, 0
	for first > 0 && matches < n {
		first--
		if r.items[(r.start+first)%len(r.items)].Stream == stream {
			matches++
		}
	}
	if matches == 0 {
		return nil
	}
	result := make([]LogEntry, 0, matches)
	for i := first; i < r.count; i++ {
		entry := r.items[(r.start+i)%len(r.items)]
		if entry.Stream == stream {
			result = append(result, entry)
		}
	}
	return result
}
