package main

import (
	"encoding/json"
	"os"
	"path/filepath"
)

// Rebuild metadata for volume introductions; reading offsets and persisted
// progress/bookmark formats do not change.
const chapterIndexCacheVersion = 2

type chapterIndexCache struct {
	Version     int             `json:"version"`
	Path        string          `json:"path"`
	Fingerprint BookFingerprint `json:"fingerprint"`
	Encoding    Encoding        `json:"encoding"`
	Chapters    []Chapter       `json:"chapters"`
}

func chapterIndexCachePath(bookPath string) string {
	return bookPath + ".chapter-index.json"
}

func loadChapterIndexCache(bookPath string, current BookFingerprint) (chapterIndexCache, bool) {
	data, err := os.ReadFile(chapterIndexCachePath(bookPath))
	if err != nil {
		return chapterIndexCache{}, false
	}
	var cache chapterIndexCache
	if err := json.Unmarshal(data, &cache); err != nil {
		return chapterIndexCache{}, false
	}
	if cache.Version != chapterIndexCacheVersion || cache.Path != bookPath || cache.Fingerprint != current {
		return chapterIndexCache{}, false
	}
	if cache.Encoding != EncodingUTF8 && cache.Encoding != EncodingGB18030 {
		return chapterIndexCache{}, false
	}
	if !validChapterIndex(cache.Chapters, current.Size) {
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
	data, err := json.MarshalIndent(cache, "", "  ")
	if err != nil {
		return err
	}
	directory := filepath.Dir(bookPath)
	temporary, err := os.CreateTemp(directory, ".chapter-index-*.tmp")
	if err != nil {
		return err
	}
	temporaryPath := temporary.Name()
	defer os.Remove(temporaryPath)
	if _, err = temporary.Write(data); err == nil {
		err = temporary.Sync()
	}
	if closeErr := temporary.Close(); err == nil {
		err = closeErr
	}
	if err != nil {
		return err
	}
	return os.Rename(temporaryPath, chapterIndexCachePath(bookPath))
}
