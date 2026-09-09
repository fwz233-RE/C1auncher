package main

import (
	"encoding/csv"
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"time"
)

type sessionLog struct {
	started   time.Time
	lastFlush time.Time
	file      *os.File
	csv       *csv.Writer
	failed    bool
	path      string
}

func openSessionLog(directory string) *sessionLog {
	l := &sessionLog{started: time.Now()}
	dirs := []string{directory}
	if directory == "" {
		dirs = []string{"/storage/mtp/RefreshTest", "/tmp/RefreshTest"}
	}
	for _, dir := range dirs {
		if err := os.MkdirAll(dir, 0700); err != nil {
			fmt.Fprintln(os.Stderr, "refresh-test: log directory:", err)
			continue
		}
		f, err := os.CreateTemp(dir, "refresh-"+l.started.UTC().Format("20060102T150405Z")+"-*.csv")
		if err != nil {
			fmt.Fprintln(os.Stderr, "refresh-test: log file:", err)
			continue
		}
		l.file, l.path = f, f.Name()
		l.csv = csv.NewWriter(f)
		l.write([]string{"kind", "id", "segment", "pattern", "wait_ms", "start_us", "end_us", "scene_us", "full", "status", "detail"})
		l.event("session", state{}, "version="+version+"; utc="+l.started.UTC().Format(time.RFC3339Nano)+"; times=monotonic_us; accepted=draw_returned_without_error_not_visible_frame; log="+filepath.ToSlash(l.path))
		l.flush()
		fmt.Fprintln(os.Stderr, "refresh-test: CSV:", l.path)
		return l
	}
	l.failed = true
	fmt.Fprintln(os.Stderr, "refresh-test: NO LOG; filming still works, submission timing will be unavailable")
	return l
}
func (l *sessionLog) label() string {
	if !l.healthy() {
		return "NO LOG"
	}
	if filepath.ToSlash(filepath.Dir(l.path)) == "/tmp/RefreshTest" {
		return "TMP LOG"
	}
	return "LOG OK"
}
func (l *sessionLog) healthy() bool { return l != nil && l.csv != nil && !l.failed }
func (l *sessionLog) write(row []string) {
	if !l.healthy() {
		return
	}
	if err := l.csv.Write(row); err != nil {
		l.fail(err)
	}
}
func (l *sessionLog) fail(err error) {
	if !l.failed {
		fmt.Fprintln(os.Stderr, "refresh-test: logging stopped:", err)
	}
	l.failed = true
}
func (l *sessionLog) flush() {
	if !l.healthy() {
		return
	}
	l.csv.Flush()
	if err := l.csv.Error(); err != nil {
		l.fail(err)
	}
	l.lastFlush = time.Now()
}
func (l *sessionLog) attempt(kind string, s state, id uint32, start, end, elapsed time.Duration, full bool, status, detail string) {
	l.write([]string{kind, strconv.FormatUint(uint64(id), 10), strconv.FormatUint(uint64(s.segment), 10), modes[s.mode], strconv.FormatInt(s.interval.Milliseconds(), 10), strconv.FormatInt(start.Microseconds(), 10), strconv.FormatInt(end.Microseconds(), 10), strconv.FormatInt(elapsed.Microseconds(), 10), strconv.FormatBool(full), status, detail})
	if time.Since(l.lastFlush) >= time.Second {
		l.flush()
	}
}
func (l *sessionLog) event(kind string, s state, detail string) {
	t := time.Since(l.started)
	l.attempt(kind, s, 0, t, t, 0, false, "event", detail)
	l.flush()
}
func (l *sessionLog) close() {
	if l == nil || l.file == nil {
		return
	}
	l.flush()
	if err := l.file.Sync(); err != nil {
		l.fail(err)
	}
	if err := l.file.Close(); err != nil {
		l.fail(err)
	}
}
