package main

import (
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
)

const maxSongBytes = 1 << 20 // At most 64 pages, 16 steps, 8 hits per step.

func loadSong(path string) (Song, error) {
	f, err := os.Open(path)
	if errors.Is(err, os.ErrNotExist) {
		return defaultSong(), nil
	}
	if err != nil {
		return Song{}, err
	}
	defer f.Close()
	info, err := f.Stat()
	if err != nil {
		return Song{}, err
	}
	if info.Size() > maxSongBytes {
		return Song{}, fmt.Errorf("oversized song data")
	}
	d := json.NewDecoder(io.LimitReader(f, maxSongBytes+1))
	d.DisallowUnknownFields()
	// Decode cells as slices first: encoding/json silently discards excess
	// elements when reading directly into a fixed [16] array.
	var stored struct {
		Format  int       `json:"format"`
		BPM     int       `json:"bpm"`
		Octave  int       `json:"octave"`
		Tone    int       `json:"tone"`
		Volume  int       `json:"volume"`
		Pattern [][]Hit   `json:"pattern"`
		Pages   [][][]Hit `json:"pages"`
	}
	if err = d.Decode(&stored); err != nil {
		return Song{}, fmt.Errorf("read song (original preserved): %w", err)
	}
	var extra any
	if err = d.Decode(&extra); err != io.EOF {
		return Song{}, fmt.Errorf("trailing or oversized song data")
	}
	if len(stored.Pattern) > steps || len(stored.Pages) >= maxPages {
		return Song{}, fmt.Errorf("too many pages or cells; original preserved")
	}
	s := Song{Format: stored.Format, BPM: stored.BPM, Octave: stored.Octave, Tone: stored.Tone, Volume: stored.Volume}
	copy(s.Pattern[:], stored.Pattern)
	if stored.Pages != nil {
		s.Pages = make([][steps][]Hit, len(stored.Pages))
	}
	for page, cells := range stored.Pages {
		if len(cells) > steps {
			return Song{}, fmt.Errorf("too many cells in page %d; original preserved", page+2)
		}
		copy(s.Pages[page][:], cells)
	}
	return s, s.validate()
}
func saveSong(path string, s Song) error {
	if err := s.validate(); err != nil {
		return err
	}
	dir := filepath.Dir(path)
	if err := os.MkdirAll(dir, 0700); err != nil {
		return err
	}
	data, err := json.MarshalIndent(s, "", "  ")
	if err != nil {
		return err
	}
	if len(data)+1 > maxSongBytes {
		return fmt.Errorf("song exceeds size limit")
	}
	if s.Format == 2 {
		if err := preserveLegacySong(path); err != nil {
			return err
		}
	}
	f, err := os.CreateTemp(dir, ".song-*")
	if err != nil {
		return err
	}
	tmp := f.Name()
	defer os.Remove(tmp)
	if _, err = f.Write(append(data, '\n')); err == nil {
		err = f.Sync()
	}
	ce := f.Close()
	if err == nil {
		err = ce
	}
	if err != nil {
		return err
	}
	if err = os.Rename(tmp, path); err != nil {
		return err
	}
	return syncDirectory(dir)
}
func demoSong() Song {
	s := defaultSong()
	s.Volume = 30
	melody := []int{60, 64, 67, 72, 71, 67, 64, 62, 60, 64, 69, 72, 71, 67, 64, 62}
	for i, n := range melody {
		s.Pattern[i] = []Hit{{MIDI: n, Tone: 0}}
		if i%4 == 0 {
			s.Pattern[i] = append(s.Pattern[i], Hit{Drum: 1})
		}
		if i%4 == 2 {
			s.Pattern[i] = append(s.Pattern[i], Hit{Drum: 2})
		}
		if i%2 == 1 {
			s.Pattern[i] = append(s.Pattern[i], Hit{Drum: 3})
		}
	}
	return s
}
