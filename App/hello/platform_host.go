//go:build !linux || !mipsle

package main

import (
	"bufio"
	"fmt"
	"os"
)

type hostPlatform struct {
	output chan keyInput
}

func openPlatform() (*hostPlatform, error) {
	platform := &hostPlatform{output: make(chan keyInput, 16)}
	go platform.readInput()
	return platform, nil
}

func (platform *hostPlatform) draw(_ frame, _ bool) error {
	fmt.Fprint(os.Stdout, "\x1b[2J\x1b[H")
	fmt.Fprintln(os.Stdout, "HELLO WORLD")
	fmt.Fprintln(os.Stdout, "Independent e-paper application host preview")
	fmt.Fprintln(os.Stdout, "Press q to exit")
	return nil
}

func (platform *hostPlatform) events() <-chan keyInput {
	return platform.output
}

func (platform *hostPlatform) close() {}

func (platform *hostPlatform) readInput() {
	defer close(platform.output)
	reader := bufio.NewReader(os.Stdin)
	for {
		character, _, err := reader.ReadRune()
		if err != nil {
			return
		}
		if character == 'q' || character == 'Q' || character == 27 {
			platform.output <- keyInput{name: "BACK", exit: true}
			return
		}
		platform.output <- keyInput{name: "KEY"}
	}
}
