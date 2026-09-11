package main

import (
	"bytes"
	"crypto/sha256"
	"io"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"
	"unicode/utf8"
)

// Opt-in: supplied books are never copied into source or package assets.
func suppliedBooks(t *testing.T) (string, []string) {
	t.Helper()
	root := os.Getenv("BOOK_READER_TEST_BOOKS")
	if root == "" {
		t.Skip("set BOOK_READER_TEST_BOOKS for read-only real-book tests")
	}
	entries, err := os.ReadDir(root)
	if err != nil {
		t.Fatal(err)
	}
	paths := []string{filepath.Join(root, "蛊真人.txt"), filepath.Join(root, "040.花开堪折.txt")}
	for _, entry := range entries {
		if strings.EqualFold(filepath.Ext(entry.Name()), ".epub") {
			paths = append(paths, filepath.Join(root, entry.Name()))
		}
	}
	if len(paths) != 3 {
		t.Fatalf("expected exactly one supplied EPUB; total books=%d", len(paths))
	}
	return root, paths
}

func TestSuppliedBookByteDiagnostics(t *testing.T) {
	_, paths := suppliedBooks(t)
	for i, path := range paths[:2] {
		raw, err := os.ReadFile(path)
		if err != nil {
			t.Fatal(err)
		}
		file, err := os.Open(path)
		if err != nil {
			t.Fatal(err)
		}
		encoding, bom, err := detectEncoding(file)
		file.Close()
		if err != nil {
			t.Fatal(err)
		}
		text, err := decodeBytes(raw, encoding)
		if err != nil {
			t.Fatal(err)
		}
		legacy, decorated, invalidLines := 0, 0, 0
		for _, line := range strings.Split(text, "\n") {
			title := strings.TrimSpace(line)
			if utf8.RuneCountInString(title) <= 80 && (chapterTitlePattern.MatchString(title) || volumeTitlePattern.MatchString(title)) {
				legacy++
			}
			if isChapterTitle(title) && normalizedChapterTitle(title) != title {
				decorated++
			}
			if strings.ContainsRune(title, '\ufffd') {
				invalidLines++
			}
		}
		prefix := raw
		if len(prefix) > 4 {
			prefix = prefix[:4]
		}
		t.Logf("TXT[%d]: sourceBytes=%d prefixHex=%x validUTF8=%v encoding=%s BOM=%d LF=%d CR=%d legacyHeadings=%d decoratedHeadings=%d replacementRunes=%d affectedLines=%d", i, len(raw), prefix, utf8.Valid(raw), encoding, bom, bytes.Count(raw, []byte{'\n'}), bytes.Count(raw, []byte{'\r'}), legacy, decorated, strings.Count(text, "\ufffd"), invalidLines)
		if i == 1 && (encoding != EncodingGB18030 || legacy != 0 || decorated < 300) {
			t.Fatal("supplied GB text regression was not reproduced")
		}
	}
}

func fileDigest(t *testing.T, path string) [32]byte {
	t.Helper()
	file, err := os.Open(path)
	if err != nil {
		t.Fatal(err)
	}
	defer file.Close()
	hash := sha256.New()
	if _, err := io.Copy(hash, file); err != nil {
		t.Fatal(err)
	}
	var sum [32]byte
	copy(sum[:], hash.Sum(nil))
	return sum
}
func TestSuppliedBooksReadOnlyIngestion(t *testing.T) {
	root, paths := suppliedBooks(t)
	t.Setenv("C1BOOK_READER_CACHE_DIR", t.TempDir())
	beforeEntries, err := os.ReadDir(root)
	if err != nil {
		t.Fatal(err)
	}
	for index, path := range paths {
		t.Run([]string{"UTF8-novel", "GB18030-novel", "EPUB-history"}[index], func(t *testing.T) {
			before := fileDigest(t, path)
			mark, err := fingerprint(path)
			if err != nil {
				t.Fatal(err)
			}
			document, err := OpenDocument(path)
			if err != nil {
				t.Fatal(err)
			}
			if document.Path != path || !validChapterIndex(document.Chapters, document.Size) {
				t.Fatal("invalid identity or chapter ranges")
			}
			if len(document.Chapters) < 2 {
				t.Fatal("book was not divided into chapters")
			}
			pagesTotal := 0
			for chapterIndex, chapter := range document.Chapters {
				start := chapter.Start
				for {
					pages, end, err := Paginate(document, chapter, start, fixedFace{}, 40, 240)
					if err != nil {
						t.Fatalf("chapter %d at %d: %v", chapterIndex, start, err)
					}
					for _, page := range pages {
						if page.Start < start || page.End > chapter.End || page.Start > page.End {
							t.Fatalf("invalid page bounds in chapter %d", chapterIndex)
						}
						for _, line := range page.Lines {
							if !utf8.ValidString(line) {
								t.Fatal("invalid decoded page text")
							}
						}
					}
					pagesTotal += len(pages)
					if end >= chapter.End {
						break
					}
					if end <= start {
						t.Fatal("pagination failed to advance")
					}
					start = end
				}
			}
			again, err := OpenDocument(path)
			if err != nil {
				t.Fatal(err)
			}
			if !reflect.DeepEqual(document.Chapters, again.Chapters) || document.Size != again.Size {
				t.Fatal("cached document changed navigation")
			}
			testIngestionPersistence(t, document)
			afterMark, err := fingerprint(path)
			if err != nil {
				t.Fatal(err)
			}
			if before != fileDigest(t, path) || mark != afterMark {
				t.Fatal("source book was changed")
			}
			t.Logf("sourceBytes=%d textBytes=%d encoding=%s chapters=%d pages=%d normalized=%v originalIdentity=true sourceHashUnchanged=true", mark.Size, document.Size, document.Encoding, len(document.Chapters), pagesTotal, document.dataPath != "")
		})
	}
	afterEntries, err := os.ReadDir(root)
	if err != nil {
		t.Fatal(err)
	}
	names := func(entries []os.DirEntry) []string {
		var result []string
		for _, entry := range entries {
			result = append(result, entry.Name())
		}
		return result
	}
	if !reflect.DeepEqual(names(beforeEntries), names(afterEntries)) {
		t.Fatal("files were created beside source books")
	}
}

func testIngestionPersistence(t *testing.T, document *Document) {
	t.Helper()
	chapterIndex := len(document.Chapters) - 1
	offset := document.Chapters[chapterIndex].Start
	mark, err := fingerprint(document.Path)
	if err != nil {
		t.Fatal(err)
	}
	store := ProgressStore{Dir: t.TempDir()}
	if err := store.Save(Progress{Path: document.Path, Fingerprint: mark, Chapter: chapterIndex, Offset: offset}); err != nil {
		t.Fatal(err)
	}
	saved, ok, err := store.Load(document.Path)
	if err != nil || !ok || saved.Offset != offset || saved.Chapter != chapterIndex {
		t.Fatal("original-identity progress roundtrip failed")
	}
	bookmarks := BookmarkStore{Dir: t.TempDir()}
	want := []Bookmark{{Chapter: chapterIndex, Offset: offset}}
	if err := bookmarks.Save(document.Path, want); err != nil {
		t.Fatal(err)
	}
	got, err := bookmarks.Load(document.Path)
	if err != nil || !reflect.DeepEqual(got, want) {
		t.Fatal("original-identity bookmark roundtrip failed")
	}
}
