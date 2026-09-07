package main

import (
	"image"
	"image/color"
	"image/png"
	"os"
	"path/filepath"
	"testing"

	"c1device"
)

func TestImmersiveContainsOnlyWallpaperFrame(t *testing.T) {
	var frame c1device.Frame
	for i := range frame {
		frame[i] = byte(i*11 + 71)
	}
	picture := imageFromFrame(frame)
	got := renderPicture(pictureState{current: picture, immersive: true}, nil, nil)
	if got != frame {
		t.Fatal("immersive frame contains UI or alters raw pixels")
	}
}

func TestRenderWithProductionFont(t *testing.T) {
	fontPath := os.Getenv("C1_PIC_TEST_FONT")
	if fontPath == "" {
		t.Skip("set C1_PIC_TEST_FONT to exercise real Chinese font")
	}
	data, err := os.ReadFile(fontPath)
	if err != nil {
		t.Fatal(err)
	}
	typeface, err := c1device.ParseTypeface(data)
	if err != nil {
		t.Fatal(err)
	}
	face, err := typeface.NewFace(16)
	if err != nil {
		t.Fatal(err)
	}
	defer face.Close()
	picture := image.NewGray(image.Rect(0, 0, 296, 152))
	for y := 0; y < 152; y++ {
		for x := 0; x < 296; x++ {
			picture.SetGray(x, y, color.Gray{Y: uint8(x * 255 / 295)})
		}
	}
	state := pictureState{pictures: []Picture{{Name: "示例图片.png"}}, selected: 0, current: picture}
	for _, mode := range []string{"preview", "immersive", "confirm", "saved", "loading"} {
		state.immersive = mode != "preview" && mode != "loading"
		state.confirmWallpaper = mode == "confirm"
		state.notice = ""
		state.loading = false
		state.current = picture
		if mode == "saved" {
			state.notice = "壁纸已设置，下次锁屏生效"
		}
		if mode == "loading" {
			state.loading = true
			state.current = nil
		}
		frame := renderPicture(state, face, face)
		if mode == "immersive" && frame != renderImmersive(picture) {
			t.Fatal("immersive and saved image disagree")
		}
		if output := os.Getenv("C1_PIC_PREVIEW_DIR"); output != "" {
			if err := os.MkdirAll(output, 0755); err != nil {
				t.Fatal(err)
			}
			f, err := os.Create(filepath.Join(output, mode+".png"))
			if err != nil {
				t.Fatal(err)
			}
			if err := png.Encode(f, imageFromFrame(frame)); err != nil {
				f.Close()
				t.Fatal(err)
			}
			if err := f.Close(); err != nil {
				t.Fatal(err)
			}
		}
	}
	if face.Measure("方向切图 OK全屏 P壁纸 R刷新") > 280 {
		t.Fatal("footer exceeds screen")
	}
	if face.Measure("将全屏画面覆盖为锁屏壁纸？") > 276 {
		t.Fatal("confirmation exceeds screen")
	}
	t.Logf("production-font UI rendering peak RSS: %d KiB", peakRSSKB())
}
