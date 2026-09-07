package main

import (
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestVolumePersistsAndRestoresActualMixer(t *testing.T) {
	path := filepath.Join(t.TempDir(), "music", "volume.json")
	applied := -1
	apply := func(value int) error { applied = value; return nil }
	volume, err := restoreVolume(path, apply)
	if err != nil || applied != 50 || volume.Value() != 50 {
		t.Fatalf("initial restore: %v, mixer=%d", err, applied)
	}
	if _, err = os.Stat(path); !os.IsNotExist(err) {
		t.Fatal("startup should not write preferences")
	}
	for _, value := range []int{65, 0, 100, 25} {
		if err = volume.Set(value); err != nil {
			t.Fatal(err)
		}
		applied = -1
		reopened, err := restoreVolume(path, apply)
		if err != nil || applied != value || reopened.Value() != value {
			t.Fatalf("restore %d: %v, mixer=%d", value, err, applied)
		}
	}
}

func TestInvalidVolumeSettingsUseDefault(t *testing.T) {
	path := filepath.Join(t.TempDir(), "volume.json")
	for _, data := range []string{"invalid", "{}", `{"volume":null}`, `{"volume":-1}`, `{"volume":101}`, `{"volume":12.5}`, `{"volume":"70"}`, `{"volume":80} trailing`, strings.Repeat(" ", 257)} {
		if err := os.WriteFile(path, []byte(data), 0600); err != nil {
			t.Fatal(err)
		}
		value, saved := loadVolumeSetting(path)
		if value != 50 || saved {
			t.Fatalf("invalid settings accepted: %q", data)
		}
	}
	for _, value := range []int{-1, 101} {
		if !errors.Is(saveVolumeSetting(path, value), os.ErrInvalid) {
			t.Fatal("out-of-range save accepted")
		}
	}
}

func TestVolumeMixerFailurePreservesSavedValue(t *testing.T) {
	path := filepath.Join(t.TempDir(), "volume.json")
	if err := saveVolumeSetting(path, 40); err != nil {
		t.Fatal(err)
	}
	fail := false
	volume, err := restoreVolume(path, func(int) error {
		if fail {
			return errors.New("mixer unavailable")
		}
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	fail = true
	if volume.Set(75) == nil || volume.Value() != 40 {
		t.Fatal("failed mixer changed volume")
	}
	if saved, ok := loadVolumeSetting(path); !ok || saved != 40 {
		t.Fatal("failed adjustment overwrote settings")
	}
	if _, err := restoreVolume(path, func(int) error { return errors.New("mixer unavailable") }); err == nil {
		t.Fatal("restore hid mixer error")
	}
}

func TestVolumeSaveFailureKeepsActualValueAndCanRetry(t *testing.T) {
	root := t.TempDir()
	blocker := filepath.Join(root, "blocked")
	if err := os.WriteFile(blocker, []byte("not a directory"), 0600); err != nil {
		t.Fatal(err)
	}
	calls := 0
	volume, err := restoreVolume(filepath.Join(blocker, "volume.json"), func(int) error { calls++; return nil })
	if err != nil {
		t.Fatal(err)
	}
	if err := volume.Set(70); err == nil || !strings.Contains(err.Error(), "保存失败") {
		t.Fatal("save failure not reported")
	}
	if volume.Value() != 70 {
		t.Fatal("UI no longer matches adjusted mixer")
	}
	if err := os.Remove(blocker); err != nil {
		t.Fatal(err)
	}
	if err := volume.Set(70); err != nil {
		t.Fatal(err)
	}
	if calls != 2 {
		t.Fatal("save retry unnecessarily reapplied mixer")
	}
	if saved, ok := loadVolumeSetting(volume.settingsPath); !ok || saved != 70 {
		t.Fatal("retry did not persist")
	}
}

func TestVolumeLimitsAvoidRepeatedWritesAndPreserveVisualPreference(t *testing.T) {
	root := t.TempDir()
	path := filepath.Join(root, "volume.json")
	visualPath := filepath.Join(root, "display.json")
	if err := saveVisualMode(visualPath, visualRecord); err != nil {
		t.Fatal(err)
	}
	calls := 0
	volume, err := restoreVolume(path, func(int) error { calls++; return nil })
	if err != nil {
		t.Fatal(err)
	}
	for _, value := range []int{-5, 105} {
		if err := volume.Set(value); err != nil {
			t.Fatal(err)
		}
		before, err := os.Stat(path)
		if err != nil {
			t.Fatal(err)
		}
		beforeCalls := calls
		for i := 0; i < 10; i++ {
			if err := volume.Set(value); err != nil {
				t.Fatal(err)
			}
		}
		after, err := os.Stat(path)
		if err != nil {
			t.Fatal(err)
		}
		if calls != beforeCalls || !os.SameFile(before, after) {
			t.Fatal("held key rewrites unchanged volume")
		}
		if volume.Value() != clampVolume(value) {
			t.Fatal("volume limit failed")
		}
	}
	if loadVisualMode(visualPath) != visualRecord {
		t.Fatal("volume update lost visual mode")
	}
	if err := saveVisualMode(visualPath, visualBars); err != nil {
		t.Fatal(err)
	}
	if saved, ok := loadVolumeSetting(path); !ok || saved != 100 {
		t.Fatal("visual update lost saved volume")
	}
}
