package main

import (
	"context"
	"fmt"
	"os"
	"os/signal"
	"syscall"
)

var version = "dev"

type keyInput struct {
	name string
	exit bool
}

func main() {
	if len(os.Args) > 1 && os.Args[1] == "--version" {
		fmt.Printf("c1hello %s\n", version)
		return
	}
	if err := run(); err != nil {
		fmt.Fprintf(os.Stderr, "hello: %v\n", err)
		os.Exit(1)
	}
}

func run() error {
	platform, err := openPlatform()
	if err != nil {
		return fmt.Errorf("initialize platform: %w", err)
	}
	defer platform.close()
	ctx, stop := signal.NotifyContext(
		context.Background(),
		syscall.SIGINT,
		syscall.SIGTERM,
		syscall.SIGHUP,
	)
	defer stop()
	state := helloState{lastKey: "READY"}
	if err := platform.draw(renderHello(state), true); err != nil {
		return fmt.Errorf("draw initial screen: %w", err)
	}
	for {
		select {
		case <-ctx.Done():
			requestDesktopOnExit()
			return nil
		case event, ok := <-platform.events():
			if !ok || event.exit {
				requestDesktopOnExit()
				return nil
			}
			state.lastKey = event.name
			state.count++
			if err := platform.draw(renderHello(state), false); err != nil {
				return fmt.Errorf("refresh screen: %w", err)
			}
		}
	}
}
