package main

import (
	"io"
	"strings"
	"testing"
	"time"
)

type oversizedStderrProcess struct {
	*fakeProcess
	stderr *notifyingReader
}

func (fake *oversizedStderrProcess) Stderr() io.ReadCloser { return fake.stderr }

type notifyingReader struct {
	io.Reader
	closed chan struct{}
}

func (reader *notifyingReader) Close() error { close(reader.closed); return nil }

func TestOversizedDiagnosticsDoNotReportPlaybackFailure(t *testing.T) {
	reader := &notifyingReader{Reader: strings.NewReader(strings.Repeat("x", 128*1024)), closed: make(chan struct{})}
	fake := &oversizedStderrProcess{fakeProcess: newFakeProcess(), stderr: reader}
	player := newPlayer(func(string) (process, error) { return fake, nil })
	if err := player.Play("track.mp3"); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(player.Stop)
	select {
	case <-reader.closed:
	case <-time.After(time.Second):
		t.Fatal("stderr reader did not finish")
	}
	select {
	case event := <-player.Events():
		t.Fatalf("diagnostics reported a terminal error while audio was still running: %+v", event)
	default:
	}
}

func TestCompletionSurvivesFullProgressQueue(t *testing.T) {
	fake := newFakeProcess()
	player := newPlayer(func(string) (process, error) { return fake, nil })
	if err := player.Play("track.mp3"); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(player.Stop)
	generation := player.Generation()
	player.mu.Lock()
	for len(player.events) < cap(player.events) {
		player.events <- PlayerEvent{Kind: PlayerProgress, Generation: generation}
	}
	player.mu.Unlock()
	if err := fake.Kill(); err != nil {
		t.Fatal(err)
	}
	deadline := time.Now().Add(time.Second)
	for {
		player.mu.Lock()
		finished := player.current == nil
		player.mu.Unlock()
		if finished {
			break
		}
		if time.Now().After(deadline) {
			t.Fatal("player did not finish")
		}
		time.Sleep(time.Millisecond)
	}
	for len(player.events) > 0 {
		event := <-player.Events()
		if event.Kind == PlayerCompleted && event.Generation == generation {
			return
		}
	}
	t.Fatal("completion dropped behind progress; UI would stay playing without advancing")
}
