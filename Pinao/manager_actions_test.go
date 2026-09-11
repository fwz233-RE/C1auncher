package main

import (
	"path/filepath"
	"testing"
	"time"
)

func TestManagementActions(t *testing.T) {
	m := newModel(defaultSong())
	m.Management = true
	m.StoragePath = filepath.Join(t.TempDir(), "song.json")
	m.ManagerItems = []archiveEntry{{Name: "take.wav", Path: "/tmp/take.wav", IsWAV: true}, {Name: "song.json", Path: "/tmp/song.json"}}
	_, a := m.handle(keyEvent{Code: 28, Down: true}, time.Now())
	if a != "manager-play:/tmp/take.wav" {
		t.Fatalf("wav action %q", a)
	}
	m.ManagerIndex = 1
	_, a = m.handle(keyEvent{Code: 28, Down: true}, time.Now())
	if a != "manager-preview:/tmp/song.json" {
		t.Fatalf("preview action %q", a)
	}
	_, a = m.handle(keyEvent{Code: 24, Down: true}, time.Now())
	if a != "manager-open:/tmp/song.json" {
		t.Fatalf("open action %q", a)
	}
	m.Management = false
	_, _ = m.handle(keyEvent{Code: 50, Down: true}, time.Now())
	if !m.Management {
		t.Fatal("M did not open manager")
	}
}
