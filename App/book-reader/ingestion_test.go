package main

import (
	"archive/zip"
	"bytes"
	"encoding/binary"
	"fmt"
	"os"
	"path/filepath"
	"reflect"
	"sort"
	"strings"
	"testing"
	"unicode/utf16"

	"golang.org/x/text/encoding/simplifiedchinese"
	"golang.org/x/text/transform"
)

func writeEPUBFixture(t *testing.T, files map[string]string) string {
	t.Helper()
	name := filepath.Join(t.TempDir(), "test.epub")
	file, err := os.Create(name)
	if err != nil {
		t.Fatal(err)
	}
	archive := zip.NewWriter(file)
	var names []string
	for name := range files {
		names = append(names, name)
	}
	sort.Strings(names)
	for _, name := range names {
		writer, err := archive.Create(name)
		if err != nil {
			t.Fatal(err)
		}
		if _, err := writer.Write([]byte(files[name])); err != nil {
			t.Fatal(err)
		}
	}
	if err := archive.Close(); err != nil {
		t.Fatal(err)
	}
	if err := file.Close(); err != nil {
		t.Fatal(err)
	}
	return name
}
func epubFixture() map[string]string {
	return map[string]string{
		"mimetype":               "application/epub+zip",
		"META-INF/container.xml": `<?xml version="1.0"?><container xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles><rootfile full-path="OPS/package.opf" media-type="application/oebps-package+xml"/></rootfiles></container>`,
		"OPS/package.opf":        `<package xmlns="http://www.idpf.org/2007/opf"><manifest><item id="a" href="Text/a.xhtml" media-type="application/xhtml+xml"/><item id="b" href="Text/b.xhtml" media-type="application/xhtml+xml"/><item id="nav" href="nav.xhtml" properties="nav" media-type="application/xhtml+xml"/></manifest><spine><itemref idref="b"/><itemref idref="a"/></spine></package>`,
		"OPS/nav.xhtml":          `<html xmlns:epub="http://www.idpf.org/2007/ops"><body><nav epub:type="toc"><ol><li><a href="Text/b.xhtml">乙章</a></li><li><a href="Text/a.xhtml#part1">甲一</a></li><li><a href="Text/a.xhtml#part2">甲二</a></li></ol></nav></body></html>`,
		"OPS/Text/a.xhtml":       `<html><head><title>隐藏标题</title><style>隐藏样式</style></head><body><h1 id="part1">甲标题</h1><p>甲正文&amp;符号&nbsp;空格</p><h2 id="part2">第二段</h2><p>结束<br/>换行</p><script>隐藏脚本</script><svg><text>隐藏向量</text></svg></body></html>`,
		"OPS/Text/b.xhtml":       `<html><head><title>乙标题</title></head><body><p>乙<b>正文</b> first <em>inline</em> last.</p><img src="https://example.invalid/never-fetch.png"/></body></html>`,
	}
}
func TestEPUBSpineNavigationExtractionAndCache(t *testing.T) {
	t.Setenv("C1BOOK_READER_CACHE_DIR", t.TempDir())
	source := writeEPUBFixture(t, epubFixture())
	doc, err := OpenDocument(source)
	if err != nil {
		t.Fatal(err)
	}
	if doc.Path != source || doc.Encoding != EncodingUTF8 || doc.contentPath() == source {
		t.Fatal("EPUB identity or normalized source incorrect")
	}
	var titles []string
	for _, chapter := range doc.Chapters {
		titles = append(titles, chapter.Title)
	}
	if !reflect.DeepEqual(titles, []string{"乙章", "甲一", "甲二"}) {
		t.Fatalf("navigation = %v", titles)
	}
	raw, _, err := doc.ReadRange(0, doc.Size, 0)
	if err != nil {
		t.Fatal(err)
	}
	text := string(raw)
	if !strings.HasPrefix(text, "乙正文 first inline last.") || !strings.Contains(text, "甲正文&符号 空格") || !strings.Contains(text, "结束\n换行") || strings.Contains(text, "隐藏") {
		t.Fatalf("bad synthetic extraction: %q", text)
	}
	for _, chapter := range doc.Chapters {
		pages, end, err := Paginate(doc, chapter, chapter.Start, fixedFace{}, 3, 10)
		if err != nil || len(pages) == 0 || end != chapter.End {
			t.Fatal("EPUB pagination failed")
		}
		aligned, err := doc.AlignLineStart(chapter.End-1, chapter.Start)
		if err != nil || aligned < chapter.Start || aligned >= chapter.End {
			t.Fatal("EPUB line alignment failed")
		}
	}
	testIngestionPersistence(t, doc)
	cached, err := OpenDocument(source)
	if err != nil || !reflect.DeepEqual(doc.Chapters, cached.Chapters) {
		t.Fatal("EPUB index cache changed")
	}
	if err := os.Remove(doc.contentPath()); err != nil {
		t.Fatal(err)
	}
	rebuilt, err := OpenDocument(source)
	if err != nil || !reflect.DeepEqual(doc.Chapters, rebuilt.Chapters) {
		t.Fatal("missing EPUB text cache was not rebuilt")
	}
	entries, err := os.ReadDir(filepath.Dir(source))
	if err != nil {
		t.Fatal(err)
	}
	if len(entries) != 1 {
		t.Fatal("cache was written beside original EPUB")
	}
}
func TestEPUBNCXAndRelativeReferences(t *testing.T) {
	t.Setenv("C1BOOK_READER_CACHE_DIR", t.TempDir())
	files := epubFixture()
	files["OPS/package.opf"] = `<package><manifest><item id="a" href="Text/a.xhtml" media-type="application/xhtml+xml"/><item id="toc" href="Nav/toc.ncx" media-type="application/x-dtbncx+xml"/></manifest><spine toc="toc"><itemref idref="a"/></spine></package>`
	files["OPS/Nav/toc.ncx"] = `<ncx><navMap><navPoint><navLabel><text>第一部分</text></navLabel><content src="../Text/a.xhtml#part1"/><navPoint><navLabel><text>第二部分</text></navLabel><content src="../Text/a.xhtml#part2"/></navPoint></navPoint></navMap></ncx>`
	doc, err := OpenDocument(writeEPUBFixture(t, files))
	if err != nil {
		t.Fatal(err)
	}
	if len(doc.Chapters) != 2 || doc.Chapters[0].Title != "第一部分" || doc.Chapters[1].Title != "第二部分" {
		t.Fatalf("NCX chapters=%v", doc.Chapters)
	}
}
func TestEPUBInvalidInputs(t *testing.T) {
	cases := map[string]func(map[string]string){
		"traversal":         func(f map[string]string) { f["../escape.txt"] = "bad" },
		"absolute":          func(f map[string]string) { f["/escape.txt"] = "bad" },
		"backslash":         func(f map[string]string) { f[`OPS\escape.txt`] = "bad" },
		"drive":             func(f map[string]string) { f["C:/escape.txt"] = "bad" },
		"missing-container": func(f map[string]string) { delete(f, "META-INF/container.xml") },
		"broken-container":  func(f map[string]string) { f["META-INF/container.xml"] = "<container>" },
		"broken-package":    func(f map[string]string) { f["OPS/package.opf"] = "<package>" },
		"empty-spine":       func(f map[string]string) { f["OPS/package.opf"] = "<package/>" },
		"missing-resource":  func(f map[string]string) { delete(f, "OPS/Text/b.xhtml") },
		"missing-idref": func(f map[string]string) {
			f["OPS/package.opf"] = strings.Replace(f["OPS/package.opf"], `idref="b"`, `idref="missing"`, 1)
		},
		"remote-spine": func(f map[string]string) {
			f["OPS/package.opf"] = strings.Replace(f["OPS/package.opf"], `href="Text/b.xhtml"`, `href="https://example.invalid/book"`, 1)
		},
		"escaping-spine": func(f map[string]string) {
			f["OPS/package.opf"] = strings.Replace(f["OPS/package.opf"], `href="Text/b.xhtml"`, `href="../../escape.xhtml"`, 1)
		},
		"encoded-escaping-spine": func(f map[string]string) {
			f["OPS/package.opf"] = strings.Replace(f["OPS/package.opf"], `href="Text/b.xhtml"`, `href="%2e%2e/%2e%2e/escape.xhtml"`, 1)
		},
		"encrypted": func(f map[string]string) { f["META-INF/encryption.xml"] = "<encryption/>" },
		"unsupported-spine": func(f map[string]string) {
			f["OPS/package.opf"] = strings.ReplaceAll(f["OPS/package.opf"], `application/xhtml+xml`, `image/png`)
		},
		"empty-text": func(f map[string]string) {
			f["OPS/Text/a.xhtml"] = "<html><body/></html>"
			f["OPS/Text/b.xhtml"] = "<html><body/></html>"
		},
		"nesting": func(f map[string]string) {
			f["OPS/Text/b.xhtml"] = "<html><body>" + strings.Repeat("<div>", 300) + "body" + strings.Repeat("</div>", 300) + "</body></html>"
		},
	}
	for name, change := range cases {
		t.Run(name, func(t *testing.T) {
			t.Setenv("C1BOOK_READER_CACHE_DIR", t.TempDir())
			files := epubFixture()
			change(files)
			if _, err := OpenDocument(writeEPUBFixture(t, files)); err == nil {
				t.Fatal("invalid EPUB accepted")
			}
			entries, err := os.ReadDir(documentCacheDir())
			if err != nil && !os.IsNotExist(err) {
				t.Fatal(err)
			}
			if len(entries) != 0 {
				t.Fatal("failed conversion left cache files")
			}
		})
	}
}
func TestEPUBArchiveLimits(t *testing.T) {
	cases := []struct {
		name                     string
		count                    int
		uncompressed, compressed uint64
	}{
		{"entry-count", maxEPUBEntries + 1, 0, 0},
		{"entry-size", 1, maxEPUBEntryBytes + 1, maxEPUBEntryBytes + 1},
		{"total-expansion", 9, maxEPUBEntryBytes, maxEPUBEntryBytes},
		{"ratio", 1, 2 << 20, 1},
	}
	for _, test := range cases {
		t.Run(test.name, func(t *testing.T) {
			name := filepath.Join(t.TempDir(), "limit.epub")
			file, err := os.Create(name)
			if err != nil {
				t.Fatal(err)
			}
			writer := zip.NewWriter(file)
			for i := 0; i < test.count; i++ {
				_, err := writer.CreateRaw(&zip.FileHeader{Name: fmt.Sprintf("entry-%d", i), Method: zip.Store, UncompressedSize64: test.uncompressed, CompressedSize64: test.compressed})
				if err != nil {
					t.Fatal(err)
				}
			}
			if err := writer.Close(); err != nil {
				t.Fatal(err)
			}
			file.Close()
			reader, err := zip.OpenReader(name)
			if err != nil {
				t.Fatal(err)
			}
			defer reader.Close()
			if _, err := openEPUBArchive(reader); err == nil {
				t.Fatal("unsafe ZIP limits accepted")
			}
		})
	}
}
func TestDocumentDecoratedGBHeadingsAndMalformedOffsets(t *testing.T) {
	t.Setenv("C1BOOK_READER_CACHE_DIR", t.TempDir())
	source := "〖第一篇 测试 〗第一章 开始\r\n甲乙正文\r\n〖第一篇 测试 〗二章继续\r\n结尾\r\n"
	encoded, _, err := transform.Bytes(simplifiedchinese.GB18030.NewEncoder(), []byte(source))
	if err != nil {
		t.Fatal(err)
	}
	name := filepath.Join(t.TempDir(), "decorated.txt")
	if err := os.WriteFile(name, encoded, 0600); err != nil {
		t.Fatal(err)
	}
	doc, err := OpenDocument(name)
	if err != nil {
		t.Fatal(err)
	}
	if len(doc.Chapters) != 2 || doc.Chapters[0].Start != 0 || doc.Size != int64(len(encoded)) {
		t.Fatal("decorated GB headings not recognized with original offsets")
	}
	pages, _, err := Paginate(doc, doc.Chapters[0], 0, fixedFace{}, 40, 240)
	if err != nil || len(pages) != 1 || strings.Contains(strings.Join(pages[0].Lines, ""), "第一章") {
		t.Fatal("decorated heading was not suppressed in body")
	}
	// Invalid lead byte + ASCII: replacement occupies one source byte, not the
	// four GB18030 bytes that re-encoding U+FFFD would produce.
	malformed := []byte{0xff, 'A', 0x80, 'B', '\n'}
	if err := os.WriteFile(name, malformed, 0600); err != nil {
		t.Fatal(err)
	}
	doc, err = OpenDocument(name)
	if err != nil {
		t.Fatal(err)
	}
	pages, _, err = Paginate(doc, doc.Chapters[0], 0, fixedFace{}, 1, 10)
	if err != nil {
		t.Fatal(err)
	}
	var starts []int64
	for _, page := range pages {
		starts = append(starts, page.Start)
	}
	if !reflect.DeepEqual(starts, []int64{0, 1, 2, 3}) {
		t.Fatalf("malformed/noncanonical GB offsets=%v", starts)
	}
}
func TestDocumentUTF16AndEncodingErrors(t *testing.T) {
	for _, big := range []bool{false, true} {
		t.Run(fmt.Sprint("bigEndian=", big), func(t *testing.T) {
			t.Setenv("C1BOOK_READER_CACHE_DIR", t.TempDir())
			var order binary.ByteOrder = binary.LittleEndian
			bom := []byte{0xff, 0xfe}
			if big {
				order = binary.BigEndian
				bom = []byte{0xfe, 0xff}
			}
			raw := append([]byte(nil), bom...)
			for _, unit := range utf16.Encode([]rune("第一章 开始\r\n甲😀乙\n第二章 继续\n结束")) {
				var b [2]byte
				order.PutUint16(b[:], unit)
				raw = append(raw, b[:]...)
			}
			source := filepath.Join(t.TempDir(), "utf16.txt")
			if err := os.WriteFile(source, raw, 0600); err != nil {
				t.Fatal(err)
			}
			doc, err := OpenDocument(source)
			if err != nil {
				t.Fatal(err)
			}
			if doc.Path != source || doc.Encoding != EncodingUTF8 || len(doc.Chapters) != 2 || doc.dataPath == "" {
				t.Fatal("UTF16 normalization failed")
			}
			text, _, err := doc.ReadRange(0, doc.Size, 0)
			if err != nil || !bytes.Contains(text, []byte("甲😀乙")) {
				t.Fatal("UTF16 text extraction failed")
			}
			testIngestionPersistence(t, doc)
		})
	}
	for name, raw := range map[string][]byte{"truncated-utf16": {0xff, 0xfe, 0x41}, "utf32": {0xff, 0xfe, 0, 0}, "invalid-bom-utf8": {0xef, 0xbb, 0xbf, 0xff}, "nul-binary": {'a', 0, 'b'}} {
		t.Run(name, func(t *testing.T) {
			t.Setenv("C1BOOK_READER_CACHE_DIR", t.TempDir())
			source := filepath.Join(t.TempDir(), "invalid.txt")
			if err := os.WriteFile(source, raw, 0600); err != nil {
				t.Fatal(err)
			}
			if _, err := OpenDocument(source); err == nil {
				t.Fatal("invalid encoding accepted")
			}
		})
	}
}
func TestDocumentSourceOffsetRecovery(t *testing.T) {
	t.Setenv("C1BOOK_READER_CACHE_DIR", t.TempDir())
	text := "〖第一篇 测试〗第一章 开始\n正文\n〖第一篇 测试〗第二章 继续\n末尾\n"
	source := filepath.Join(t.TempDir(), "indexed.txt")
	if err := os.WriteFile(source, []byte(text), 0600); err != nil {
		t.Fatal(err)
	}
	doc, err := OpenDocument(source)
	if err != nil {
		t.Fatal(err)
	}
	offset := int64(strings.Index(text, "末尾"))
	index, ok := doc.ChapterAtOffset(offset)
	if !ok || index != 1 {
		t.Fatal("old single-chapter source offset could not be recovered")
	}
	for _, offset := range []int64{-1, doc.Size, doc.Size + 1} {
		if _, ok := doc.ChapterAtOffset(offset); ok {
			t.Fatal("invalid offset accepted")
		}
	}
}

func TestDocumentWindowCharacterBoundaries(t *testing.T) {
	for name, raw := range map[string][]byte{"UTF8": []byte("甲乙😀"), "GB18030": {0xbc, 0xd7, 0xd2, 0xd2, 0x94, 0x39, 0xfc, 0x36}} {
		encoding := EncodingUTF8
		if name == "GB18030" {
			encoding = EncodingGB18030
		}
		for cut := 1; cut < len(raw); cut++ {
			complete := completeTextWindow(raw[:cut], encoding)
			decoded, err := decodeBytes(complete, encoding)
			if err != nil || strings.ContainsRune(decoded, '\ufffd') {
				t.Fatalf("%s cut %d split a character", name, cut)
			}
		}
	}
	t.Setenv("C1BOOK_READER_CACHE_DIR", t.TempDir())
	source := filepath.Join(t.TempDir(), "long.txt")
	raw := []byte(strings.Repeat("甲", int(maxChapterWindow)/3+2))
	if err := os.WriteFile(source, raw, 0600); err != nil {
		t.Fatal(err)
	}
	doc, err := OpenDocument(source)
	if err != nil {
		t.Fatal(err)
	}
	// A wide synthetic face avoids millions of line allocations; the byte-window
	// boundary still falls inside a three-byte UTF-8 character.
	_, end, err := Paginate(doc, doc.Chapters[0], 0, fixedFace{}, len(raw), 240)
	if err != nil || end%3 != 0 || end >= doc.Size {
		t.Fatal("long UTF8 window did not end at a complete character")
	}
	_, last, err := Paginate(doc, doc.Chapters[0], end, fixedFace{}, 40, 240)
	if err != nil || last != doc.Size {
		t.Fatal("long UTF8 window failed to continue")
	}
}

func TestLibraryIncludesEPUB(t *testing.T) {
	root := t.TempDir()
	for _, name := range []string{"a.EPUB", "b.txt", "c.zip"} {
		if err := os.WriteFile(filepath.Join(root, name), nil, 0600); err != nil {
			t.Fatal(err)
		}
	}
	books, err := ScanLibrary(root)
	if err != nil || len(books) != 2 || books[0].Name != "a.EPUB" {
		t.Fatal("EPUB was not discovered")
	}
}
