//go:build linux

package main

import (
	"fmt"
	"os"
	"strings"
	"syscall"
)

func requestDesktopOnExit() {
	if os.Getenv("C1_C1ANCHER_TERMINAL") != "1" {
		return
	}
	parent := os.Getppid()
	if parent <= 1 {
		return
	}
	// Match existing apps, but only terminate the launcher-created shell.
	comm, err := os.ReadFile(fmt.Sprintf("/proc/%d/comm", parent))
	if err == nil && (strings.TrimSpace(string(comm)) == "sh" || strings.TrimSpace(string(comm)) == "bash") {
		_ = syscall.Kill(parent, syscall.SIGKILL)
	}
}
