package main

import (
	"context"
	"runtime/debug"
	"time"
)

// This is a Go-runtime soft memory budget, not a process RSS hard limit.
// The device exposes only ~50 MiB total RAM. Bound GC growth independently of
// host defaults, and collect sooner under animation/recording allocation load.
func configureMemory() { debug.SetMemoryLimit(6 << 20); debug.SetGCPercent(50) }

func exitKey(e keyEvent) bool {
	return e.Down && !e.Reset && (e.Code == 102 || e.Code == 158 || e.Code == 1)
}
func handleInput(m *model, e keyEvent, nowTime time.Time, exporting bool) ([]soundCommand, string) {
	if exitKey(e) {
		return nil, "exit"
	}
	if exporting {
		return nil, ""
	}
	return m.handle(e, nowTime)
}

// Cancellation during export is a normal exit, not a failed user save.
func exportResult(err error) error {
	if err == context.Canceled {
		return nil
	}
	return err
}
