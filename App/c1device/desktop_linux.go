//go:build linux

package c1device

import (
	"os"
	"syscall"
)

func ReturnToDesktop() {
	if os.Getenv("C1_C1ANCHER_TERMINAL") == "1" {
		_ = syscall.Kill(os.Getppid(), syscall.SIGKILL)
	}
}
