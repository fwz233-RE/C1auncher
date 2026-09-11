package c1device

import (
	"encoding/binary"
	"image"
	"testing"
)

func bitmapFixture() []byte {
	data := []byte("C1BF")
	for _, cp := range []rune{'A', '中', 0xfffd} {
		rec := make([]byte, 37)
		binary.LittleEndian.PutUint32(rec, uint32(cp))
		rec[4] = 16
		if cp == 'A' {
			rec[4] = 8
		}
		for y := 0; y < 16; y++ {
			binary.BigEndian.PutUint16(rec[5+y*2:], 0x8100)
		}
		data = append(data, rec...)
	}
	return data
}
func TestNativeBitmapPixelsAndClipping(t *testing.T) {
	face, err := NewBitmapFace(bitmapFixture(), 20)
	if err != nil {
		t.Fatal(err)
	}
	defer face.Close()
	if face.Measure("A中") != 24 || face.LineHeight() != 20 {
		t.Fatal("metrics")
	}
	for _, origin := range []image.Point{{0, 0}, {-4, -3}, {290, 146}} {
		canvas := NewCanvas()
		canvas.DrawText(face, origin.X, origin.Y, "中")
		frame := canvas.Frame(128)
		for y := 0; y < DisplayHeight; y++ {
			for x := 0; x < DisplayWidth; x++ {
				on := frame[(y/8)*DisplayWidth+x]&(0x80>>uint(y&7)) != 0
				want := y >= origin.Y && y < origin.Y+16 && (x == origin.X || x == origin.X+7)
				if on != want {
					t.Fatalf("native mismatch %v at %d,%d", origin, x, y)
				}
			}
		}
	}
	a, b := NewCanvas(), NewCanvas()
	a.DrawTextThreshold(face, 0, 0, "A中", 48)
	b.DrawTextThreshold(face, 0, 0, "A中", 220)
	if a.Frame(128) != b.Frame(128) {
		t.Fatal("bitmap changed with coverage threshold")
	}
}
func TestNativeBitmapValidation(t *testing.T) {
	for _, data := range [][]byte{nil, []byte("C1BF"), append(bitmapFixture(), 0)} {
		if _, err := NewBitmapFace(data, 16); err == nil {
			t.Fatal("accepted malformed font")
		}
	}
	d := bitmapFixture()
	d[8] = 9
	if _, err := NewBitmapFace(d, 16); err == nil {
		t.Fatal("accepted invalid width")
	}
}
