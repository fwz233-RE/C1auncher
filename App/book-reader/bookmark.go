package main

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"sort"
)

const bookmarkFormatVersion = 1

type Bookmark struct {
	Chapter int   `json:"chapter"`
	Offset  int64 `json:"offset"`
}

type bookmarkFile struct {
	Version     int             `json:"version"`
	Path        string          `json:"path"`
	Fingerprint BookFingerprint `json:"fingerprint"`
	Bookmarks   []Bookmark      `json:"bookmarks"`
}

type BookmarkStore struct{ Dir string }

func (store BookmarkStore) bookmarkPath(bookPath string) string {
	digest := sha256.Sum256([]byte(filepath.Clean(bookPath)))
	return filepath.Join(store.Dir, hex.EncodeToString(digest[:12])+".bookmarks.json")
}

func (store BookmarkStore) Load(bookPath string) ([]Bookmark, error) {
	path := store.bookmarkPath(bookPath)
	data, err := os.ReadFile(path)
	if os.IsNotExist(err) {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	var saved bookmarkFile
	if err := json.Unmarshal(data, &saved); err != nil {
		_ = os.Remove(path)
		return nil, nil
	}
	current, err := fingerprint(bookPath)
	if err != nil {
		return nil, err
	}
	if saved.Version != bookmarkFormatVersion || saved.Path != bookPath || saved.Fingerprint != current {
		_ = os.Remove(path)
		return nil, nil
	}
	return normalizeBookmarks(saved.Bookmarks), nil
}

func (store BookmarkStore) Save(bookPath string, bookmarks []Bookmark) error {
	path := store.bookmarkPath(bookPath)
	if len(bookmarks) == 0 {
		if err := os.Remove(path); err != nil && !os.IsNotExist(err) {
			return err
		}
		return nil
	}
	mark, err := fingerprint(bookPath)
	if err != nil {
		return err
	}
	data, err := json.MarshalIndent(bookmarkFile{
		Version:     bookmarkFormatVersion,
		Path:        bookPath,
		Fingerprint: mark,
		Bookmarks:   normalizeBookmarks(bookmarks),
	}, "", "  ")
	if err != nil {
		return err
	}
	if err := os.MkdirAll(store.Dir, 0o755); err != nil {
		return err
	}
	temporary, err := os.CreateTemp(store.Dir, ".bookmarks-*.tmp")
	if err != nil {
		return err
	}
	temporaryPath := temporary.Name()
	defer os.Remove(temporaryPath)
	if _, err = temporary.Write(data); err == nil {
		err = temporary.Sync()
	}
	if closeErr := temporary.Close(); err == nil {
		err = closeErr
	}
	if err != nil {
		return err
	}
	if err := os.Rename(temporaryPath, path); err != nil {
		return fmt.Errorf("replace bookmarks: %w", err)
	}
	return nil
}

func normalizeBookmarks(bookmarks []Bookmark) []Bookmark {
	normalized := append([]Bookmark(nil), bookmarks...)
	sort.Slice(normalized, func(left, right int) bool {
		if normalized[left].Offset == normalized[right].Offset {
			return normalized[left].Chapter < normalized[right].Chapter
		}
		return normalized[left].Offset < normalized[right].Offset
	})
	result := normalized[:0]
	for _, bookmark := range normalized {
		if bookmark.Chapter < 0 || bookmark.Offset < 0 {
			continue
		}
		if len(result) > 0 && result[len(result)-1] == bookmark {
			continue
		}
		result = append(result, bookmark)
	}
	return result
}
