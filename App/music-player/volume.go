package main

import (
	"fmt"
	"sync"
)

type Volume struct {
	mu           sync.Mutex
	value        int
	savedValue   int
	settingsPath string
	apply        func(int) error
}

func newVolume(initial int) *Volume {
	return &Volume{value: clampVolume(initial), savedValue: -1, apply: applyVolume}
}

// Restore the actual mixer before displaying the saved value. Loading does not
// rewrite preferences; an invalid or absent file uses the original default.
func restoreVolume(path string, apply func(int) error) (*Volume, error) {
	initial, saved := loadVolumeSetting(path)
	volume := newVolume(initial)
	volume.settingsPath, volume.apply = path, apply
	if err := apply(initial); err != nil {
		return nil, fmt.Errorf("恢复音量失败: %w", err)
	}
	if saved {
		volume.savedValue = initial
	}
	return volume, nil
}

func (volume *Volume) Value() int {
	volume.mu.Lock()
	defer volume.mu.Unlock()
	return volume.value
}

func (volume *Volume) Set(value int) error {
	volume.mu.Lock()
	defer volume.mu.Unlock()
	value = clampVolume(value)
	if value != volume.value {
		if err := volume.apply(value); err != nil {
			return err
		}
		volume.value = value
	}
	// At a limit, held volume keys must not repeatedly write flash. Failed saves
	// remain retryable without applying the already successful mixer change again.
	if volume.settingsPath != "" && value != volume.savedValue {
		if err := saveVolumeSetting(volume.settingsPath, value); err != nil {
			return fmt.Errorf("音量已调，保存失败: %w", err)
		}
		volume.savedValue = value
	}
	return nil
}

func clampVolume(value int) int {
	if value < 0 {
		return 0
	}
	if value > 100 {
		return 100
	}
	return value
}
