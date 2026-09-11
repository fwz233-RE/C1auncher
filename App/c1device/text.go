package c1device

import (
	"fmt"
	"image"
	"image/color"
	"strings"

	"golang.org/x/image/font"
	"golang.org/x/image/font/opentype"
	"golang.org/x/image/math/fixed"
)

type Typeface struct {
	font *opentype.Font
}

type Face struct {
	face       font.Face
	lineHeight int
	ascent     int
}

type Canvas struct {
	image *image.Gray
}

func ParseTypeface(data []byte) (*Typeface, error) {
	parsed, err := opentype.Parse(data)
	if err != nil {
		return nil, fmt.Errorf("parse typeface: %w", err)
	}
	return &Typeface{font: parsed}, nil
}

func (typeface *Typeface) NewFace(size float64) (*Face, error) {
	face, err := opentype.NewFace(typeface.font, &opentype.FaceOptions{
		Size:    size,
		DPI:     72,
		Hinting: font.HintingFull,
	})
	if err != nil {
		return nil, fmt.Errorf("create font face: %w", err)
	}
	metrics := face.Metrics()
	return &Face{
		face:       face,
		lineHeight: metrics.Height.Ceil(),
		ascent:     metrics.Ascent.Ceil(),
	}, nil
}

func (face *Face) Close() error {
	if closer, ok := face.face.(interface{ Close() error }); ok {
		return closer.Close()
	}
	return nil
}

func (face *Face) LineHeight() int { return face.lineHeight }

func (face *Face) Measure(text string) int {
	return font.MeasureString(face.face, text).Ceil()
}

func (face *Face) Wrap(text string, width int) []string {
	if width <= 0 {
		return nil
	}
	text = strings.TrimSuffix(strings.TrimSuffix(text, "\n"), "\r")
	if text == "" {
		return []string{""}
	}
	lines := make([]string, 0, 2)
	current := make([]rune, 0, len([]rune(text)))
	currentWidth := 0
	for _, character := range text {
		advance, ok := face.face.GlyphAdvance(character)
		characterWidth := advance.Ceil()
		if !ok || characterWidth <= 0 {
			characterWidth = face.Measure(string(character))
		}
		if currentWidth+characterWidth > width && len(current) > 0 {
			lines = append(lines, string(current))
			current = current[:0]
			currentWidth = 0
		}
		current = append(current, character)
		currentWidth += characterWidth
	}
	if len(current) > 0 {
		lines = append(lines, string(current))
	}
	return lines
}

func NewCanvas() *Canvas {
	canvas := &Canvas{image: image.NewGray(image.Rect(0, 0, DisplayWidth, DisplayHeight))}
	canvas.Clear()
	return canvas
}

func (canvas *Canvas) Clear() {
	for index := range canvas.image.Pix {
		canvas.image.Pix[index] = 0xff
	}
}

const textCoverageThreshold uint8 = 48

func (canvas *Canvas) DrawText(face *Face, x, top int, text string) {
	canvas.DrawTextThreshold(face, x, top, text, textCoverageThreshold)
}

// DrawTextThreshold renders glyph coverage directly to 1-bit pixels. Higher
// thresholds produce cleaner, lighter edges on monochrome displays.
func (canvas *Canvas) DrawTextThreshold(face *Face, x, top int, text string, threshold uint8) {
	if text == "" {
		return
	}
	if bitmap, ok := face.face.(*bitmapFace); ok {
		canvas.drawBitmap(bitmap, x, top, text)
		return
	}
	baseline := fixed.P(x, top+face.ascent)
	glyphBounds, _ := font.BoundString(face.face, text)
	bounds := image.Rect(
		(baseline.X + glyphBounds.Min.X).Floor(),
		(baseline.Y + glyphBounds.Min.Y).Floor(),
		(baseline.X + glyphBounds.Max.X).Ceil(),
		(baseline.Y + glyphBounds.Max.Y).Ceil(),
	).Intersect(canvas.image.Bounds())
	if bounds.Empty() {
		return
	}
	mask := image.NewAlpha(bounds)
	drawer := font.Drawer{
		Dst:  mask,
		Src:  image.NewUniform(color.Alpha{A: 0xff}),
		Face: face.face,
		Dot:  baseline,
	}
	drawer.DrawString(text)
	canvas.drawTextMask(mask, threshold)
}

func (canvas *Canvas) drawTextMask(mask *image.Alpha, threshold uint8) {
	bounds := mask.Bounds().Intersect(canvas.image.Bounds())
	for y := bounds.Min.Y; y < bounds.Max.Y; y++ {
		for x := bounds.Min.X; x < bounds.Max.X; x++ {
			if mask.AlphaAt(x, y).A >= threshold {
				canvas.setBlack(x, y)
			}
		}
	}
}

func (canvas *Canvas) DrawImage(source image.Image, rect image.Rectangle) {
	rect = rect.Intersect(canvas.image.Bounds())
	if rect.Empty() || source == nil {
		return
	}
	sourceBounds := source.Bounds()
	if sourceBounds.Empty() {
		return
	}
	sourceWidth, sourceHeight := sourceBounds.Dx(), sourceBounds.Dy()
	destinationWidth, destinationHeight := rect.Dx(), rect.Dy()
	for y := rect.Min.Y; y < rect.Max.Y; y++ {
		for x := rect.Min.X; x < rect.Max.X; x++ {
			sourceX := sourceBounds.Min.X + (x-rect.Min.X)*sourceWidth/destinationWidth
			sourceY := sourceBounds.Min.Y + (y-rect.Min.Y)*sourceHeight/destinationHeight
			gray := color.GrayModel.Convert(source.At(sourceX, sourceY)).(color.Gray).Y
			threshold := uint8(imageDither[(y-rect.Min.Y)&3][(x-rect.Min.X)&3])
			if gray < threshold {
				canvas.setBlack(x, y)
			} else {
				canvas.image.SetGray(x, y, color.Gray{Y: 0xff})
			}
		}
	}
}

var imageDither = [4][4]uint8{
	{96, 160, 112, 176},
	{192, 32, 208, 48},
	{128, 224, 144, 240},
	{224, 64, 240, 80},
}

func (canvas *Canvas) DrawLine(x0, y0, x1, y1 int) {
	dx, sx := abs(x1-x0), -1
	if x0 < x1 {
		sx = 1
	}
	dy, sy := -abs(y1-y0), -1
	if y0 < y1 {
		sy = 1
	}
	err := dx + dy
	for {
		canvas.setBlack(x0, y0)
		if x0 == x1 && y0 == y1 {
			return
		}
		twice := 2 * err
		if twice >= dy {
			err += dy
			x0 += sx
		}
		if twice <= dx {
			err += dx
			y0 += sy
		}
	}
}

func (canvas *Canvas) DrawRect(rect image.Rectangle) {
	rect = rect.Intersect(canvas.image.Bounds())
	if rect.Empty() {
		return
	}
	canvas.DrawLine(rect.Min.X, rect.Min.Y, rect.Max.X-1, rect.Min.Y)
	canvas.DrawLine(rect.Min.X, rect.Max.Y-1, rect.Max.X-1, rect.Max.Y-1)
	canvas.DrawLine(rect.Min.X, rect.Min.Y, rect.Min.X, rect.Max.Y-1)
	canvas.DrawLine(rect.Max.X-1, rect.Min.Y, rect.Max.X-1, rect.Max.Y-1)
}

func (canvas *Canvas) InvertRect(rect image.Rectangle) {
	rect = rect.Intersect(canvas.image.Bounds())
	for y := rect.Min.Y; y < rect.Max.Y; y++ {
		for x := rect.Min.X; x < rect.Max.X; x++ {
			offset := canvas.image.PixOffset(x, y)
			canvas.image.Pix[offset] = 0xff - canvas.image.Pix[offset]
		}
	}
}

func (canvas *Canvas) DrawTextRight(face *Face, right, top int, text string) {
	canvas.DrawText(face, right-face.Measure(text), top, text)
}

func (canvas *Canvas) DrawTextCentered(face *Face, center, top int, text string) {
	canvas.DrawText(face, center-face.Measure(text)/2, top, text)
}

func (canvas *Canvas) DrawTextInverted(face *Face, rect image.Rectangle, x, top int, text string) {
	canvas.DrawText(face, x, top, text)
	canvas.InvertRect(rect)
}

func (canvas *Canvas) DrawInvertedTextBar(face *Face, rect image.Rectangle, text string) {
	top := rect.Min.Y + (rect.Dy()-face.LineHeight())/2
	canvas.DrawTextCentered(face, rect.Min.X+rect.Dx()/2, top, text)
	canvas.InvertRect(rect)
}

func (canvas *Canvas) DrawScrollIndicator(x, top, bottom, selected, count int) {
	if count <= 1 || bottom-top <= 4 {
		return
	}
	trackHeight := bottom - top - 4
	thumbHeight := trackHeight / count
	if thumbHeight < 8 {
		thumbHeight = 8
	}
	if thumbHeight > trackHeight {
		thumbHeight = trackHeight
	}
	travel := trackHeight - thumbHeight
	thumbTop := top + 2 + selected*travel/(count-1)
	canvas.DrawLine(x, top+2, x, bottom-3)
	canvas.FillRect(image.Rect(x-2, thumbTop, x+3, thumbTop+thumbHeight))
}

func (canvas *Canvas) FillRect(rect image.Rectangle) {
	rect = rect.Intersect(canvas.image.Bounds())
	for y := rect.Min.Y; y < rect.Max.Y; y++ {
		for x := rect.Min.X; x < rect.Max.X; x++ {
			canvas.setBlack(x, y)
		}
	}
}

func (canvas *Canvas) Frame(threshold uint8) Frame {
	var output Frame
	for y := 0; y < DisplayHeight; y++ {
		for x := 0; x < DisplayWidth; x++ {
			if canvas.image.GrayAt(x, y).Y >= threshold {
				continue
			}
			offset := (y/8)*DisplayWidth + x
			output[offset] |= 0x80 >> (y & 7)
		}
	}
	return output
}

func (canvas *Canvas) setBlack(x, y int) {
	if image.Pt(x, y).In(canvas.image.Bounds()) {
		canvas.image.SetGray(x, y, color.Gray{Y: 0})
	}
}

func abs(value int) int {
	if value < 0 {
		return -value
	}
	return value
}
