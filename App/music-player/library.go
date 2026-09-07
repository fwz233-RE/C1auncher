package main

import (
	"io/fs"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"unicode/utf8"
)

type Track struct {
	Path  string
	Title string
}

var supportedTrackExtensions = map[string]struct{}{
	".flac": {},
	".mp3":  {},
}

func isSupportedTrack(path string) bool {
	_, ok := supportedTrackExtensions[strings.ToLower(filepath.Ext(path))]
	return ok
}

func scanLibrary(root string) ([]Track, error) {
	tracks := make([]Track, 0)
	err := filepath.WalkDir(root, func(path string, entry fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if entry.IsDir() || !isSupportedTrack(entry.Name()) {
			return nil
		}
		tracks = append(tracks, Track{
			Path:  path,
			Title: trackDisplayName(path, entry.Name()),
		})
		return nil
	})
	if err != nil {
		return nil, err
	}
	sort.SliceStable(tracks, func(i, j int) bool {
		left, right := strings.ToLower(tracks[i].Path), strings.ToLower(tracks[j].Path)
		if left == right {
			return tracks[i].Path < tracks[j].Path
		}
		return left < right
	})
	return tracks, nil
}

func trackDisplayName(path, fallback string) string {
	name := fallback
	if data, err := os.ReadFile(path + ".name"); err == nil && len(data) > 0 && len(data) <= 4096 && utf8.Valid(data) {
		if candidate := strings.TrimSpace(string(data)); candidate != "" && !strings.ContainsAny(candidate, "\x00\r\n") {
			name = candidate
		}
	}
	return strings.TrimSuffix(name, filepath.Ext(name))
}
