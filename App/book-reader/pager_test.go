package main

import (
	"os"
	"path/filepath"
	"testing"
)

type fixedFace struct{}

func (fixedFace) LineHeight() int { return 10 }
func (fixedFace) Wrap(text string, width int) []string {
	if text == "" {
		return []string{""}
	}
	runes := []rune(text)
	if len(runes) <= width {
		return []string{text}
	}
	result := make([]string, 0)
	for len(runes) > 0 {
		end := width
		if end > len(runes) {
			end = len(runes)
		}
		result = append(result, string(runes[:end]))
		runes = runes[end:]
	}
	return result
}

func TestPaginateForwardBackwardOffsets(t *testing.T) {
	path := filepath.Join(t.TempDir(), "book.txt")
	content := "第一章 开始\n甲乙丙丁\n第二行\n第三行\n"
	if err := os.WriteFile(path, []byte(content), 0o644); err != nil {
		t.Fatal(err)
	}
	document, err := OpenDocument(path)
	if err != nil {
		t.Fatal(err)
	}
	chapter := document.Chapters[0]
	pages, loadedEnd, err := Paginate(document, chapter, chapter.Start, fixedFace{}, 2, 20)
	if err != nil {
		t.Fatal(err)
	}
	if len(pages) < 2 {
		t.Fatalf("got %d pages", len(pages))
	}
	if pages[0].Start < chapter.Start || pages[len(pages)-1].End > chapter.End || loadedEnd != chapter.End {
		t.Fatalf("invalid bounds: %#v", pages)
	}
	if pages[1].Start <= pages[0].Start {
		t.Fatalf("wrapped page did not advance raw offset: %#v", pages)
	}
	for index := 1; index < len(pages); index++ {
		if pages[index].Start < pages[index-1].Start || pages[index].End < pages[index-1].End {
			t.Fatalf("offsets not monotonic: %#v", pages)
		}
	}
	for index := len(pages) - 1; index > 0; index-- {
		if pages[index-1].Start > pages[index].Start {
			t.Fatal("backward page offset moved forward")
		}
	}
}
