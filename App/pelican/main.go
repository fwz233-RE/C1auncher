package main

import (
	"context"
	"flag"
	"fmt"
	"io"
	"os"
	"os/signal"
	"runtime/debug"
	"syscall"
	"time"

	"c1device"
)

var version = "dev"

const defaultInterval = 750 * time.Millisecond

func main() {
	if err := run(os.Args[1:], os.Stdout); err != nil {
		fmt.Fprintln(os.Stderr, "pelican:", err)
		os.Exit(1)
	}
}

func run(args []string, out io.Writer) error {
	flags := flag.NewFlagSet("pelican", flag.ContinueOnError)
	flags.SetOutput(out)
	showVersion := flags.Bool("version", false, "print version without accessing the device")
	preview := flags.String("preview", "", "write a looping GIF preview (no device required)")
	interval := flags.Duration("interval", defaultInterval, "wait between frame submissions, e.g. 750ms (not measured panel FPS)")
	if err := flags.Parse(args); err != nil {
		if err == flag.ErrHelp {
			return nil
		}
		return err
	}
	if flags.NArg() != 0 {
		return fmt.Errorf("unexpected positional arguments")
	}
	if *showVersion {
		_, err := fmt.Fprintf(out, "pelican %s\n", version)
		return err
	}
	if *interval < 100*time.Millisecond || *interval > 5*time.Second {
		return fmt.Errorf("interval must be between 100ms and 5s")
	}
	if *preview != "" {
		return writePreview(*preview, *interval)
	}
	debug.SetMemoryLimit(12 << 20)
	debug.SetGCPercent(50)
	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM, syscall.SIGHUP)
	defer stop()
	frames := animationFrames()
	platform, err := c1device.OpenPlatform()
	if err != nil {
		return fmt.Errorf("open screen: %w", err)
	}
	defer func() { _ = platform.Close(); returnToDesktop() }()
	return play(ctx, platform, &frames, *interval)
}

func play(ctx context.Context, platform c1device.Platform, frames *[frameCount]c1device.Frame, interval time.Duration) error {
	// One in-flight write, no accumulated frames. Slow IO runs separately so
	// keyboard and signals remain serviceable. Cleanup waits for that write.
	type request struct {
		index int
		full  bool
	}
	jobs := make(chan request)
	results := make(chan error, 1)
	done := make(chan struct{})
	go func() {
		defer close(done)
		for job := range jobs {
			results <- platform.Draw(frames[job.index], job.full)
		}
	}()
	defer func() { close(jobs); <-done }()
	timer := time.NewTimer(interval)
	if !timer.Stop() {
		<-timer.C
	}
	defer timer.Stop()
	var ticks <-chan time.Time
	index := 0
	select {
	case <-ctx.Done():
		return nil
	case jobs <- request{full: true}:
	}
	for {
		select {
		case <-ctx.Done():
			return nil
		case event, ok := <-platform.Events():
			if !ok || event.Key == c1device.KeyBack || (event.Key == c1device.KeyRune && event.Rune == 'q') {
				return nil
			}
		case err := <-results:
			if err != nil {
				return fmt.Errorf("draw frame: %w", err)
			}
			timer.Reset(interval)
			ticks = timer.C
		case <-ticks:
			ticks = nil
			index = (index + 1) % frameCount
			select {
			case <-ctx.Done():
				return nil
			case jobs <- request{index: index}:
			}
		}
	}
}
