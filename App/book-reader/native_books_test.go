package main

import "testing"

// Run the supplied books through the exact native font and device dimensions,
// not only the lightweight fake font used by ingestion unit tests.
func TestSuppliedBooksNativeLayout(t *testing.T) {
	_, paths := suppliedBooks(t)
	t.Setenv("C1BOOK_READER_CACHE_DIR", t.TempDir())
	face, err := newReaderFace(true)
	if err != nil {
		t.Fatal(err)
	}
	defer face.Close()
	for index, path := range paths {
		before := fileDigest(t, path)
		doc, err := OpenDocument(path)
		if err != nil {
			t.Fatal(err)
		}
		count := 0
		for ci, ch := range doc.Chapters {
			start := ch.Start
			for {
				pages, end, err := Paginate(doc, ch, start, face, readerTextWidth, readerBodyHeight)
				if err != nil {
					t.Fatalf("book %d chapter %d: %v", index, ci, err)
				}
				for _, page := range pages {
					if len(page.Lines) > 5 {
						t.Fatal("page exceeds native body height")
					}
					for _, line := range page.Lines {
						if face.Measure(line) > readerTextWidth {
							t.Fatalf("native line overflows: book %d chapter %d", index, ci)
						}
					}
				}
				count += len(pages)
				if end >= ch.End {
					break
				}
				if end <= start {
					t.Fatal("non-advancing native page window")
				}
				start = end
			}
		}
		if before != fileDigest(t, path) {
			t.Fatal("book was modified")
		}
		t.Logf("book %d: %d chapters, %d native-layout pages; 282px width / five 20px lines PASS", index, len(doc.Chapters), count)
	}
}
