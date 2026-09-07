package main

import (
	"context"
	"errors"
	"fmt"
	"image"
	"image/color"
	"io"
	"os"
	"path/filepath"
	"strings"

	"c1device"
	"c1pic/internal/lowmem"
)

const (
	maxImageFileSize = 64 << 20
	imageRectLeft    = 8
	imageRectTop     = 25
	imageRectRight   = 288
	imageRectBottom  = 109
)

func loadPicture(path string) (image.Image, error) {
	return loadPictureContext(context.Background(), path)
}

// The decoder produces at most a panel-sized image. No original-size image is
// retained by the viewer, and the worker only decodes one selected file at a time.
func loadPictureContext(ctx context.Context, path string) (image.Image, error) {
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	info, err := os.Lstat(path)
	if err != nil {
		return nil, err
	}
	if !info.Mode().IsRegular() || info.Size() <= 0 {
		return nil, fmt.Errorf("图片文件无效")
	}
	if info.Size() > maxImageFileSize {
		return nil, fmt.Errorf("%w: file exceeds 64 MiB", lowmem.ErrLimit)
	}
	file, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer file.Close()
	opened, err := file.Stat()
	if err != nil {
		return nil, err
	}
	if !os.SameFile(info, opened) || opened.Size() != info.Size() {
		return nil, fmt.Errorf("图片正在修改，请刷新重试")
	}
	if strings.EqualFold(filepath.Ext(path), ".raw") {
		if info.Size() != c1device.FrameBytes {
			return nil, fmt.Errorf("RAW必须是296×152的5624字节壁纸")
		}
		var frame c1device.Frame
		if _, err := io.ReadFull(file, frame[:]); err != nil {
			return nil, err
		}
		return imageFromFrame(frame), nil
	}
	return lowmem.Decode(ctx, file, c1device.DisplayWidth, c1device.DisplayHeight)
}

func pictureErrorNotice(err error) string {
	if errors.Is(err, lowmem.ErrUnsupported) {
		return "此编码暂不支持大图，请转为普通JPG或非交错PNG"
	}
	if errors.Is(err, lowmem.ErrLimit) {
		return "图片超过安全限制，已停止加载"
	}
	if strings.Contains(err.Error(), "RAW") {
		return "RAW需为296×152、5624字节"
	}
	return "图片损坏或无法读取，请换图或刷新"
}

func imageFromFrame(frame c1device.Frame) *image.Gray {
	result := image.NewGray(image.Rect(0, 0, c1device.DisplayWidth, c1device.DisplayHeight))
	for y := 0; y < c1device.DisplayHeight; y++ {
		for x := 0; x < c1device.DisplayWidth; x++ {
			value := uint8(255)
			if frame[(y/8)*c1device.DisplayWidth+x]&(0x80>>uint(y%8)) != 0 {
				value = 0
			}
			result.SetGray(x, y, color.Gray{Y: value})
		}
	}
	return result
}

func fitImageRect(source, area image.Rectangle) image.Rectangle {
	if source.Empty() || area.Empty() {
		return image.Rectangle{}
	}
	width, height := area.Dx(), area.Dy()
	if int64(source.Dx())*int64(height) > int64(source.Dy())*int64(width) {
		height = max(1, int(int64(source.Dy())*int64(width)/int64(source.Dx())))
	} else {
		width = max(1, int(int64(source.Dx())*int64(height)/int64(source.Dy())))
	}
	left := area.Min.X + (area.Dx()-width)/2
	top := area.Min.Y + (area.Dy()-height)/2
	return image.Rect(left, top, left+width, top+height)
}

// Shared by immersive viewing and wallpaper saving: no title, hint or notice
// is ever included in the saved image. Aspect ratio is preserved on white.
func renderImmersive(picture image.Image) c1device.Frame {
	canvas := c1device.NewCanvas()
	if picture != nil {
		canvas.DrawImage(picture, fitImageRect(picture.Bounds(), image.Rect(0, 0, c1device.DisplayWidth, c1device.DisplayHeight)))
	}
	return canvas.Frame(128)
}
