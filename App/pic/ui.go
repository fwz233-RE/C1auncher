package main

import (
	"fmt"
	"image"
	"strings"

	"c1device"
)

type pictureAction uint8

const (
	pictureNone pictureAction = iota
	picturePrevious
	pictureNext
	pictureOpen
	pictureWallpaper
	pictureDelete
	pictureRefresh
	pictureQuit
)

type pictureState struct {
	pictures         []Picture
	selected         int
	current          image.Image
	picturesDir      string
	notice           string
	immersive        bool
	loading          bool
	loadRequested    bool
	refreshRequested bool
	confirmWallpaper bool
	confirmDelete    bool
}

func pictureActionForEvent(event c1device.Event) pictureAction {
	if event.Repeat && event.Key != c1device.KeyUp && event.Key != c1device.KeyDown && event.Key != c1device.KeyLeft && event.Key != c1device.KeyRight {
		return pictureNone
	}
	switch event.Key {
	case c1device.KeyBack:
		return pictureQuit
	case c1device.KeyUp, c1device.KeyLeft:
		return picturePrevious
	case c1device.KeyDown, c1device.KeyRight:
		return pictureNext
	case c1device.KeyOK:
		return pictureOpen
	case c1device.KeyPause:
		return pictureWallpaper
	case c1device.KeyRune:
		switch event.Rune {
		case 'p', 'P':
			return pictureWallpaper
		case 'd', 'D':
			return pictureDelete
		case 'r', 'R':
			return pictureRefresh
		}
	}
	return pictureNone
}

func (state *pictureState) handle(event c1device.Event) (bool, bool) {
	action := pictureActionForEvent(event)
	if action == pictureNone {
		return false, false
	}
	if state.confirmWallpaper || state.confirmDelete {
		if action == pictureQuit {
			state.confirmWallpaper, state.confirmDelete = false, false
			return true, false
		}
		if action != pictureOpen {
			return false, false
		}
		if state.confirmWallpaper {
			state.confirmWallpaper = false
			if state.current == nil || state.loading {
				state.notice = "请先等待图片加载完成"
				return true, false
			}
			if err := saveWallpaper(state.picturesDir, renderImmersive(state.current)); err != nil {
				state.notice = "壁纸保存失败"
				fmt.Printf("wallpaper: %v\n", err)
			} else {
				state.notice = "壁纸已设置，下次锁屏生效"
			}
			return true, false
		}
		state.confirmDelete = false
		if state.selected < 0 || state.selected >= len(state.pictures) {
			return true, false
		}
		removed := state.pictures[state.selected]
		// The fixed wallpaper is managed only through the explicit save action.
		if strings.EqualFold(removed.Name, "wallpaper.raw") {
			state.notice = "当前壁纸不能在这里删除"
			return true, false
		}
		if err := deletePicture(removed); err != nil {
			state.notice = "删除失败"
			return true, false
		}
		state.pictures = append(state.pictures[:state.selected], state.pictures[state.selected+1:]...)
		if state.selected >= len(state.pictures) {
			state.selected = len(state.pictures) - 1
		}
		state.requestSelected()
		return true, false
	}
	if action == pictureQuit {
		if state.loading {
			return false, true
		}
		if state.immersive {
			state.immersive = false
			return true, false
		}
		return false, true
	}
	state.notice = ""
	switch action {
	case picturePrevious, pictureNext:
		delta := 1
		if action == picturePrevious {
			delta = -1
		}
		state.selected = movePictureSelection(state.selected, len(state.pictures), delta)
		state.requestSelected()
	case pictureOpen:
		if state.current != nil && !state.loading {
			state.immersive = !state.immersive
		} else if !state.loading {
			state.requestSelected()
		}
	case pictureWallpaper:
		if state.current == nil || state.loading {
			state.notice = "请先等待图片加载完成"
		} else {
			state.immersive = true
			state.confirmWallpaper = true
		}
	case pictureDelete:
		if state.current != nil && !state.loading {
			state.confirmDelete = true
		}
	case pictureRefresh:
		state.refreshRequested = true
	}
	return true, false
}

func (state *pictureState) requestSelected() {
	state.current = nil
	state.notice = ""
	state.confirmWallpaper, state.confirmDelete = false, false
	state.loading = state.selected >= 0 && state.selected < len(state.pictures)
	state.loadRequested = true // Also cancels a pending load when the list is empty.
}

// Synchronous helper for tests and the headless decoder check; the interactive
// event loop uses the single cancellable loader instead.
func (state *pictureState) loadSelected() {
	state.current = nil
	if state.selected < 0 || state.selected >= len(state.pictures) {
		return
	}
	picture, err := loadPicture(state.pictures[state.selected].Path)
	if err != nil {
		state.notice = "图片无法读取"
		return
	}
	state.current = picture
}

func movePictureSelection(current, count, delta int) int {
	if count == 0 {
		return -1
	}
	if current < 0 || current >= count {
		return 0
	}
	current += delta
	if current < 0 {
		return count - 1
	}
	if current >= count {
		return 0
	}
	return current
}

func renderPicture(state pictureState, face, small *c1device.Face) c1device.Frame {
	canvas := c1device.NewCanvas()
	if state.immersive && state.current != nil {
		canvas.DrawImage(state.current, fitImageRect(state.current.Bounds(), image.Rect(0, 0, 296, 152)))
	} else {
		drawSmallText(canvas, face, 8, 2, "图片")
		drawSmallTextRight(canvas, face, 288, 2, picturePosition(state))
		canvas.DrawLine(8, 22, 287, 22)
		if state.current != nil {
			canvas.DrawImage(state.current, fitImageRect(state.current.Bounds(), image.Rect(imageRectLeft, imageRectTop, imageRectRight, imageRectBottom)))
		} else {
			label := "Pic内无图片，R刷新"
			if state.loading {
				label = "正在加载，可切图或返回"
			} else if state.notice != "" {
				label = "读取失败：请换图或R刷新"
			}
			drawSmallTextCentered(canvas, face, 148, 61, label)
		}
		if state.selected >= 0 && state.selected < len(state.pictures) {
			drawSmallText(canvas, face, 8, 111, fitPictureText(face, state.pictures[state.selected].Name, 280))
		}
		canvas.DrawLine(8, 130, 287, 130)
		drawSmallText(canvas, small, 8, 134, "方向切图 OK全屏 P壁纸 R刷新")
	}
	if state.notice != "" && !state.confirmWallpaper && !state.confirmDelete {
		white := image.NewUniform(image.White)
		canvas.DrawImage(white, image.Rect(0, 110, 296, 152))
		for i, line := range face.Wrap(state.notice, 280) {
			if i >= 2 {
				break
			}
			drawSmallText(canvas, face, 8, 112+i*19, line)
		}
	}
	if state.confirmWallpaper || state.confirmDelete {
		canvas.DrawImage(image.NewUniform(image.White), image.Rect(4, 45, 292, 109))
		canvas.DrawRect(image.Rect(4, 45, 292, 109))
		label := "将全屏画面覆盖为锁屏壁纸？"
		if state.confirmDelete {
			label = "确认删除当前图片文件？"
		}
		drawSmallTextCentered(canvas, face, 148, 52, label)
		drawSmallTextCentered(canvas, face, 148, 80, "OK确认    返回取消")
	}
	return canvas.Frame(128)
}

func picturePosition(state pictureState) string {
	if len(state.pictures) == 0 {
		return "0 / 0"
	}
	return fmt.Sprintf("%d / %d", state.selected+1, len(state.pictures))
}
func emptyPictureLabel(state pictureState) string {
	if state.notice != "" {
		return state.notice
	}
	return "请将图片放入 Pic 文件夹"
}
func drawSmallTextCentered(canvas *c1device.Canvas, face *c1device.Face, center, top int, text string) {
	canvas.DrawTextThreshold(face, center-face.Measure(text)/2, top, text, 112)
}
func fitPictureText(face *c1device.Face, text string, width int) string {
	if face.Measure(text) <= width {
		return text
	}
	runes := []rune(strings.TrimSpace(text))
	for len(runes) > 0 {
		runes = runes[:len(runes)-1]
		candidate := strings.TrimSpace(string(runes)) + "…"
		if face.Measure(candidate) <= width {
			return candidate
		}
	}
	return ""
}
