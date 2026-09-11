package main

import (
	"path/filepath"
	"testing"
)

func TestTieLargestProjectRoundTrip(t *testing.T) {
	s := defaultSong()
	s.Format = 3
	s.Pages = make([][steps][]Hit, maxPages-1)
	for pos := 0; pos < maxPages*steps; pos++ {
		for i := 0; i < voiceCount; i++ {
			cell := &s.patternAt(pos / steps)[pos%steps]
			*cell = append(*cell, Hit{MIDI: 60 + i, Tie: pos > 0})
		}
	}
	path := filepath.Join(t.TempDir(), "full.json")
	if err := saveSong(path, s); err != nil {
		t.Fatal(err)
	}
	got, err := loadSong(path)
	if err != nil {
		t.Fatal(err)
	}
	if got.pageCount() != maxPages || len(got.Pages[maxPages-2][15]) != voiceCount {
		t.Fatal("full project truncated")
	}
}
func BenchmarkTieTransportEightVoices(b *testing.B) {
	var p loopTransport
	var buffer [9]soundCommand
	var hits [8]Hit
	for i := range hits {
		hits[i] = Hit{MIDI: 60 + i, Tie: true}
	}
	b.ReportAllocs()
	for i := 0; i < b.N; i++ {
		p.commands(buffer[:0], hits[:], i, true)
	}
}
