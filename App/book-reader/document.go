package main

import (
	"bufio"
	"bytes"
	"errors"
	"fmt"
	"io"
	"os"
	"regexp"
	"strings"
	"unicode/utf8"

	"golang.org/x/text/encoding/simplifiedchinese"
	"golang.org/x/text/transform"
)

type Encoding string

const (
	EncodingUTF8    Encoding = "utf-8"
	EncodingGB18030 Encoding = "gb18030"
)

type Chapter struct {
	Title string
	Start int64
	End   int64
	// HasBody distinguishes a readable volume introduction from a heading-only
	// volume. Chapter order and byte ranges remain identical to the v1 index.
	HasBody bool
}

type Document struct {
	Path     string
	Encoding Encoding
	Size     int64
	Chapters []Chapter
	outline  *chapterOutline
}

var chapterTitlePattern = regexp.MustCompile(`^第.{1,30}[章节卷回部篇](?:[：:\s　].*)?$`)
var volumeTitlePattern = regexp.MustCompile(`^(?:卷|部|篇)[一二三四五六七八九十百千万零〇0-9]+(?:[：:\s　].*)?$`)

func OpenDocument(path string) (*Document, error) {
	file, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer file.Close()
	info, err := file.Stat()
	if err != nil {
		return nil, err
	}
	mark := BookFingerprint{Size: info.Size(), ModTime: info.ModTime().UnixNano()}
	if cached, ok := loadChapterIndexCache(path, mark); ok {
		return &Document{Path: path, Encoding: cached.Encoding, Size: info.Size(), Chapters: cached.Chapters}, nil
	}
	encoding, bom, err := detectEncoding(file)
	if err != nil {
		return nil, err
	}
	if _, err := file.Seek(0, io.SeekStart); err != nil {
		return nil, err
	}
	chapters, err := scanChapters(file, info.Size(), encoding, bom)
	if err != nil {
		return nil, err
	}
	_ = saveChapterIndexCache(path, chapterIndexCache{
		Version:     chapterIndexCacheVersion,
		Path:        path,
		Fingerprint: mark,
		Encoding:    encoding,
		Chapters:    chapters,
	})
	return &Document{Path: path, Encoding: encoding, Size: info.Size(), Chapters: chapters}, nil
}

func detectEncoding(file *os.File) (Encoding, int64, error) {
	if _, err := file.Seek(0, io.SeekStart); err != nil {
		return "", 0, err
	}
	reader := bufio.NewReaderSize(file, 64*1024)
	bom := int64(0)
	prefix, err := reader.Peek(3)
	if err == nil && bytes.Equal(prefix, []byte{0xef, 0xbb, 0xbf}) {
		_, _ = reader.Discard(3)
		bom = 3
	}
	for {
		r, size, err := reader.ReadRune()
		if errors.Is(err, io.EOF) {
			return EncodingUTF8, bom, nil
		}
		if err != nil {
			return "", 0, err
		}
		if r == utf8.RuneError && size == 1 {
			return EncodingGB18030, 0, nil
		}
	}
}

func scanChapters(file io.Reader, size int64, encoding Encoding, bom int64) ([]Chapter, error) {
	const maxTitleLineBytes = 256 << 10
	reader := bufio.NewReaderSize(file, 128*1024)
	chapters := make([]Chapter, 0, 128)
	lineStart, offset := int64(0), int64(0)
	preambleHasBody := false
	line := make([]byte, 0, 1024)
	for {
		fragment, err := reader.ReadSlice('\n')
		offset += int64(len(fragment))
		if line != nil {
			if len(line)+len(fragment) <= maxTitleLineBytes {
				line = append(line, fragment...)
			} else {
				line = nil
			}
		}
		if errors.Is(err, bufio.ErrBufferFull) {
			continue
		}
		if line != nil && len(line) > 0 {
			content := line
			titleStart := lineStart
			if lineStart == 0 && bom > 0 && int64(len(content)) >= bom {
				content = content[bom:]
				titleStart += bom
			}
			text, decodeErr := decodeBytes(content, encoding)
			if decodeErr != nil {
				return nil, fmt.Errorf("decode line at %d: %w", titleStart, decodeErr)
			}
			title := strings.TrimSpace(text)
			if isChapterTitle(title) {
				if len(chapters) == 0 && titleStart > bom {
					chapters = append(chapters, Chapter{Title: "开始", Start: bom, End: titleStart, HasBody: preambleHasBody})
				}
				if len(chapters) > 0 {
					chapters[len(chapters)-1].End = titleStart
				}
				chapters = append(chapters, Chapter{Title: title, Start: titleStart, End: size})
			} else if meaningfulBodyLine(title) {
				if len(chapters) > 0 {
					chapters[len(chapters)-1].HasBody = true
				} else {
					preambleHasBody = true
				}
			}
		} else if line == nil {
			// Oversized lines cannot be headings; retain them as body content.
			if len(chapters) > 0 {
				chapters[len(chapters)-1].HasBody = true
			} else {
				preambleHasBody = true
			}
		}
		lineStart = offset
		line = make([]byte, 0, 1024)
		if errors.Is(err, io.EOF) {
			break
		}
		if err != nil {
			return nil, err
		}
	}
	if len(chapters) == 0 {
		chapters = append(chapters, Chapter{Title: "正文", Start: bom, End: size, HasBody: preambleHasBody})
	} else {
		chapters[len(chapters)-1].End = size
	}
	return chapters, nil
}

// Common TXT export separators do not constitute a volume introduction.
// Their bytes are retained in the original section, not removed from the book.
func meaningfulBodyLine(text string) bool {
	return strings.Trim(text, " \t\r\n　-—–_=＝~～*＊·•─━") != ""
}

func isChapterTitle(text string) bool {
	if text == "" || utf8.RuneCountInString(text) > 80 {
		return false
	}
	return chapterTitlePattern.MatchString(text) || volumeTitlePattern.MatchString(text)
}

func decodeBytes(raw []byte, encoding Encoding) (string, error) {
	raw = bytes.TrimSuffix(raw, []byte{'\n'})
	raw = bytes.TrimSuffix(raw, []byte{'\r'})
	if encoding == EncodingUTF8 {
		if !utf8.Valid(raw) {
			return "", errors.New("invalid UTF-8")
		}
		return string(raw), nil
	}
	decoded, err := io.ReadAll(transform.NewReader(bytes.NewReader(raw), simplifiedchinese.GB18030.NewDecoder()))
	if err != nil {
		return "", err
	}
	return string(decoded), nil
}

func (document *Document) AlignLineStart(offset, floor int64) (int64, error) {
	if offset <= floor {
		return floor, nil
	}
	base := offset - 64*1024
	if base < floor {
		base = floor
	}
	file, err := os.Open(document.Path)
	if err != nil {
		return floor, err
	}
	defer file.Close()
	buffer := make([]byte, offset-base)
	n, err := file.ReadAt(buffer, base)
	if err != nil && !errors.Is(err, io.EOF) {
		return floor, err
	}
	if newline := bytes.LastIndexByte(buffer[:n], '\n'); newline >= 0 {
		return base + int64(newline+1), nil
	}
	return floor, nil
}

func (document *Document) ReadRange(start, end int64, limit int64) ([]byte, int64, error) {
	if start < 0 {
		start = 0
	}
	if end > document.Size {
		end = document.Size
	}
	if end < start {
		end = start
	}
	if limit > 0 && end-start > limit {
		end = start + limit
	}
	file, err := os.Open(document.Path)
	if err != nil {
		return nil, start, err
	}
	defer file.Close()
	buffer := make([]byte, end-start)
	n, err := file.ReadAt(buffer, start)
	if err != nil && !errors.Is(err, io.EOF) {
		return nil, start, err
	}
	return buffer[:n], start + int64(n), nil
}
