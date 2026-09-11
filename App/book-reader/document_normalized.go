package main

import (
	"errors"
	"io"
	"os"

	textunicode "golang.org/x/text/encoding/unicode"
	"golang.org/x/text/transform"
)

const maxNormalizedTextBytes int64 = 256 << 20

func (document *Document) contentPath() string {
	if document.dataPath != "" {
		return document.dataPath
	}
	return document.Path
}

func openUTF16Document(path string, source *os.File, mark BookFingerprint, encoding Encoding, bom int64) (*Document, error) {
	if (mark.Size-bom)%2 != 0 {
		return nil, errors.New("truncated UTF-16 code unit")
	}
	if _, err := source.Seek(bom, io.SeekStart); err != nil {
		return nil, err
	}
	order := textunicode.LittleEndian
	if encoding == EncodingUTF16BE {
		order = textunicode.BigEndian
	}
	decoder := textunicode.UTF16(order, textunicode.IgnoreBOM).NewDecoder()
	destination := normalizedDocumentPath(path, mark)
	err := writePrivateCache(destination, func(w io.Writer) error {
		n, err := io.Copy(w, io.LimitReader(transform.NewReader(source, decoder), maxNormalizedTextBytes+1))
		if err != nil {
			return err
		}
		if n > maxNormalizedTextBytes {
			return errors.New("normalized text exceeds size limit")
		}
		return nil
	})
	if err != nil {
		return nil, err
	}
	file, err := os.Open(destination)
	if err != nil {
		return nil, err
	}
	defer file.Close()
	info, err := file.Stat()
	if err != nil {
		return nil, err
	}
	chapters, err := scanChapters(file, info.Size(), EncodingUTF8, 0)
	if err != nil {
		return nil, err
	}
	return finishNormalizedDocument(path, mark, info.Size(), chapters)
}

func finishNormalizedDocument(path string, mark BookFingerprint, size int64, chapters []Chapter) (*Document, error) {
	document := &Document{Path: path, Encoding: EncodingUTF8, Size: size, Chapters: chapters, dataPath: normalizedDocumentPath(path, mark)}
	// A cache index write failure need not prevent reading successfully converted text.
	_ = saveChapterIndexCache(path, chapterIndexCache{Version: chapterIndexCacheVersion, Path: path, Fingerprint: mark, Encoding: EncodingUTF8, Chapters: chapters, Normalized: true, ContentSize: size})
	return document, nil
}
