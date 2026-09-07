package main

import (
	"fmt"
	"os"
	"path/filepath"
	"runtime/debug"
	"time"
)

// Keep fatal diagnostics outside the book directory. This also records panic
// stacks on devices where the launcher immediately hides application stderr.
func runWithDiagnostics() (err error) {
	defer func() {
		if recovered := recover(); recovered != nil {
			err = fmt.Errorf("panic: %v\n%s", recovered, debug.Stack())
		}
		if err != nil {
			writeFailureLog(err)
		}
	}()
	return run()
}

func writeFailureLog(err error) {
	home := envOr("C1_BOOK_READER_HOME", "/usr/data/c1/book-reader")
	if os.MkdirAll(home, 0755) != nil {
		return
	}
	// One bounded last-failure record rather than an indefinitely growing log.
	text := fmt.Sprintf("%s book-reader %s\n%v\n", time.Now().Format(time.RFC3339), version, err)
	if len(text) > 64*1024 {
		text = text[:64*1024]
	}
	_ = os.WriteFile(filepath.Join(home, "last-error.log"), []byte(text), 0600)
}
