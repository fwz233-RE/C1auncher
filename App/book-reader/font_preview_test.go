package main

import "testing"

func TestNativeBodyPreview(t *testing.T) {
	app := readingFixture(t)
	readerUIFaces(t, app)
	app.pages = []Page{{Start: 0, End: app.document.Size, Lines: []string{
		"中文阅读采用原生点阵字体。",
		"清朝开国史：目录、正文、书签。",
		"横竖笔画直接对应屏幕像素，",
		"不缩放，不平滑，不作灰度抖动。",
		"数字 0123456789 与 ABC abc。",
	}}}
	writeReaderPreview(t, "native-body", app.render())
}
