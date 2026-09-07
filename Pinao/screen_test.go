package main

import (
	"errors"
	"testing"
	"time"
)

func TestScreenKeepsLatestAndDoesNotBlock(t *testing.T) {
	entered := make(chan struct{}, 1)
	release := make(chan struct{})
	seen := make(chan frame, 3)
	w := newScreenWriter(func(f frame, _ bool) error {
		seen <- f
		if f[0] == 1 {
			entered <- struct{}{}
			<-release
		}
		return nil
	})
	var f frame
	f[0] = 1
	w.submit(f)
	select {
	case <-entered:
	case <-time.After(time.Second):
		t.Fatal("writer did not start")
	}
	for i := 2; i <= 100; i++ {
		f[0] = byte(i)
		w.submit(f)
	}
	close(release)
	<-seen
	select {
	case latest := <-seen:
		if latest[0] != 100 {
			t.Fatal("stale frame", latest[0])
		}
	case <-time.After(time.Second):
		t.Fatal("missing latest frame")
	}
	w.close()
}
func TestScreenErrorPropagates(t *testing.T) {
	want := errors.New("screen failed")
	w := newScreenWriter(func(frame, bool) error { return want })
	defer w.close()
	w.submit(frame{})
	select {
	case got := <-w.errors:
		if !errors.Is(got, want) {
			t.Fatal(got)
		}
	case <-time.After(time.Second):
		t.Fatal("error not delivered")
	}
}
func TestHardwareControls(t *testing.T) {
	m := newModel(defaultSong())
	now := time.Now()
	m.handle(keyEvent{Code: 16, Down: true}, now)
	if m.Song.Tone != 1 {
		t.Fatal("Q tone")
	}
	for c := uint16(46); c <= 49; c++ {
		cmd, _ := m.handle(keyEvent{Code: c, Down: true}, now)
		if len(cmd) != 1 || cmd[0].Kind != "drum" || cmd[0].Drum != int(c)-46 {
			t.Fatal("CVBN drums", cmd)
		}
	}
	m.handle(keyEvent{Code: 38, Down: true}, now)
	if !m.Help {
		t.Fatal("L help")
	}
	m.handle(keyEvent{Code: 38, Down: true}, now)
	_, a := m.handle(keyEvent{Code: 25, Down: true}, now)
	if a != "export" {
		t.Fatal("P export")
	}
}
