package main

import (
	"golang.org/x/text/encoding/simplifiedchinese"
	"golang.org/x/text/transform"
	"unicode/utf8"
)

// Window limits may bisect a multi-byte character on a very long line.
func completeTextWindow(raw []byte, encoding Encoding) []byte {
	if encoding == EncodingUTF8 {
		start := len(raw) - 1
		for start > 0 && !utf8.RuneStart(raw[start]) {
			start--
		}
		if start >= 0 && !utf8.FullRune(raw[start:]) {
			return raw[:start]
		}
		return raw
	}
	decoder := simplifiedchinese.GB18030.NewDecoder()
	var output [4096]byte
	consumed := 0
	for consumed < len(raw) {
		_, n, err := decoder.Transform(output[:], raw[consumed:], false)
		consumed += n
		if err != transform.ErrShortDst {
			break
		}
	}
	return raw[:consumed]
}

// Invalid GB bytes decode to U+FFFD and some valid characters have noncanonical
// encodings. Re-encoding either loses source-byte lengths, breaking bookmarks.
// This slow path records the actual decoder consumption, one rune at a time.
func gb18030SourceOffsets(raw []byte) []int {
	decoder := simplifiedchinese.GB18030.NewDecoder()
	offsets := []int{0}
	source := 0
	var output [4]byte
	for len(raw) > 0 {
		written, consumed := 0, 0
		for capacity := 1; capacity <= len(output); capacity++ {
			written, consumed, _ = decoder.Transform(output[:capacity], raw, true)
			if consumed > 0 {
				break
			}
		}
		if consumed == 0 {
			return nil
		}
		source += consumed
		for i := 0; i < written; i++ {
			offsets = append(offsets, source)
		}
		raw = raw[consumed:]
	}
	return offsets
}
