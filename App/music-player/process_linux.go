//go:build linux

package main

import (
	"os"
	"os/exec"
	"syscall"
)

var (
	processSignalStop      os.Signal = syscall.SIGSTOP
	processSignalContinue  os.Signal = syscall.SIGCONT
	processSignalTerminate os.Signal = syscall.SIGTERM
)

func configureProcess(command *exec.Cmd) {
	command.SysProcAttr = &syscall.SysProcAttr{Pdeathsig: syscall.SIGTERM}
}
