package main

import (
	"image"
	"image/color"
	"os"
	"strings"
	"testing"
	"time"

	"c1device"
	"golang.org/x/image/font/gofont/goregular"
)

func uiTestFaces(t testing.TB) (*c1device.Face, *c1device.Face) {
	t.Helper()
	data := goregular.TTF
	if path := os.Getenv("C1_FONT_PATH"); path != "" {
		var err error
		data, err = os.ReadFile(path)
		if err != nil {
			t.Fatal(err)
		}
	}
	typeface, err := c1device.ParseTypeface(data)
	if err != nil {
		t.Fatal(err)
	}
	face, err := typeface.NewFace(musicTitleFontSize)
	if err != nil {
		t.Fatal(err)
	}
	small, err := typeface.NewFace(musicLabelFontSize)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { face.Close(); small.Close() })
	return face, small
}

func frameBlack(frame c1device.Frame, x, y int) bool {
	return frame[(y/8)*c1device.DisplayWidth+x]&(0x80>>(y&7)) != 0
}

func TestFooterNeverOverlaysArtwork(t *testing.T) {
	face, small := uiTestFaces(t)
	cover := image.NewGray(image.Rect(0, 0, 98, 98))
	for y := 0; y < 98; y++ {
		for x := 0; x < 98; x++ {
			if (x+y)%2 == 0 {
				cover.SetGray(x, y, color.Gray{Y: 255})
			}
		}
	}
	for _, notice := range []string{"", "播放失败：请检查音乐文件", strings.Repeat("error", 50)} {
		state := appState{current: -1, cover: cover, notice: notice, volume: 100}
		frame := renderMusic(state, face, small)
		for y := 0; y < 98; y++ {
			for x := 0; x < 98; x++ {
				if frameBlack(frame, x+musicCoverRect.Min.X, y+musicCoverRect.Min.Y) != ((x+y)%2 != 0) {
					t.Fatalf("cover pixel overwritten at %d,%d with notice %q", x, y, notice)
				}
			}
		}
	}
	if musicCoverRect.Overlaps(musicFooterRect) {
		t.Fatal("footer overlaps cover")
	}
}

func TestFooterAndTitleFitPanel(t *testing.T) {
	face, small := uiTestFaces(t)
	if small.Measure(controlHint(appState{})) > 234 {
		t.Fatal("footer overlaps volume")
	}
	if !strings.Contains(controlHint(appState{}), "V") {
		t.Fatal("visual control undiscoverable")
	}
	for _, text := range []string{"夏日的风", strings.Repeat("很长的中文歌曲名", 20), strings.Repeat("Long Album Title ", 20), "", "a\nb\tc"} {
		lines := titleLines(face, text, musicTitleWidth)
		if len(lines) > 2 {
			t.Fatal("more than two title lines")
		}
		for _, line := range lines {
			if face.Measure(line) > musicTitleWidth {
				t.Fatalf("title too wide: %q", line)
			}
		}
	}
	for _, forbidden := range []string{"↑↓选曲", "←→切歌"} {
		if strings.Contains(controlHint(appState{}), forbidden) {
			t.Fatal("obsolete footer text")
		}
	}
}

func TestUIStatesHaveWhiteMarginsAndFooterGap(t *testing.T) {
	face, small := uiTestFaces(t)
	for _, visual := range []visualMode{visualStatic, visualBars, visualRecord} {
		for _, count := range []int{0, 1, 9999} {
			tracks := make([]Track, count)
			for i := range tracks {
				tracks[i] = Track{Title: strings.Repeat("Song 歌曲", 10), Path: "a.flac"}
			}
			state := appState{tracks: tracks, current: count - 1, playing: true, visual: visual, position: 123 * time.Minute, duration: 180 * time.Minute, volume: 100}
			frame := renderMusic(state, face, small)
			for y := 0; y < c1device.DisplayHeight; y++ {
				for _, x := range []int{0, 1, 294, 295} {
					if frameBlack(frame, x, y) {
						t.Fatalf("clipped margin at %d,%d", x, y)
					}
				}
			}
			for y := 126; y < 128; y++ {
				for x := 0; x < 296; x++ {
					if frameBlack(frame, x, y) {
						t.Fatalf("content escaped into footer gap at %d,%d", x, y)
					}
				}
			}
			for x := 0; x < 296; x++ {
				if frameBlack(frame, x, 151) {
					t.Fatal("text clipped at panel bottom")
				}
			}
		}
	}
}

func TestPlaybackTimeClampsAndUnknownDuration(t *testing.T) {
	state := appState{position: 5 * time.Minute, duration: 3 * time.Minute}
	if elapsedTimeLabel(state) != "03:00" {
		t.Fatal("elapsed exceeds duration")
	}
	if durationLabel(appState{}) != "--:--" {
		t.Fatal("unknown duration looks like zero")
	}
}
