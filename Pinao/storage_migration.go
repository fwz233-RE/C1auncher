package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
)

// preserveLegacySong keeps every legacy revision before replacing path with a
// format-2 project. The first revision uses the historical, stable backup
// name; later revisions use a content-addressed name so that the historical
// backup is never changed or overwritten.
func preserveLegacySong(path string) error {
	data, err := readMigrationFile(path)
	if errors.Is(err, os.ErrNotExist) {
		return nil
	}
	if err != nil {
		return err
	}
	var header struct {
		Format int `json:"format"`
	}
	if err := json.Unmarshal(data, &header); err != nil {
		return fmt.Errorf("existing song invalid; original preserved: %w", err)
	}
	if header.Format == 2 || header.Format == 3 {
		return nil
	}
	if header.Format != 1 {
		return fmt.Errorf("unknown original format; refusing replacement")
	}

	primary := path + ".v1-backup.json"
	if _, err := os.Stat(primary); err == nil {
		// The primary backup is immutable. A different legacy revision gets a
		// deterministic secondary name derived from its exact bytes.
		if info, statErr := os.Stat(primary); statErr != nil {
			return statErr
		} else if info.IsDir() {
			return fmt.Errorf("legacy backup is not a file; refusing replacement")
		}
		h := sha256.Sum256(data)
		secondary := path + ".v1-backup-" + hex.EncodeToString(h[:]) + ".json"
		if err := createMigrationBackup(secondary, data); err != nil {
			return fmt.Errorf("preserve legacy revision: %w", err)
		}
		return syncDirectory(filepath.Dir(path))
	} else if !errors.Is(err, os.ErrNotExist) {
		return err
	}
	if err := createMigrationBackup(primary, data); err != nil {
		return fmt.Errorf("preserve single-page backup: %w", err)
	}
	return syncDirectory(filepath.Dir(path))
}

// readMigrationFile bounds both normal files and special files. The extra byte
// distinguishes an exactly-full file from one that exceeds the storage limit.
func readMigrationFile(path string) ([]byte, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	data, readErr := io.ReadAll(io.LimitReader(f, maxSongBytes+1))
	closeErr := f.Close()
	if readErr != nil {
		return nil, readErr
	}
	if closeErr != nil {
		return nil, closeErr
	}
	if len(data) > maxSongBytes {
		return nil, fmt.Errorf("existing song too large; original preserved")
	}
	return data, nil
}

// createMigrationBackup never overwrites. If another process won the race,
// its bytes must match exactly; otherwise the collision is a hard failure.
func createMigrationBackup(path string, data []byte) error {
	f, err := os.OpenFile(path, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0600)
	if err == nil {
		if _, err = f.Write(data); err == nil {
			err = f.Sync()
		}
		closeErr := f.Close()
		if err == nil {
			err = closeErr
		}
		if err != nil {
			_ = os.Remove(path)
		}
		return err
	}
	if !errors.Is(err, os.ErrExist) {
		return err
	}
	existing, readErr := readMigrationFile(path)
	if readErr != nil {
		return readErr
	}
	if !bytes.Equal(existing, data) {
		return fmt.Errorf("legacy backup hash collision; refusing replacement")
	}
	return nil
}
