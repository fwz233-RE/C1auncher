package main

import (
	"encoding/json"
	"io"
	"os"
	"path/filepath"
)

const defaultVolume = 50

// A pointer distinguishes a valid muted value (0) from a missing/null field.
func loadVolumeSetting(path string) (int, bool) {
	file, err := os.Open(path)
	if err != nil {
		return defaultVolume, false
	}
	defer file.Close()
	data, err := io.ReadAll(io.LimitReader(file, 257))
	if err != nil || len(data) > 256 {
		return defaultVolume, false
	}
	var settings struct {
		Volume *int `json:"volume"`
	}
	if json.Unmarshal(data, &settings) != nil || settings.Volume == nil || *settings.Volume < 0 || *settings.Volume > 100 {
		return defaultVolume, false
	}
	return *settings.Volume, true
}

func saveVolumeSetting(path string, value int) error {
	if value < 0 || value > 100 {
		return os.ErrInvalid
	}
	if err := os.MkdirAll(filepath.Dir(path), 0700); err != nil {
		return err
	}
	temporary, err := os.CreateTemp(filepath.Dir(path), ".volume-*.tmp")
	if err != nil {
		return err
	}
	defer os.Remove(temporary.Name())
	err = json.NewEncoder(temporary).Encode(struct {
		Volume int `json:"volume"`
	}{value})
	if err == nil {
		err = temporary.Sync()
	}
	if closeErr := temporary.Close(); err == nil {
		err = closeErr
	}
	if err != nil {
		return err
	}
	return replaceArtworkCache(temporary.Name(), path)
}
