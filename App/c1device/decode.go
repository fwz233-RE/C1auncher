package c1device

// 帧像素格式（三个独立来源交叉验证，见 README）：
//
//	5624 字节 = 296 × (152/8)，strip-major 打包
//	offset = (y/8)*DisplayWidth + x
//	位     = 0x80 >> (y&7)
//	bit=1 表示黑色，MSB 是该 strip 的最上面一行
//
// 来源：
//  1. 上游 text.go 的 Canvas.Frame()：output[(y/8)*DisplayWidth+x] |= 0x80 >> (y&7)
//  2. mason-yb-zhang/c1slim-toolkit tools/frame2png.py：frame[(y//8)*W+x] & (0x80>>(y%8))
//  3. 上游 App/refresh-test、App/badapple 的 writePreview()（3× 最近邻，同公式）

// Pixel 报告 (x, y) 是否为黑。越界返回 false。
func (f Frame) Pixel(x, y int) bool {
	if uint(x) >= DisplayWidth || uint(y) >= DisplayHeight {
		return false
	}
	return f[(y/8)*DisplayWidth+x]&(0x80>>uint(y&7)) != 0
}

// DecodeGray 把一帧展开成每像素一字节的灰度：0x00 为黑、0xFF 为白。
// dst 为 nil 时自行分配；长度必须不小于 DisplayWidth*DisplayHeight。
// 这是模拟器渲染与 frame2png.py 交叉校验的唯一入口。
func DecodeGray(f Frame, dst []uint8) []uint8 {
	if dst == nil {
		dst = make([]uint8, DisplayWidth*DisplayHeight)
	}
	for strip := 0; strip < DisplayHeight/8; strip++ {
		base := strip * DisplayWidth
		for x := 0; x < DisplayWidth; x++ {
			b := f[base+x]
			for row := 0; row < 8; row++ {
				if b&(0x80>>uint(row)) != 0 {
					dst[(strip*8+row)*DisplayWidth+x] = 0x00
				} else {
					dst[(strip*8+row)*DisplayWidth+x] = 0xFF
				}
			}
		}
	}
	return dst
}
