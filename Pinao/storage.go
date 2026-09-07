package main

import (
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
)

func loadSong(path string) (Song, error) {
	f, err := os.Open(path)
	if errors.Is(err, os.ErrNotExist) {
		return defaultSong(), nil
	}
	if err != nil {
		return Song{}, err
	}
	defer f.Close()
	d := json.NewDecoder(io.LimitReader(f, 32769))
	d.DisallowUnknownFields()
	var s Song
	if err = d.Decode(&s); err != nil {
		return Song{}, fmt.Errorf("read song (original preserved): %w", err)
	}
	var extra any
	if err = d.Decode(&extra); err != io.EOF {
		return Song{}, fmt.Errorf("trailing or oversized song data")
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
