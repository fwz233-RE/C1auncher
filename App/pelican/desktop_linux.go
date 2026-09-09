//go:build linux

package main

import (
	"fmt"
	"os"
	"strings"
	"syscall"
)

func returnToDesktop() {
	if os.Getenv("C1_C1ANCHER_TERMINAL") != "1" {
		return
	}
	parent := os.Getppid()
	data, err := os.ReadFile(fmt.Sprintf("/proc/%d/comm", parent))
	if err == nil && (strings.TrimSpace(string(data)) == "bash" || strings.TrimSpace(string(data)) == "sh") {
		_ = syscall.Kill(parent, syscall.SIGKILL)
	}
}
