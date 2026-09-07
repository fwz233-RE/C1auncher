package main

import (
	"encoding/json"
	"os"
	"path/filepath"
	"runtime"
	"time"
)

type sessionRecord struct {
	Version      string `json:"version"`
	At           string `json:"at"`
	State        string `json:"state"`
	Error        string `json:"error,omitempty"`
	HeapAlloc    uint64 `json:"heap_alloc"`
	RuntimeBytes uint64 `json:"runtime_bytes"`
	Goroutines   int    `json:"goroutines"`
}

// A single bounded metadata record, not a growing event log. Never records notes.
func writeSessionRecord(path, state string, err error) {
	var mem runtime.MemStats
	runtime.ReadMemStats(&mem)
	rec := sessionRecord{Version: version, At: time.Now().UTC().Format(time.RFC3339), State: state, HeapAlloc: mem.HeapAlloc, RuntimeBytes: mem.Sys - mem.HeapReleased, Goroutines: runtime.NumGoroutine()}
	if err != nil {
		rec.Error = err.Error()
		if len(rec.Error) > 4096 {
			rec.Error = rec.Error[:4096]
		}
	}
	data, e := json.Marshal(rec)
	if e != nil {
		return
	}
	dir := filepath.Dir(path)
	if os.MkdirAll(dir, 0700) != nil {
		return
	}
	// Private predictable file name is replaced atomically, never opened through
	// an existing symlink. At most one application holds the hardware lease.
	f, e := os.CreateTemp(dir, ".pinao-session-*")
	if e != nil {
		return
	}
	defer os.Remove(f.Name())
	_, e = f.Write(append(data, '\n'))
	ce := f.Close()
	if e == nil && ce == nil {
		_ = os.Rename(f.Name(), filepath.Join(dir, "last-session.json"))
	}
}
func recordSessionResult(path string, err error) {
	state := "exited"
	if err != nil {
		state = "failed"
	}
	writeSessionRecord(path, state, err)
}
