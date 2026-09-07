package main

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
)

const layoutVersion = 3

type BookFingerprint struct {
	Size    int64 `json:"size"`
	ModTime int64 `json:"mtime"`
}

type Progress struct {
	Fingerprint   BookFingerprint `json:"fingerprint"`
	Path          string          `json:"path"`
	Chapter       int             `json:"chapter"`
	Offset        int64           `json:"offset"`
	LayoutVersion int             `json:"layoutVersion"`
}

type ProgressStore struct{ Dir string }

func fingerprint(path string) (BookFingerprint, error) {
	info, err := os.Stat(path)
	if err != nil {
		return BookFingerprint{}, err
	}
	return BookFingerprint{Size: info.Size(), ModTime: info.ModTime().UnixNano()}, nil
}

func (store ProgressStore) progressPath(bookPath string) string {
	digest := sha256.Sum256([]byte(filepath.Clean(bookPath)))
	return filepath.Join(store.Dir, hex.EncodeToString(digest[:12])+".json")
}

func (store ProgressStore) Save(progress Progress) error {
	if err := os.MkdirAll(store.Dir, 0o755); err != nil {
		return err
	}
	progress.LayoutVersion = layoutVersion
	data, err := json.MarshalIndent(progress, "", "  ")
	if err != nil {
		return err
	}
	path := store.progressPath(progress.Path)
	temporary, err := os.CreateTemp(store.Dir, ".progress-*.tmp")
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
		return fmt.Errorf("replace progress: %w", err)
	}
	return nil
}

func (store ProgressStore) Load(bookPath string) (Progress, bool, error) {
	data, err := os.ReadFile(store.progressPath(bookPath))
	if os.IsNotExist(err) {
		return Progress{}, false, nil
	}
	if err != nil {
		return Progress{}, false, err
	}
	var progress Progress
	if err := json.Unmarshal(data, &progress); err != nil {
		return Progress{}, false, err
	}
	current, err := fingerprint(bookPath)
	if err != nil {
		return Progress{}, false, err
	}
	if progress.Path != bookPath || progress.Fingerprint != current || progress.LayoutVersion != layoutVersion {
		return Progress{}, false, nil
	}
	return progress, true, nil
}
