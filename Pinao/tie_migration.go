package main

import (
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"os"
	"path/filepath"
)

// Preserve the precise pre-tie project, including multipage songs. Old builds
// reject format 3 rather than silently turning continuations into repeated notes.
func preservePreTieSong(path string) error {
	data, err := readMigrationFile(path)
	if errors.Is(err, os.ErrNotExist) {
		return nil
	}
	if err != nil {
		return err
	}
	old, err := loadSong(path)
	if err != nil {
		return err
	}
	if old.Format == 3 {
		return nil
	}
	h := sha256.Sum256(data)
	backup := path + ".pre-ties-" + hex.EncodeToString(h[:]) + ".json"
	if err := createMigrationBackup(backup, data); err != nil {
		return err
	}
	return syncDirectory(filepath.Dir(path))
}
