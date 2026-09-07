package main

import (
	"fmt"
	"regexp"
	"strings"
	"unicode"
)

// Classify existing headings without changing recognition, offsets, or the
// order of Document.Chapters. A unit in the title (e.g. 第一章 新卷) must not
// turn a chapter into a volume. 卷/部/篇 form one grouping level.
var ordinalVolumePattern = regexp.MustCompile(`^第[^章节卷回部篇]{1,30}[卷部篇](?:[：:\s　].*)?$`)

func isVolumeTitle(title string) bool {
	return ordinalVolumePattern.MatchString(title) || volumeTitlePattern.MatchString(title)
}

func normalizedVolumeTitle(title string) string {
	title = strings.TrimSpace(title)
	if number, unit, ok := volumeNumber(title); ok {
		match := numberedVolumePattern.FindStringSubmatchIndex(title)
		if match == nil {
			match = reverseNumberedVolumePattern.FindStringSubmatchIndex(title)
		}
		// Both forms end their numeric/unit prefix at the second capture.
		title = fmt.Sprintf("%s%d:%s", unit, number, strings.TrimLeft(title[match[5]:], " ：:\t　"))
	}
	return strings.Map(func(character rune) rune {
		if unicode.IsSpace(character) {
			return -1
		}
		if character == '：' {
			return ':'
		}
		return character
	}, title)
}

// Use the source volume label, not a generated ordinal: books may begin at
// 第三卷 or omit some volume headings entirely.
func shortVolumeTitle(title string) string {
	parts := strings.FieldsFunc(title, func(character rune) bool {
		return unicode.IsSpace(character) || character == ':' || character == '：'
	})
	if len(parts) > 0 {
		return parts[0]
	}
	return title
}

type outlineEntry struct {
	volume       bool
	parent       int // Raw volume chapter index, -1 for ungrouped content.
	chapterNo    int // Whole-book chapter number; zero for introductions.
	chapterCount int // Chapters owned by a volume, excluding introductions.
}

type chapterOutline struct {
	grouped  bool // Only enable hierarchy when root volume numbers are 1,2,... .
	entries  []outlineEntry
	volumes  int
	chapters int
}

// Documents are immutable after opening. Cache only the derived structure;
// expanded/collapsed UI state belongs to readerApp, not the disk cache.
func (document *Document) chapterOutline() *chapterOutline {
	if document.outline != nil {
		return document.outline
	}
	outline := &chapterOutline{entries: make([]outlineEntry, len(document.Chapters))}
	parent := -1
	for index, chapter := range document.Chapters {
		entry := outlineEntry{parent: parent}
		if isVolumeTitle(chapter.Title) {
			entry.volume = true
			// TXT exports sometimes repeat the same volume title between
			// batches of chapters. Group consecutive occurrences together,
			// while retaining every original section and its body.
			if parent < 0 || normalizedVolumeTitle(chapter.Title) != normalizedVolumeTitle(document.Chapters[parent].Title) {
				entry.parent = -1
				parent = index
				outline.volumes++
			}
		} else if chapter.Title != "开始" && chapter.Title != "正文" {
			outline.chapters++
			entry.chapterNo = outline.chapters
			if parent >= 0 {
				outline.entries[parent].chapterCount++
			}
		}
		outline.entries[index] = entry
	}
	document.validateVolumeSequence(outline)
	document.outline = outline
	return outline
}

func (document *Document) headingOnly(index int) bool {
	chapter := document.Chapters[index]
	return !chapter.HasBody && (document.chapterOutline().entries[index].volume || chapter.Title == "开始")
}

func (document *Document) readableChapter(index, direction int) (int, bool) {
	for index >= 0 && index < len(document.Chapters) {
		if !document.headingOnly(index) {
			return index, true
		}
		index += direction
	}
	return 0, false
}

// Old bookmarks may reference a heading-only volume. Keep their disk records
// intact, but open the next readable section (or the previous at book end).
func (document *Document) readingTarget(index int, offset int64) (int, int64, bool) {
	if index < 0 || index >= len(document.Chapters) {
		return 0, 0, false
	}
	if target, ok := document.readableChapter(index, 1); ok {
		if target != index {
			offset = document.Chapters[target].Start
		}
		return target, offset, true
	}
	if target, ok := document.readableChapter(index, -1); ok {
		end := document.Chapters[target].End
		if end > document.Chapters[target].Start {
			return target, end - 1, true
		}
		return target, document.Chapters[target].Start, true
	}
	return 0, 0, false
}

func (document *Document) positionLabel(index int) string {
	outline := document.chapterOutline()
	entry := outline.entries[index]
	if entry.volume {
		if !outline.grouped {
			return "正文"
		}
		if entry.parent >= 0 {
			return "卷内正文"
		}
		return "卷首正文"
	}
	if entry.chapterNo == 0 {
		return document.Chapters[index].Title
	}
	label := fmt.Sprintf("第 %d / %d 章", entry.chapterNo, outline.chapters)
	if entry.parent >= 0 {
		label = fmt.Sprintf("%s  %s", shortVolumeTitle(document.Chapters[entry.parent].Title), label)
	}
	return label
}

func (document *Document) contextualChapterTitle(index int) string {
	entry := document.chapterOutline().entries[index]
	title := document.Chapters[index].Title
	if entry.parent >= 0 {
		if entry.volume {
			title = "卷内正文"
		}
		return fmt.Sprintf("%s · %s", shortVolumeTitle(document.Chapters[entry.parent].Title), title)
	}
	return title
}
