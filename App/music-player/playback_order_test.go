package main

import (
	"math/rand"
	"testing"
)

func TestPlaybackOrderSequentialWraps(t *testing.T) {
	order := newPlaybackOrder(3, rand.New(rand.NewSource(1)))
	if got := order.Next(0); got != 1 {
		t.Fatalf("next from 0 = %d, want 1", got)
	}
	if got := order.Next(1); got != 2 {
		t.Fatalf("next from 1 = %d, want 2", got)
	}
	if got := order.Next(2); got != 0 {
		t.Fatalf("next from 2 = %d, want 0", got)
	}
	if got := order.Previous(0); got != 2 {
		t.Fatalf("previous from 0 = %d, want 2", got)
	}
}

func TestPlaybackOrderShuffleUsesEveryTrackBeforeRepeating(t *testing.T) {
	order := newPlaybackOrder(6, rand.New(rand.NewSource(42)))
	order.SetShuffle(true, 0)
	seen := map[int]bool{0: true}
	last := 0
	for index := 0; index < 5; index++ {
		last = order.Next(last)
		if seen[last] {
			t.Fatalf("track %d repeated before shuffle cycle ended: seen=%v", last, seen)
		}
		seen[last] = true
	}
	if len(seen) != 6 {
		t.Fatalf("shuffle cycle visited %d tracks, want 6", len(seen))
	}
	next := order.Next(last)
	if next == last {
		t.Fatalf("new shuffle cycle repeated track %d immediately", last)
	}
}

func TestPlaybackOrderTogglePreservesCurrentTrack(t *testing.T) {
	order := newPlaybackOrder(4, rand.New(rand.NewSource(7)))
	order.SetShuffle(true, 2)
	if got := order.Next(2); got == 2 {
		t.Fatal("shuffle next returned current track")
	}
	order.SetShuffle(false, 2)
	if got := order.Next(2); got != 3 {
		t.Fatalf("sequential next after toggle = %d, want 3", got)
	}
}
