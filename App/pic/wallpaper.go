package main

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"

	"c1device"
)

// Write in the canonical wallpaper directory, not beside a selected image in
// Pictures or a nested album. The old wallpaper survives every pre-rename error.
func saveWallpaper(directory string, frame c1device.Frame) error {
	info, err := os.Lstat(directory)
	if err != nil {
		return err
	}
	if !info.IsDir() || info.Mode()&os.ModeSymlink != 0 {
		return errors.New("壁纸目录无效")
	}
	target := filepath.Join(directory, "wallpaper.raw")
	if existing, err := os.Lstat(target); err == nil {
		if !existing.Mode().IsRegular() {
			return errors.New("壁纸目标不是普通文件")
		}
	} else if !os.IsNotExist(err) {
		return err
	}
	file, err := os.CreateTemp(directory, ".wallpaper-*.tmp")
	if err != nil {
		return err
	}
	name := file.Name()
	defer os.Remove(name)
	defer file.Close()
	if err = file.Chmod(0644); err != nil {
		return err
	}
	if _, err = file.Write(frame[:]); err != nil {
		return err
	}
	if err = file.Sync(); err != nil {
		return err
	}
	if err = file.Close(); err != nil {
		return err
	}
	if err = os.Rename(name, target); err != nil {
		return err
	}
	if err = syncWallpaperDirectory(directory); err != nil {
		return fmt.Errorf("壁纸已替换，但目录同步失败: %w", err)
	}
	return nil
}
