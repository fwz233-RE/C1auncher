package main

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"path/filepath"
)

// v3 recognizes decorated TXT headings and stores all derived data privately.
// Existing UTF-8/GB18030 byte offsets and original book identities are unchanged.
const chapterIndexCacheVersion = 3

type chapterIndexCache struct {
	Version     int             `json:"version"`
	Path        string          `json:"path"`
	Fingerprint BookFingerprint `json:"fingerprint"`
	Encoding    Encoding        `json:"encoding"`
	Chapters    []Chapter       `json:"chapters"`
	Normalized  bool            `json:"normalized,omitempty"`
	ContentSize int64           `json:"contentSize,omitempty"`
}

// An explicit directory also allows read-only integration tests without touching
// books or the normal device cache. Only generated hash names are written here.
func documentCacheDir() string {
	if root := os.Getenv("C1BOOK_READER_CACHE_DIR"); root != "" {
		return root
	}
	root, err := os.UserCacheDir()
	if err != nil {
		root = os.TempDir()
	}
	return filepath.Join(root, "c1book-reader", "documents")
}

func documentCacheKey(bookPath string) string {
	absolute, err := filepath.Abs(bookPath)
	if err != nil {
		absolute = filepath.Clean(bookPath)
	}
	sum := sha256.Sum256([]byte(absolute))
	return hex.EncodeToString(sum[:])
}
func chapterIndexCachePath(bookPath string) string {
	return filepath.Join(documentCacheDir(), documentCacheKey(bookPath)+".json")
}
func normalizedDocumentPath(bookPath string, mark BookFingerprint) string {
	return filepath.Join(documentCacheDir(), fmt.Sprintf("%s-v%d-%d-%d.txt", documentCacheKey(bookPath), chapterIndexCacheVersion, mark.Size, mark.ModTime))
}

func loadChapterIndexCache(bookPath string, current BookFingerprint) (chapterIndexCache, bool) {
	file, err := os.Open(chapterIndexCachePath(bookPath))
	if err != nil {
		return chapterIndexCache{}, false
	}
	defer file.Close()
	data, err := io.ReadAll(io.LimitReader(file, 16<<20))
	if err != nil {
		return chapterIndexCache{}, false
	}
	var cache chapterIndexCache
	if json.Unmarshal(data, &cache) != nil || cache.Version != chapterIndexCacheVersion || cache.Path != bookPath || cache.Fingerprint != current {
		return chapterIndexCache{}, false
	}
	if cache.Encoding != EncodingUTF8 && cache.Encoding != EncodingGB18030 {
		return chapterIndexCache{}, false
	}
	size := current.Size
	if cache.Normalized {
		info, err := os.Stat(normalizedDocumentPath(bookPath, current))
		if err != nil || !info.Mode().IsRegular() || info.Size() != cache.ContentSize || cache.Encoding != EncodingUTF8 {
			return chapterIndexCache{}, false
		}
		size = cache.ContentSize
	}
	if !validChapterIndex(cache.Chapters, size) {
		return chapterIndexCache{}, false
	}
	return cache, true
}

func validChapterIndex(chapters []Chapter, size int64) bool {
	if len(chapters) == 0 || (chapters[0].Start != 0 && chapters[0].Start != 3) {
		return false
	}
	previousEnd := chapters[0].Start
	for _, chapter := range chapters {
		if chapter.Start < 0 || chapter.Start > chapter.End || chapter.End > size || chapter.Start != previousEnd {
			return false
		}
		previousEnd = chapter.End
	}
	return previousEnd == size
}

func saveChapterIndexCache(bookPath string, cache chapterIndexCache) error {
	data, err := json.Marshal(cache)
	if err != nil {
		return err
	}
	return writePrivateCache(chapterIndexCachePath(bookPath), func(w io.Writer) error { _, err := w.Write(data); return err })
}

func writePrivateCache(destination string, write func(io.Writer) error) error {
	if err := os.MkdirAll(filepath.Dir(destination), 0700); err != nil {
		return err
	}
	temporary, err := os.CreateTemp(filepath.Dir(destination), ".document-*.tmp")
	if err != nil {
		return err
	}
	name := temporary.Name()
	defer os.Remove(name)
	if err = write(temporary); err == nil {
		err = temporary.Sync()
	}
	if closeErr := temporary.Close(); err == nil {
		err = closeErr
	}
	if err != nil {
		return err
	}
	return os.Rename(name, destination)
}
