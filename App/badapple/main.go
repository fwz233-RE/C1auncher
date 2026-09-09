package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"os"
	"os/signal"
	"runtime/debug"
	"syscall"
	"time"
)

var version = "0.1.1"
var createDisplay = openDisplay

type options struct {
	path, check, preview string
	interval, duration   time.Duration
	loop                 bool
}
type display interface {
	draw(frame, bool) error
	events() <-chan uint16
	close() error
}

func defaultAsset() string {
	// External media is an explicit override only. Normal installations always
	// use the built-in animation, even if an old sidecar file exists.
	return os.Getenv("C1_BADAPPLE_FILE")
}
func main() {
	err := run(os.Args[1:])
	if err != nil {
		fmt.Fprintln(os.Stderr, "badapple:", err)
	}
	if err != nil {
		os.Exit(1)
	}
}
func run(args []string) error {
	var o options
	fs := flag.NewFlagSet("badapple", flag.ContinueOnError)
	ver := fs.Bool("version", false, "print version without opening hardware")
	previewFrame := fs.Uint("preview-frame", 0, "zero-based frame index for --preview")
	fs.StringVar(&o.path, "file", "", "C1BA0001 preprocessed animation (.bap)")
	fs.StringVar(&o.check, "check", "", "validate every frame and print metadata; no hardware")
	fs.StringVar(&o.preview, "preview", "", "write an animation frame to a PNG on the host (built-in media by default)")
	fs.DurationVar(&o.interval, "interval", 700*time.Millisecond, "minimum wait between frame submissions, 100ms..2s; not panel FPS")
	fs.DurationVar(&o.duration, "duration", 0, "exit after this duration (0 means unlimited)")
	fs.BoolVar(&o.loop, "loop", true, "loop at the original media speed, skipping overdue frames")
	if err := fs.Parse(args); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			return nil
		}
		return err
	}
	if fs.NArg() != 0 {
		return errors.New("unexpected positional arguments; use --help")
	}
	if *ver {
		fmt.Println("badapple", version)
		return nil
	}
	if o.check != "" {
		a, err := openAsset(o.check)
		if err != nil {
			return err
		}
		defer a.close()
		for i := uint32(0); i < a.count; i++ {
			if _, err := a.read(i); err != nil {
				return err
			}
		}
		fmt.Printf("%dx%d, %d bytes/frame, %d frames, %d/%d fps, duration=%s\n", width, height, frameBytes, a.count, a.fpsNum, a.fpsDen, a.duration())
		return nil
	}
	if o.path == "" {
		o.path = defaultAsset()
	}
	if o.preview != "" {
		if uint64(*previewFrame) > 108000 {
			return errors.New("preview frame out of range")
		}
		return writePreviewAt(o.path, o.preview, uint32(*previewFrame))
	}
	if o.interval < 100*time.Millisecond || o.interval > 2*time.Second || o.duration < 0 {
		return errors.New("interval must be 100ms..2s and duration must be nonnegative")
	}
	debug.SetMemoryLimit(8 << 20)
	debug.SetGCPercent(50)
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM, syscall.SIGHUP)
	defer stop()
	if o.duration > 0 {
		var cancel context.CancelFunc
		ctx, cancel = context.WithTimeout(ctx, o.duration)
		defer cancel()
	}
	return play(ctx, o)
}
func exitKey(k uint16) bool  { return k == 102 || k == 158 || k == 16 }           // HOME, BACK, Q
func pauseKey(k uint16) bool { return k == 28 || k == 352 || k == 57 || k == 25 } // ENTER, OK, SPACE, P

func play(ctx context.Context, o options) (result error) {
	// Validate media before taking over the display. Missing/bad media gets a
	// readable screen instead of silently exiting back into a launcher loop.
	a, assetErr := openAsset(o.path)
	if a != nil {
		defer a.close()
	}
	d, err := createDisplay()
	if err != nil {
		return err
	}
	defer requestDesktopOnExit()
	defer func() { result = errors.Join(result, d.close()) }()
	if assetErr != nil {
		fmt.Fprintln(os.Stderr, "badapple:", assetErr)
		if err = d.draw(messageFrame("BAD APPLE", "ANIMATION ERROR", "CHECK --FILE OVERRIDE", "OR REINSTALL APP", "Q / BACK / HOME: EXIT"), true); err != nil {
			return err
		}
		for {
			select {
			case <-ctx.Done():
				return nil
			case k, ok := <-d.events():
				if !ok || exitKey(k) {
					return nil
				}
			}
		}
	}
	p := playback{started: time.Now(), length: a.duration(), loop: o.loop}
	var last frame
	ready := false
	help := false
	draw := func(full bool) error {
		f, err := a.read(a.index(p.position(time.Now())))
		if err != nil {
			return err
		}
		if help {
			f = messageFrame("BAD APPLE", "OK / SPACE / P: PAUSE", "LEFT / RIGHT: SEEK 5S", "N: RESTART  H: HELP", "R: CLEAN WHEN PAUSED", "Q / BACK / HOME: EXIT")
		} else if p.paused {
			banner(&f, "PAUSED  OK:PLAY N:RESTART")
		}
		if ready && last == f && !full {
			return nil
		}
		if err := d.draw(f, full); err != nil {
			return err
		}
		last = f
		ready = true
		return nil
	}
	// First paint is a full refresh; establish the media clock afterwards.
	if err = draw(true); err != nil {
		return err
	}
	p.started = time.Now()
	timer := time.NewTimer(o.interval)
	defer timer.Stop()
	input := d.events()
	for {
		select {
		case <-ctx.Done():
			return nil
		case k, ok := <-input:
			if !ok {
				return errors.New("input devices disconnected")
			}
			if exitKey(k) {
				return nil
			}
			now := time.Now()
			redraw, full := true, false
			switch {
			case k == 143 || k == 116:
				continue // system power keys stay with the core
			case pauseKey(k):
				if help {
					help = false
				} else {
					p.toggle(now)
				}
			case k == 35: // H; help always pauses, closing leaves the player paused
				help = !help
				if help && !p.paused {
					p.toggle(now)
				}
			case k == 49:
				p.restart(now) // N
			case k == 105:
				p.seek(-5*time.Second, now)
			case k == 106:
				p.seek(5*time.Second, now)
			case k == 19:
				full = p.paused
				redraw = full // R; never interrupt playing with full clears
			default:
				redraw = false
			}
			if redraw {
				if err = draw(full); err != nil {
					return err
				}
			}
		case <-timer.C:
			if !p.paused {
				if !p.loop && p.position(time.Now()) >= p.length {
					p.base = p.length
					p.paused = true
				}
				if err = draw(false); err != nil {
					return err
				}
			}
			// A single-shot timer means slow writes never build a pending frame queue.
			// Media time, not frame count, chooses the next frame, including on resume.
			timer.Reset(o.interval)
		}
	}
}
