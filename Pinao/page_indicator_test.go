package main

import (
	"crypto/sha256"
	"fmt"
	"image"
	"os"
	"path/filepath"
	"testing"
)

// Keep these literal bounds independent of the renderer: the rest of the
// existing 296x152 interface is not available to the page tabs.
var pageIndicatorRegion = image.Rect(217, 3, 288, 20)

func pageIndicatorFixture() viewState {
	return viewState{
		Tone: 0, BPM: 120, Octave: 4, Volume: 8,
		Recording: true, StepMode: true, Step: 8,
		Pattern: 0x9189, Keys: 0x15, Notes: 0x1015, Drums: 5,
		Footer: "ENTER SAVE  L HELP",
	}
}

func pagePixelBlack(f *frame, x, y int) bool {
	return f[(y/8)*width+x]&(0x80>>uint(y&7)) != 0
}

func TestPageIndicatorLegacyBaseline(t *testing.T) {
	f := renderView(pageIndicatorFixture())
	// The unused header area was solid black in the original renderer. Restore
	// just that area and compare every packed pixel with its pre-change digest.
	f.box(217, 3, 71, 17, true)
	const want = "b521bd8200532a175253e2e2cdbb3b0eb6605c932be73945267cb167568b4651"
	if got := fmt.Sprintf("%x", sha256.Sum256(f[:])); got != want {
		t.Fatalf("original UI pixels changed outside page indicator: %s", got)
	}
}

func TestPageIndicatorTabs(t *testing.T) {
	tests := []struct {
		name        string
		page, count int
		labels      [3]string // previous, current, next; empty means no tab
	}{
		{"default", 0, 0, [3]string{"", "1", ""}},
		{"single", 0, 1, [3]string{"", "1", ""}},
		{"first", 0, 3, [3]string{"", "1", "2"}},
		{"middle", 1, 3, [3]string{"1", "2", "3"}},
		{"last", 2, 3, [3]string{"2", "3", ""}},
		{"digit-transition", 8, 64, [3]string{"8", "9", "10"}},
		{"two-digit", 9, 64, [3]string{"9", "10", "11"}},
		{"near-limit", 62, 64, [3]string{"62", "63", "64"}},
		{"limit", 63, 64, [3]string{"63", "64", ""}},
		{"negative-page", -1, 3, [3]string{"", "1", "2"}},
		{"past-last-page", 64, 64, [3]string{"63", "64", ""}},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			v := pageIndicatorFixture()
			v.Page, v.PageCount = tt.page, tt.count
			got := renderView(v)
			var want frame
			want.box(0, 0, width, 22, true)
			for i, label := range tt.labels {
				if label == "" {
					continue
				}
				bounds := [3]image.Rectangle{
					image.Rect(217, 6, 236, 17),
					image.Rect(241, 3, 264, 20),
					image.Rect(269, 6, 288, 17),
				}[i]
				for y := bounds.Min.Y; y < bounds.Max.Y; y++ {
					for x := bounds.Min.X; x < bounds.Max.X; x++ {
						edge := x == bounds.Min.X || x == bounds.Max.X-1 || y == bounds.Min.Y || y == bounds.Max.Y-1
						want.pixel(x, y, i != 1 && !edge)
					}
				}
				x := bounds.Min.X + (bounds.Dx()-(len(label)*6-1))/2
				y := bounds.Min.Y + (bounds.Dy()-7)/2
				want.text(x, y, label, 1, i == 1)
			}
			for y := 0; y < 22; y++ {
				for x := 200; x < width; x++ {
					if pagePixelBlack(&got, x, y) != pagePixelBlack(&want, x, y) {
						t.Fatalf("incorrect tab pixel at (%d,%d), labels %q", x, y, tt.labels)
					}
				}
			}
		})
	}
}

func TestPageIndicatorChangesStayInsideHeader(t *testing.T) {
	for _, help := range []bool{false, true} {
		for tone := range toneNames {
			v := pageIndicatorFixture()
			v.Help, v.Tone = help, tone
			baseline := renderView(v)
			v.PageCount = 1
			if got := renderView(v); got != baseline {
				t.Fatal("zero page count differs from explicit single page")
			}
			baseline.box(pageIndicatorRegion.Min.X, pageIndicatorRegion.Min.Y, pageIndicatorRegion.Dx(), pageIndicatorRegion.Dy(), true)
			for count := 1; count <= 64; count++ {
				v.PageCount = count
				var previous frame
				for page := 0; page < count; page++ {
					v.Page = page
					got := renderView(v)
					if page > 0 && got == previous {
						t.Fatalf("page %d/%d did not visibly change", page+1, count)
					}
					previous = got
					got.box(pageIndicatorRegion.Min.X, pageIndicatorRegion.Min.Y, pageIndicatorRegion.Dx(), pageIndicatorRegion.Dy(), true)
					if got != baseline {
						t.Fatalf("page %d/%d changed pixels outside header, help=%v tone=%d", page+1, count, help, tone)
					}
				}
			}
		}
	}
}

func TestPageIndicatorHelp(t *testing.T) {
	v := pageIndicatorFixture()
	v.Help = true
	got := renderView(v)
	var want frame
	lines := []string{
		"PLAY: A W S E D F T G Y H U J K",
		"CVBN: KICK/SNARE/HAT/CLAP  Q: TONE",
		"Z/X: OCTAVE  R: RECORD  SPACE: LOOP",
		"LEFT/RIGHT: STEP  UP/DOWN: BPM",
		"I/O: TAP PAGE / HOLD INSERT",
		"VOL KEYS: VOLUME  ENTER: SAVE  P: WAV",
		"DEL X2: CLEAR  HOME/BACK: EXIT",
		"8 BEATS/16 STEPS  L: CLOSE HELP",
	}
	for i, line := range lines {
		if 7+len(line)*6-1 > width || 29+i*15+7 > height {
			t.Fatalf("help line %d exceeds original layout", i)
		}
		want.text(7, 29+i*15, line, 1, true)
	}
	for y := 24; y < height; y++ {
		for x := 0; x < width; x++ {
			if pagePixelBlack(&got, x, y) != pagePixelBlack(&want, x, y) {
				t.Fatalf("help text differs at (%d,%d)", x, y)
			}
		}
	}
}

func TestPageIndicatorPreview(t *testing.T) {
	dir := os.Getenv("PINAO_PAGE_PREVIEW_DIR")
	if dir == "" {
		t.Skip("set PINAO_PAGE_PREVIEW_DIR to generate page indicator PNGs")
	}
	if err := os.MkdirAll(dir, 0755); err != nil {
		t.Fatal(err)
	}
	for _, tt := range []struct {
		name        string
		page, count int
		help        bool
	}{
		{"page-default", 0, 0, false},
		{"page-single", 0, 1, false},
		{"page-first", 0, 3, false},
		{"page-middle", 1, 3, false},
		{"page-last", 2, 3, false},
		{"page-two-digit", 9, 64, false},
		{"page-63", 62, 64, false},
		{"page-64", 63, 64, false},
		{"page-help", 9, 64, true},
	} {
		v := pageIndicatorFixture()
		v.Page, v.PageCount, v.Help = tt.page, tt.count, tt.help
		path := filepath.Join(dir, tt.name+".png")
		out, err := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0600)
		if err != nil {
			t.Fatal(err)
		}
		err = writePNG(out, renderView(v), 3)
		closeErr := out.Close()
		if err != nil {
			t.Fatal(err)
		}
		if closeErr != nil {
			t.Fatal(closeErr)
		}
		t.Log(path)
	}
}
