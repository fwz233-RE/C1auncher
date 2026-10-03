package c1device

import "testing"

// setPixel 按文档格式直接置位，作为测试侧的独立构造手段。
func setPixel(f *Frame, x, y int, black bool) {
	if black {
		f[(y/8)*DisplayWidth+x] |= 0x80 >> uint(y&7)
	} else {
		f[(y/8)*DisplayWidth+x] &= ^(0x80 >> uint(y&7))
	}
}

func TestFrameSize(t *testing.T) {
	if FrameBytes != 5624 {
		t.Fatalf("FrameBytes = %d, want 5624", FrameBytes)
	}
	if len(DecodeGray(Frame{}, nil)) != DisplayWidth*DisplayHeight {
		t.Fatal("DecodeGray 长度不等于一屏像素数")
	}
}

func TestPixelOutOfRangeIsWhite(t *testing.T) {
	var f Frame
	for _, c := range [][2]int{{-1, 0}, {DisplayWidth, 0}, {0, -1}, {0, DisplayHeight}, {295, 152}} {
		if f.Pixel(c[0], c[1]) {
			t.Fatalf("越界坐标 %v 不应为黑", c)
		}
	}
}

// 每一行都单独验证：能抓出 strip 边界（7/8）、首末行（0/151）以及位序错误。
func TestEachRowRoundTrip(t *testing.T) {
	for y := 0; y < DisplayHeight; y++ {
		var f Frame
		setPixel(&f, 0, y, true)
		setPixel(&f, DisplayWidth-1, y, true)
		gray := DecodeGray(f, nil)
		for yy := 0; yy < DisplayHeight; yy++ {
			for xx := 0; xx < DisplayWidth; xx++ {
				want := uint8(0xFF)
				if yy == y && (xx == 0 || xx == DisplayWidth-1) {
					want = 0x00
				}
				if gray[yy*DisplayWidth+xx] != want {
					t.Fatalf("y=%d 处 (%d,%d) = %#x, want %#x", y, xx, yy, gray[yy*DisplayWidth+xx], want)
				}
			}
		}
	}
}

// 独立复算最后一行：strip 18（151/8=18）、strip 内第 7 位（0x80>>7 = 0x01）。
func TestLastRowUsesFinalStripLeastSignificantBit(t *testing.T) {
	var f Frame
	const lastStrip = (DisplayHeight - 1) / 8 // 18
	for x := 0; x < DisplayWidth; x++ {
		f[lastStrip*DisplayWidth+x] |= 0x01
	}
	gray := DecodeGray(f, nil)
	for x := 0; x < DisplayWidth; x++ {
		if gray[(DisplayHeight-1)*DisplayWidth+x] != 0x00 {
			t.Fatalf("最后一行 x=%d 应为黑", x)
		}
		if gray[(DisplayHeight-2)*DisplayWidth+x] != 0xFF {
			t.Fatalf("倒数第二行 x=%d 不应被置黑（位序错）", x)
		}
	}
}

func TestAllBlackAndAllWhite(t *testing.T) {
	var black Frame
	for i := range black {
		black[i] = 0xFF
	}
	for _, v := range DecodeGray(black, nil) {
		if v != 0x00 {
			t.Fatal("全 0xFF 应解出全黑（位极性反了）")
		}
	}
	var white Frame
	for _, v := range DecodeGray(white, nil) {
		if v != 0xFF {
			t.Fatal("全 0x00 应解出全白（位极性反了）")
		}
	}
}

// 抽样往返：每 37 个像素置黑，解码后必须恰好这些像素为黑。
func TestSampledRoundTrip(t *testing.T) {
	var f Frame
	want := make(map[int]bool)
	for i := 0; i < DisplayWidth*DisplayHeight; i += 37 {
		x, y := i%DisplayWidth, i/DisplayWidth
		setPixel(&f, x, y, true)
		want[i] = true
	}
	gray := DecodeGray(f, nil)
	for i, v := range gray {
		if (v == 0x00) != want[i] {
			t.Fatalf("索引 %d = %#x, 是否应为黑: %v", i, v, want[i])
		}
	}
}
