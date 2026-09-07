package main

import (
	"bytes"
	"encoding/json"
	"io"
	"os"
	"path/filepath"
	"testing"

	"golang.org/x/text/encoding/simplifiedchinese"
	"golang.org/x/text/transform"
)

func TestOpenDocumentUTF8ChaptersAndOffsets(t *testing.T) {
	path := filepath.Join(t.TempDir(), "book.txt")
	content := "序言第一段也能阅读。\n第一卷 魔性不改\n卷正文\n第一节：穿越\n正文首段\n第二章 继续\n结尾\n"
	if err := os.WriteFile(path, append([]byte{0xef, 0xbb, 0xbf}, []byte(content)...), 0o644); err != nil {
		t.Fatal(err)
	}
	document, err := OpenDocument(path)
	if err != nil {
		t.Fatal(err)
	}
	if document.Encoding != EncodingUTF8 {
		t.Fatalf("encoding = %s", document.Encoding)
	}
	want := []string{"开始", "第一卷 魔性不改", "第一节：穿越", "第二章 继续"}
	if len(document.Chapters) != len(want) {
		t.Fatalf("chapters = %#v", document.Chapters)
	}
	for index, title := range want {
		if document.Chapters[index].Title != title {
			t.Errorf("chapter %d = %q, want %q", index, document.Chapters[index].Title, title)
		}
		if document.Chapters[index].Start >= document.Chapters[index].End {
			t.Errorf("chapter %d has invalid offsets", index)
		}
		if index > 0 && document.Chapters[index-1].End != document.Chapters[index].Start {
			t.Errorf("chapter boundary %d is not contiguous", index)
		}
	}
}

func TestOpenDocumentUsesChapterIndexCache(t *testing.T) {
	root := t.TempDir()
	path := filepath.Join(root, "cached.txt")
	content := "第一章：开端\n正文\n第二章：继续\n结尾\n"
	if err := os.WriteFile(path, []byte(content), 0o644); err != nil {
		t.Fatal(err)
	}
	first, err := OpenDocument(path)
	if err != nil {
		t.Fatal(err)
	}
	cachePath := chapterIndexCachePath(path)
	if _, err := os.Stat(cachePath); err != nil {
		t.Fatalf("chapter index cache was not written: %v", err)
	}
	data, err := os.ReadFile(cachePath)
	if err != nil {
		t.Fatal(err)
	}
	var cache chapterIndexCache
	if err := json.Unmarshal(data, &cache); err != nil {
		t.Fatal(err)
	}
	cache.Chapters[0].Title = "缓存命中"
	data, err = json.Marshal(cache)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(cachePath, data, 0o644); err != nil {
		t.Fatal(err)
	}
	cachedDocument, err := OpenDocument(path)
	if err != nil {
		t.Fatal(err)
	}
	if cachedDocument.Chapters[0].Title != "缓存命中" {
		t.Fatalf("valid cache was not used: %#v", cachedDocument.Chapters)
	}
	if err := os.WriteFile(cachePath, []byte("not json"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, []byte(content+"新增内容\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	second, err := OpenDocument(path)
	if err != nil {
		t.Fatal(err)
	}
	if len(second.Chapters) != len(first.Chapters) {
		t.Fatalf("rebuilt chapters = %#v", second.Chapters)
	}
	data, err = os.ReadFile(cachePath)
	if err != nil {
		t.Fatal(err)
	}
	cache = chapterIndexCache{}
	if err := json.Unmarshal(data, &cache); err != nil {
		t.Fatalf("rebuilt cache is invalid: %v", err)
	}
	if cache.Fingerprint.Size != int64(len(content+"新增内容\n")) || cache.Chapters[len(cache.Chapters)-1].End != int64(len(content+"新增内容\n")) {
		t.Fatalf("cache was not rebuilt: %#v", cache)
	}
	cached, ok := loadChapterIndexCache(path, BookFingerprint{Size: cache.Fingerprint.Size, ModTime: cache.Fingerprint.ModTime})
	if !ok || len(cached.Chapters) != len(second.Chapters) {
		t.Fatalf("rebuilt cache was not accepted: %#v, %v", cached, ok)
	}
}

func TestOpenDocumentGB18030(t *testing.T) {
	path := filepath.Join(t.TempDir(), "gb.txt")
	input := "第一节：开端\n正文首段\n第二节：继续\n内容\n"
	encoded, err := io.ReadAll(transform.NewReader(bytes.NewBufferString(input), simplifiedchinese.GB18030.NewEncoder()))
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, encoded, 0o644); err != nil {
		t.Fatal(err)
	}
	document, err := OpenDocument(path)
	if err != nil {
		t.Fatal(err)
	}
	if document.Encoding != EncodingGB18030 {
		t.Fatalf("encoding = %s", document.Encoding)
	}
	if len(document.Chapters) != 2 || document.Chapters[1].Title != "第二节：继续" {
		t.Fatalf("chapters = %#v", document.Chapters)
	}
}
