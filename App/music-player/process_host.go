//go:build !linux

package main

import (
	"os"
	"os/exec"
)

type hostSignal string

func (signal hostSignal) String() string { return string(signal) }
func (hostSignal) Signal()               {}

var (
	processSignalStop      os.Signal = hostSignal("stop")
	processSignalContinue  os.Signal = hostSignal("continue")
	processSignalTerminate os.Signal = hostSignal("terminate")
)

func configureProcess(_ *exec.Cmd) {}
