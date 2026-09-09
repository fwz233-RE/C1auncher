package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"os"
	"os/signal"
	"syscall"
	"time"
)

var version = "0.1.1"
var createDisplay = openDisplay

const settleDelay = 2 * time.Second

type display interface {
	draw(frame, bool) error
	events() <-chan uint16
	close() error
}
type state struct {
	interval     time.Duration
	mode         int
	segment      uint32
	paused, help bool
}
type action int

const (
	noAction action = iota
	showUI
	newSegment
	quit
)

var presets = [...]time.Duration{1000 * time.Millisecond, 700 * time.Millisecond, 500 * time.Millisecond, 300 * time.Millisecond, 200 * time.Millisecond, 100 * time.Millisecond}

func (s *state) key(k uint16) action {
	switch k {
	case 1, 45, 102, 158:
		return quit // ESC, X, HOME, BACK
	case 116, 143:
		return noAction // power belongs to the system
	case 35: // H
		s.help = !s.help
		s.paused = true
		return showUI
	case 28, 352, 57, 25: // ENTER, OK, SPACE, P
		if s.help {
			s.help = false
			s.paused = true
			return showUI
		}
		s.paused = !s.paused
		if s.paused {
			return showUI
		}
		return newSegment
	}
	if s.help {
		return noAction
	}
	old := s.interval
	switch {
	case k >= 16 && k <= 21: // Physical Q W E R T Y keys; no number row required.
		s.interval = presets[k-16]
	case k >= 2 && k <= 7: // Optional number-row aliases on external keyboards.
		s.interval = presets[k-2]
	case k == 106 || k == 13 || k == 78:
		s.interval -= 50 * time.Millisecond // right, =/+, keypad +
	case k == 105 || k == 12 || k == 74:
		s.interval += 50 * time.Millisecond
	case k == 50 || k == 15:
		s.mode = (s.mode + 1) % len(modes)
		return newSegment
	case k == 46:
		return newSegment // C: clean; R now selects the 300ms preset.
	default:
		return noAction
	}
	if s.interval < 100*time.Millisecond {
		s.interval = 100 * time.Millisecond
	}
	if s.interval > 2*time.Second {
		s.interval = 2 * time.Second
	}
	if s.interval == old {
		return noAction
	}
	return newSegment
}
func main() {
	if err := run(os.Args[1:]); err != nil {
		fmt.Fprintln(os.Stderr, "refresh-test:", err)
		os.Exit(1)
	}
}
func run(args []string) error {
	fs := flag.NewFlagSet("refresh-test", flag.ContinueOnError)
	ver := fs.Bool("version", false, "print version; no hardware")
	interval := fs.Duration("interval", 700*time.Millisecond, "post-submission wait, 100ms..2s; not panel FPS")
	duration := fs.Duration("duration", 30*time.Minute, "session time limit, 1s..1h")
	mode := fs.String("pattern", "move", "move, flip, or scene")
	logDir := fs.String("log-dir", "", "CSV directory; default /storage/mtp/RefreshTest, fallback /tmp/RefreshTest")
	preview := fs.String("preview", "", "write host preview PNG without opening hardware or logs")
	if err := fs.Parse(args); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			return nil
		}
		return err
	}
	if fs.NArg() != 0 {
		return errors.New("unexpected positional arguments")
	}
	if *ver {
		fmt.Println("refresh-test", version)
		return nil
	}
	if *interval < 100*time.Millisecond || *interval > 2*time.Second {
		return errors.New("interval must be 100ms..2s")
	}
	if *duration < time.Second || *duration > time.Hour {
		return errors.New("duration must be 1s..1h")
	}
	s := state{interval: *interval}
	switch *mode {
	case "move":
	case "flip":
		s.mode = 1
	case "scene":
		s.mode = 2
	default:
		return errors.New("pattern must be move, flip, or scene")
	}
	if *preview != "" {
		return writePreview(*preview, render(s, 12345, 1234*time.Millisecond, "RUN", "LOG OK"))
	}
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM, syscall.SIGHUP)
	defer stop()
	ctx, cancel := context.WithTimeout(ctx, *duration)
	defer cancel()
	log := openSessionLog(*logDir)
	defer log.close()
	return play(ctx, s, log)
}

func busyError(err error) bool {
	// errors.Join may include a write-busy result AND a fatal failure to
	// restore fast-refresh mode. Only retry if every cause is transient.
	if joined, ok := err.(interface{ Unwrap() []error }); ok {
		causes := joined.Unwrap()
		if len(causes) == 0 {
			return false
		}
		for _, cause := range causes {
			if !busyError(cause) {
				return false
			}
		}
		return true
	}
	if wrapped, ok := err.(interface{ Unwrap() error }); ok {
		return busyError(wrapped.Unwrap())
	}
	return err == syscall.EAGAIN || err == syscall.EBUSY
}

func play(ctx context.Context, s state, log *sessionLog) error {
	return playWithHold(ctx, s, log, settleDelay)
}

func playWithHold(ctx context.Context, s state, log *sessionLog, hold time.Duration) (result error) {
	d, err := createDisplay()
	if err != nil {
		log.event("open-error", s, err.Error())
		return err
	}
	defer requestDesktopOnExit()
	defer func() { result = errors.Join(result, d.close()); log.event("exit", s, fmt.Sprint(result)) }()
	var id uint32
	displayReady := false
	var segmentStart time.Time
	// A new segment starts with full cleaning, followed by a fixed hold. The
	// hold is only a protocol convention, NOT a measured display-ready signal.
	needFull, settling := true, false
	uiPending := false
	uiFailures := 0
	fullFailures := 0
	timer := time.NewTimer(0)
	defer timer.Stop()
	reset := func(delay time.Duration) {
		if !timer.Stop() {
			select {
			case <-timer.C:
			default:
			}
		}
		timer.Reset(delay)
	}
	s.segment = 1
	log.event("segment", s, "startup")
	submit := func(kind, label string, full bool) error {
		id++
		full = full || !displayReady
		elapsed := time.Duration(0)
		if !segmentStart.IsZero() && kind == "sample" {
			elapsed = time.Since(segmentStart)
		}
		f := render(s, id, elapsed, label, log.label())
		if s.help {
			f = helpFrame()
		}
		// Rendering is intentionally outside the draw-call measurement.
		start := time.Since(log.started)
		err := d.draw(f, full)
		end := time.Since(log.started)
		if err == nil {
			displayReady = true
		}
		status := "accepted"
		detail := ""
		if err != nil {
			status = "error"
			detail = err.Error()
			if busyError(err) {
				status = "busy"
			}
		}
		log.attempt(kind, s, id, start, end, elapsed, full, status, detail)
		return err
	}
	for {
		select {
		case <-ctx.Done():
			return nil
		case k, ok := <-d.events():
			if !ok {
				return errors.New("input devices disconnected")
			}
			a := s.key(k)
			if a == noAction {
				continue
			}
			log.event("key", s, fmt.Sprintf("key=%d", k))
			if a == quit {
				return nil
			}
			if a == newSegment {
				s.segment++
				segmentStart = time.Time{}
				needFull, settling, fullFailures = true, false, 0
				uiPending = false
				log.event("segment", s, "setting/resume/clean")
				reset(0)
			} else {
				// Pausing/help interrupts the test. Resuming creates a fresh
				// segment so time intervals across pauses are never mixed in.
				needFull, settling = false, false
				uiPending, uiFailures = true, 0
				reset(0)
			}
		case <-timer.C:
			if uiPending {
				err := submit("ui", "PAUSED", false)
				if err != nil {
					if !busyError(err) {
						return err
					}
					uiFailures++
					if uiFailures >= 8 {
						return fmt.Errorf("UI remained busy after 8 attempts: %w", err)
					}
					reset(250 * time.Millisecond)
					continue
				}
				uiPending = false
				reset(time.Second)
				continue
			}
			if needFull {
				label := "SETTLE"
				if s.paused {
					label = "PAUSED"
				}
				err := submit("full", label, true)
				if err != nil {
					if !busyError(err) {
						return err
					}
					fullFailures++
					if fullFailures >= 8 {
						return fmt.Errorf("full refresh remained busy after 8 attempts: %w", err)
					}
					reset(250 * time.Millisecond)
					continue
				}
				needFull, settling = false, true
				reset(hold)
				continue
			}
			if settling {
				settling = false
				segmentStart = time.Now()
				log.event("ready", s, "2s software hold elapsed; physical readiness unknown")
			}
			if s.paused {
				// No periodic redraws while paused, but persist buffered logs.
				log.flush()
				reset(time.Second)
				continue
			}
			if segmentStart.IsZero() {
				segmentStart = time.Now()
			}
			if err := submit("sample", "RUN", false); err != nil && !busyError(err) {
				return err
			}
			// No ticker or pending frame queue. Busy attempts are recorded,
			// then normal pacing continues with a new ID, without retry bursts.
			reset(s.interval)
		}
	}
}
