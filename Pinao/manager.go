package main

import (
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"
)

type archiveEntry struct {
	Name, Path string
	IsWAV      bool
	Size       int64
}

// listManagedFiles returns saved song archives and exported WAV files.
func listManagedFiles(songPath string) ([]archiveEntry, error) {
	dir := filepath.Dir(songPath)
	entries := make([]archiveEntry, 0)
	add := func(root string, wav bool) error {
		files, err := os.ReadDir(root)
		if os.IsNotExist(err) {
			return nil
		}
		if err != nil {
			return err
		}
		for _, e := range files {
			if e.IsDir() {
				continue
			}
			if wav && strings.ToLower(filepath.Ext(e.Name())) != ".wav" {
				continue
			}
			if !wav && filepath.Ext(e.Name()) != ".json" {
				continue
			}
			info, err := e.Info()
			if err != nil {
				return err
			}
			entries = append(entries, archiveEntry{Name: e.Name(), Path: filepath.Join(root, e.Name()), IsWAV: wav, Size: info.Size()})
		}
		return nil
	}
	if err := add(dir, false); err != nil {
		return nil, err
	}
	if err := add(filepath.Join(dir, "exports"), true); err != nil {
		return nil, err
	}
	sort.Slice(entries, func(i, j int) bool {
		if entries[i].IsWAV != entries[j].IsWAV {
			return !entries[i].IsWAV
		}
		return entries[i].Name < entries[j].Name
	})
	return entries, nil
}

func archivePath(songPath, name string) (string, error) {
	name = strings.TrimSpace(name)
	if name == "" {
		return "", fmt.Errorf("empty archive name")
	}
	name = strings.TrimSuffix(name, ".json")
	if filepath.Base(name) != name || strings.ContainsAny(name, `/\\`) {
		return "", fmt.Errorf("invalid archive name")
	}
	return filepath.Join(filepath.Dir(songPath), name+".json"), nil
}

func createArchive(songPath, name string) (string, error) {
	p, e := archivePath(songPath, name)
	if e != nil {
		return "", e
	}
	if _, e = os.Stat(p); e == nil {
		return "", fmt.Errorf("archive already exists")
	}
	if e := saveSong(p, defaultSong()); e != nil {
		return "", e
	}
	return p, nil
}
func deleteManagedFile(path string) error {
	if filepath.Ext(path) == ".json" && filepath.Base(path) == "song.json" {
		return fmt.Errorf("cannot delete active archive")
	}
	return os.Remove(path)
}
func archiveDisplayName(path string) string {
	return strings.TrimSuffix(filepath.Base(path), filepath.Ext(path))
}
func archiveStamp() string { return fmt.Sprintf("song-%s", time.Now().Format("20060102-150405")) }
