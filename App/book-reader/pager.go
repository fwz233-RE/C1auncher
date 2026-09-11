package main

import (
	"bytes"
	"fmt"
	"strings"

	"golang.org/x/text/encoding/simplifiedchinese"
	"golang.org/x/text/transform"
)

const maxChapterWindow int64 = 4 << 20

type TextFace interface {
	Wrap(string, int) []string
	LineHeight() int
}

type Page struct {
	Lines []string
	Start int64
	End   int64
}

type positionedLine struct {
	text       string
	start, end int64
}

func Paginate(document *Document, chapter Chapter, start int64, face TextFace, width, height int) ([]Page, int64, error) {
	if start < chapter.Start || start >= chapter.End {
		start = chapter.Start
	}
	raw, loadedEnd, err := document.ReadRange(start, chapter.End, maxChapterWindow)
	if err != nil {
		return nil, start, err
	}
	if loadedEnd < chapter.End {
		if cut := bytes.LastIndexByte(raw, '\n'); cut >= 0 {
			raw = raw[:cut+1]
		} else {
			raw = completeTextWindow(raw, document.Encoding)
		}
		loadedEnd = start + int64(len(raw))
	}
	lines := make([]positionedLine, 0, bytes.Count(raw, []byte{'\n'})+1)
	position := start
	for len(raw) > 0 {
		length := len(raw)
		if newline := bytes.IndexByte(raw, '\n'); newline >= 0 {
			length = newline + 1
		}
		part := raw[:length]
		lineStart, lineEnd := position, position+int64(length)
		text, err := decodeBytes(part, document.Encoding)
		if err != nil {
			return nil, start, fmt.Errorf("decode page at %d: %w", lineStart, err)
		}
		if lineStart == chapter.Start && isChapterTitle(strings.TrimSpace(text)) {
			raw = raw[length:]
			position = lineEnd
			continue
		}
		wrapped := face.Wrap(text, width)
		var sourceOffsets []int
		if document.Encoding == EncodingGB18030 {
			original := bytes.TrimSuffix(bytes.TrimSuffix(part, []byte{'\n'}), []byte{'\r'})
			encoded, _, encodeErr := transform.Bytes(simplifiedchinese.GB18030.NewEncoder(), []byte(text))
			if encodeErr != nil || !bytes.Equal(encoded, original) {
				sourceOffsets = gb18030SourceOffsets(original)
			}
		}
		decodedOffset := 0
		if len(wrapped) == 0 {
			wrapped = []string{""}
		}
		segmentStart := lineStart
		for index, wrappedLine := range wrapped {
			segmentEnd := lineEnd
			if index+1 < len(wrapped) {
				encodedLength := len([]byte(wrappedLine))
				if document.Encoding == EncodingGB18030 {
					if encoded, _, encodeErr := transform.Bytes(simplifiedchinese.GB18030.NewEncoder(), []byte(wrappedLine)); encodeErr == nil {
						encodedLength = len(encoded)
					}
				}
				segmentEnd = segmentStart + int64(encodedLength)
				decodedOffset += len(wrappedLine)
				if sourceOffsets != nil && decodedOffset < len(sourceOffsets) {
					segmentEnd = lineStart + int64(sourceOffsets[decodedOffset])
				}
				if segmentEnd > lineEnd {
					segmentEnd = lineEnd
				}
			}
			lines = append(lines, positionedLine{text: wrappedLine, start: segmentStart, end: segmentEnd})
			segmentStart = segmentEnd
		}
		raw = raw[length:]
		position = lineEnd
	}
	lineHeight := face.LineHeight()
	if lineHeight < 1 {
		lineHeight = 1
	}
	perPage := height / lineHeight
	if perPage < 1 {
		perPage = 1
	}
	pages := make([]Page, 0, (len(lines)+perPage-1)/perPage)
	for index := 0; index < len(lines); index += perPage {
		end := index + perPage
		if end > len(lines) {
			end = len(lines)
		}
		page := Page{Lines: make([]string, end-index), Start: lines[index].start, End: lines[end-1].end}
		for line := index; line < end; line++ {
			page.Lines[line-index] = lines[line].text
		}
		pages = append(pages, page)
	}
	if len(pages) == 0 {
		pages = append(pages, Page{Start: start, End: loadedEnd})
	}
	return pages, loadedEnd, nil
}
