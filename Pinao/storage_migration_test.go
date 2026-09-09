package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"testing"
)

func TestLegacyMigrationPreservesRepeatedRevisions(t *testing.T) {
	path := filepath.Join(t.TempDir(), "song.json")
	legacy1 := demoSong()
	if err := saveSong(path, legacy1); err != nil {
		t.Fatal(err)
	}
	original := append([]byte(nil), mpRead(t, path)...)
	if err := saveSong(path, mpSong(2)); err != nil {
		t.Fatal("first migration:", err)
	}
	legacy2 := demoSong()
	legacy2.Volume = 31
	if err := saveSong(path, legacy2); err != nil {
		t.Fatal("simulated reset:", err)
	}
	currentLegacy := append([]byte(nil), mpRead(t, path)...)
	if err := saveSong(path, mpSong(3)); err != nil {
		t.Fatal("repeated migration:", err)
	}
	if !bytes.Equal(mpRead(t, path), mustJSONSong(t, mpSong(3))) {
		t.Fatal("current format-2 song was not saved safely")
	}
	primary := path + ".v1-backup.json"
	if !bytes.Equal(mpRead(t, primary), original) {
		t.Fatal("original primary backup changed")
	}
	h := sha256.Sum256(currentLegacy)
	secondary := path + ".v1-backup-" + hex.EncodeToString(h[:]) + ".json"
	if !bytes.Equal(mpRead(t, secondary), currentLegacy) {
		t.Fatal("current legacy revision was not preserved byte-for-byte")
	}
}

func TestLegacyMigrationHashBackupCollisionRefusesReplacement(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "song.json")
	legacy := demoSong()
	if err := saveSong(path, legacy); err != nil {
		t.Fatal(err)
	}
	if err := saveSong(path, mpSong(2)); err != nil {
		t.Fatal(err)
	}
	legacy.Volume++
	if err := saveSong(path, legacy); err != nil {
		t.Fatal(err)
	}
	data := mpRead(t, path)
	h := sha256.Sum256(data)
	collision := path + ".v1-backup-" + hex.EncodeToString(h[:]) + ".json"
	if err := os.WriteFile(collision, []byte("wrong"), 0600); err != nil {
		t.Fatal(err)
	}
	before := append([]byte(nil), data...)
	if err := saveSong(path, mpSong(3)); err == nil {
		t.Fatal("mismatched content-addressed backup was accepted")
	}
	if !bytes.Equal(mpRead(t, path), before) {
		t.Fatal("collision refusal changed current legacy song")
	}
	if _, err := os.Stat(collision); errors.Is(err, os.ErrNotExist) {
		t.Fatal("collision backup was removed")
	}
}

func mustJSONSong(t *testing.T, s Song) []byte {
	t.Helper()
	data, err := json.MarshalIndent(s, "", "  ")
	if err != nil {
		t.Fatal(err)
	}
	return append(data, '\n')
}
