package main

import (
	"bytes"
	"image"
	"image/png"
	"os"
	"path/filepath"
	"testing"
)

// Browsing any ordinary picture must never set or overwrite the lock screen.
func TestPreviewDoesNotChangeWallpaper(t *testing.T) {
	for _, existing := range []bool{false, true} {
		name := "missing"
		if existing {
			name = "existing"
		}
		t.Run(name, func(t *testing.T) {
			root := t.TempDir()
			dir := filepath.Join(root, "pic")
			if err := os.MkdirAll(dir, 0755); err != nil {
				t.Fatal(err)
			}
			wallpaper := filepath.Join(dir, "wallpaper.raw")
			original := bytes.Repeat([]byte{0x55}, 5624)
			if existing {
				if err := os.WriteFile(wallpaper, original, 0644); err != nil {
					t.Fatal(err)
				}
			}
			for _, filename := range []string{"sample.png", "wallpaper.png"} {
				path := filepath.Join(dir, filename)
				var encoded bytes.Buffer
				if err := png.Encode(&encoded, image.NewGray(image.Rect(0, 0, 10, 10))); err != nil {
					t.Fatal(err)
				}
				if err := os.WriteFile(path, encoded.Bytes(), 0644); err != nil {
					t.Fatal(err)
				}
				state := pictureState{pictures: []Picture{{Path: path, Name: filename}}, selected: 0, picturesDir: dir}
				state.loadSelected()
				if state.current == nil {
					t.Fatal("picture preview failed")
				}
				data, err := os.ReadFile(wallpaper)
				if existing {
					if err != nil || !bytes.Equal(data, original) {
						t.Fatal("preview changed the wallpaper")
					}
				} else if !os.IsNotExist(err) {
					t.Fatal("preview created a wallpaper")
				}
			}
		})
	}
}
