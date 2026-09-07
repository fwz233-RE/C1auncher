package main

import (
	"bufio"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"time"
)

type process interface {
	Start() error
	Stderr() io.ReadCloser
	Signal(os.Signal) error
	Kill() error
	Wait() error
}

type processFactory func(path string) (process, error)

type execProcess struct {
	command *exec.Cmd
	stderr  io.ReadCloser
}

func newFFplayProcess(path string) (process, error) {
	command := exec.Command("/usr/bin/ffplay", ffplayArguments(path)...)
	configureProcess(command)
	stderr, err := command.StderrPipe()
	if err != nil {
		return nil, fmt.Errorf("open ffplay stderr: %w", err)
	}
	return &execProcess{command: command, stderr: stderr}, nil
}

func ffplayArguments(path string) []string {
	return []string{"-nodisp", "-vn", "-autoexit", "-hide_banner", "-stats", "--", path}
}

func (item *execProcess) Start() error                  { return item.command.Start() }
func (item *execProcess) Stderr() io.ReadCloser         { return item.stderr }
func (item *execProcess) Signal(signal os.Signal) error { return item.command.Process.Signal(signal) }
func (item *execProcess) Kill() error                   { return item.command.Process.Kill() }
func (item *execProcess) Wait() error                   { return item.command.Wait() }

type PlayerEventKind uint8

const (
	PlayerProgress PlayerEventKind = iota
	PlayerDuration
	PlayerCompleted
	PlayerFailed
)

type PlayerEvent struct {
	Kind       PlayerEventKind
	Generation uint64
	Position   time.Duration
	Duration   time.Duration
	Err        error
}

type playbackRun struct {
	process    process
	generation uint64
	done       chan struct{}
	stopping   bool
}

type Player struct {
	control sync.Mutex
	mu      sync.Mutex
	factory processFactory
	current *playbackRun
	nextGen uint64
	events  chan PlayerEvent
	grace   time.Duration
	paused  bool
}

func newPlayer(factory processFactory) *Player {
	if factory == nil {
		factory = newFFplayProcess
	}
	return &Player{
		factory: factory,
		events:  make(chan PlayerEvent, 128),
		grace:   400 * time.Millisecond,
	}
}

func (player *Player) Events() <-chan PlayerEvent { return player.events }

func (player *Player) Generation() uint64 {
	player.mu.Lock()
	defer player.mu.Unlock()
	return player.nextGen
}

func (player *Player) Play(path string) error {
	player.control.Lock()
	defer player.control.Unlock()

	player.detachAndStop()
	process, err := player.factory(path)
	if err != nil {
		return err
	}
	if err := process.Start(); err != nil {
		return fmt.Errorf("start ffplay: %w", err)
	}

	player.mu.Lock()
	player.nextGen++
	run := &playbackRun{process: process, generation: player.nextGen, done: make(chan struct{})}
	player.current = run
	player.paused = false
	player.mu.Unlock()

	go player.readStderr(run)
	go player.wait(run)
	return nil
}

func (player *Player) PauseToggle() error {
	player.control.Lock()
	defer player.control.Unlock()

	player.mu.Lock()
	run := player.current
	paused := player.paused
	player.mu.Unlock()
	if run == nil {
		return nil
	}
	signal := processSignalStop
	if paused {
		signal = processSignalContinue
	}
	if err := run.process.Signal(signal); err != nil {
		return fmt.Errorf("signal player: %w", err)
	}
	player.mu.Lock()
	if player.current == run {
		player.paused = !paused
	}
	player.mu.Unlock()
	return nil
}

func (player *Player) Stop() {
	player.control.Lock()
	defer player.control.Unlock()
	player.detachAndStop()
}

func (player *Player) detachAndStop() {
	player.mu.Lock()
	run := player.current
	if run != nil {
		run.stopping = true
		player.current = nil
		player.nextGen++
	}
	player.paused = false
	player.mu.Unlock()
	if run == nil {
		return
	}

	_ = run.process.Signal(processSignalContinue)
	_ = run.process.Signal(processSignalTerminate)
	timer := time.NewTimer(player.grace)
	defer timer.Stop()
	select {
	case <-run.done:
		return
	case <-timer.C:
		_ = run.process.Kill()
		<-run.done
	}
}

func (player *Player) wait(run *playbackRun) {
	err := run.process.Wait()
	close(run.done)

	player.mu.Lock()
	if player.current != run || run.stopping {
		player.mu.Unlock()
		return
	}
	player.current = nil
	player.paused = false
	kind := PlayerCompleted
	if err != nil {
		kind = PlayerFailed
	}
	event := PlayerEvent{Kind: kind, Generation: run.generation, Err: err}
	player.sendLocked(event)
	player.mu.Unlock()
}

func (player *Player) readStderr(run *playbackRun) {
	stderr := run.process.Stderr()
	defer stderr.Close()
	scanner := bufio.NewScanner(stderr)
	scanner.Split(splitCRLF)
	scanner.Buffer(make([]byte, 1024), 64*1024)
	for scanner.Scan() {
		position, duration, kind, ok := parseFFplayLine(scanner.Text())
		if !ok {
			continue
		}
		player.mu.Lock()
		if player.current == run && !run.stopping {
			player.sendLocked(PlayerEvent{
				Kind:       kind,
				Generation: run.generation,
				Position:   position,
				Duration:   duration,
			})
		}
		player.mu.Unlock()
	}
	if scanner.Err() != nil {
		// Diagnostics are advisory. A long metadata line must not mark a live
		// player as failed or stop draining its pipe and stall audio output.
		// Only Wait reports the authoritative terminal playback result.
		_, _ = io.Copy(io.Discard, stderr)
	}
}

func (player *Player) sendLocked(event PlayerEvent) {
	select {
	case player.events <- event:
		return
	default:
	}
	if event.Kind == PlayerProgress {
		return // Progress is advisory and may be dropped under load.
	}
	// Producers hold mu, so after removing one queued event there is room
	// even if the consumer drains concurrently. Never block child cleanup,
	// but retain completion/failure so the UI cannot get stuck playing.
	select {
	case <-player.events:
	default:
	}
	player.events <- event
}

func eventApplies(active uint64, event PlayerEvent) bool {
	return active != 0 && event.Generation == active
}

var durationPattern = regexp.MustCompile(`Duration:\s*(\d+):(\d+):(\d+(?:\.\d+)?)`)
var statsPattern = regexp.MustCompile(`^\s*(\d+(?:\.\d+)?)\s`)

func parseFFplayLine(line string) (position, duration time.Duration, kind PlayerEventKind, ok bool) {
	if match := durationPattern.FindStringSubmatch(line); match != nil {
		hours, _ := strconv.Atoi(match[1])
		minutes, _ := strconv.Atoi(match[2])
		seconds, err := strconv.ParseFloat(match[3], 64)
		if err == nil {
			duration = time.Duration(hours)*time.Hour + time.Duration(minutes)*time.Minute + time.Duration(seconds*float64(time.Second))
			return 0, duration, PlayerDuration, true
		}
	}
	if match := statsPattern.FindStringSubmatch(line); match != nil {
		seconds, err := strconv.ParseFloat(match[1], 64)
		if err == nil {
			return time.Duration(seconds * float64(time.Second)), 0, PlayerProgress, true
		}
	}
	return 0, 0, 0, false
}

func splitCRLF(data []byte, atEOF bool) (advance int, token []byte, err error) {
	for index, value := range data {
		if value != '\r' && value != '\n' {
			continue
		}
		advance = index + 1
		for advance < len(data) && (data[advance] == '\r' || data[advance] == '\n') {
			advance++
		}
		return advance, data[:index], nil
	}
	if atEOF && len(data) != 0 {
		return len(data), data, nil
	}
	return 0, nil, nil
}

func formatPlaybackTime(value time.Duration) string {
	if value < 0 {
		value = 0
	}
	total := int(value / time.Second)
	hours := total / 3600
	minutes := (total % 3600) / 60
	seconds := total % 60
	if hours > 0 {
		return fmt.Sprintf("%d:%02d:%02d", hours, minutes, seconds)
	}
	return fmt.Sprintf("%02d:%02d", minutes, seconds)
}

func cleanProcessError(err error) error {
	if err == nil {
		return nil
	}
	return errors.New(strings.TrimSpace(err.Error()))
}
