//go:build linux && mipsle

package main

import (
	"context"
	"errors"
	"fmt"
	"golang.org/x/sys/unix"
	"os"
	"os/exec"
	"sync"
	"time"
)

type audioStream struct {
	Commands chan soundCommand
	Errors   chan error
	done     chan struct{}
	cancel   context.CancelFunc
	once     sync.Once
}
type boundedLog struct {
	sync.Mutex
	text []byte
}

func (b *boundedLog) Write(p []byte) (int, error) {
	b.Lock()
	defer b.Unlock()
	n := len(p)
	if len(b.text) < 2048 {
		b.text = append(b.text, p[:min(len(p), 2048-len(b.text))]...)
	}
	return n, nil
}
func (b *boundedLog) String() string { b.Lock(); defer b.Unlock(); return string(b.text) }
func openAudio(volume int) (*audioStream, error) {
	r, w, err := os.Pipe()
	if err != nil {
		return nil, err
	}
	// Keep pipe backlog near 21ms instead of seconds of queued sound.
	if _, err = unix.FcntlInt(w.Fd(), unix.F_SETPIPE_SZ, 4096); err != nil {
		r.Close()
		w.Close()
		return nil, fmt.Errorf("bound PCM pipe: %w", err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	cmd := exec.CommandContext(ctx, "/usr/bin/aplay", "-q", "-D", "hw:0,0", "-B", "20000", "-R", "0", "-T", "500000", "-f", "S16_LE", "-r", "48000", "-c", "2", "-t", "raw")
	log := &boundedLog{}
	cmd.Stdin = r
	cmd.Stderr = log
	if err = cmd.Start(); err != nil {
		cancel()
		r.Close()
		w.Close()
		return nil, err
	}
	r.Close()
	a := &audioStream{Commands: make(chan soundCommand, 128), Errors: make(chan error, 1), done: make(chan struct{}), cancel: cancel}
	go func() {
		defer close(a.done)
		defer w.Close()
		defer cancel()
		// Cancellation closes the write end even if the child stops reading.
		writeDone := make(chan struct{})
		defer close(writeDone)
		go func() {
			select {
			case <-ctx.Done():
				w.Close()
			case <-writeDone:
			}
		}()
		synth := NewSynth()
		synth.SetVolume(volume)
		pcm := make([]int16, 240*2)
		raw := make([]byte, len(pcm)*2)
		var streamErr error
	loop:
		for {
			if ctx.Err() != nil {
				break
			}
			for i := 0; i < 128; i++ {
				select {
				case c := <-a.Commands:
					applySound(synth, c)
				default:
					goto render
				}
			}
		render:
			synth.Render(pcm)
			encodePCM(raw, pcm)
			if e := w.SetWriteDeadline(time.Now().Add(time.Second)); e != nil {
				streamErr = e
				break
			}
			for offset := 0; offset < len(raw); {
				n, e := w.Write(raw[offset:])
				if e != nil {
					streamErr = e
					break loop
				}
				if n == 0 {
					streamErr = errors.New("PCM write made no progress")
					break loop
				}
				offset += n
			}
		}
		cancelled := ctx.Err() != nil
		w.Close()
		cancel()
		waitErr := cmd.Wait()
		if streamErr != nil && !errors.Is(streamErr, os.ErrClosed) {
			select {
			case a.Errors <- fmt.Errorf("audio: %w; %s", streamErr, log.String()):
			default:
			}
		} else if waitErr != nil && !cancelled {
			select {
			case a.Errors <- fmt.Errorf("aplay: %w; %s", waitErr, log.String()):
			default:
			}
		}
	}()
	return a, nil
}
func (a *audioStream) close() { a.once.Do(func() { a.cancel(); <-a.done }) }
func (a *audioStream) send(c soundCommand) error {
	select {
	case <-a.done:
		return errors.New("audio process stopped")
	case a.Commands <- c:
		return nil
	default:
		return errors.New("audio command queue full")
	}
}
