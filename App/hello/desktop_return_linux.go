//go:build linux

package main

import (
	"os"
	"syscall"
)

func requestDesktopOnExit() {
	if os.Getenv("C1_C1ANCHER_TERMINAL") == "1" {
		_ = syscall.Kill(os.Getppid(), syscall.SIGKILL)
	}
}
