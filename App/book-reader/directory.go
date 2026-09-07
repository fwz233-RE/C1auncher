package main

import (
	"fmt"

	"c1device"
)

type directoryRow struct {
	chapter    int // Stable raw chapter index, never a persisted UI row index.
	volumeBody bool
	label      string
}

func (app *readerApp) directoryRows() ([]directoryRow, int) {
	if app.document == nil {
		return nil, 0
	}
	outline := app.document.chapterOutline()
	rows := make([]directoryRow, 0, len(outline.entries))
	selected := 0
	appendRow := func(row directoryRow) {
		if row.chapter == app.chapterPick && row.volumeBody == app.volumeBodyPick {
			selected = len(rows)
		}
		rows = append(rows, row)
	}
	for index, entry := range outline.entries {
		chapter := app.document.Chapters[index]
		if entry.volume && outline.grouped {
			if entry.parent >= 0 {
				if app.expandedVolumes[entry.parent] && chapter.HasBody {
					appendRow(directoryRow{chapter: index, volumeBody: true, label: "    卷内正文"})
				}
				continue
			}
			marker := "+ "
			if app.expandedVolumes[index] {
				marker = "− "
			}
			appendRow(directoryRow{chapter: index, label: fmt.Sprintf("%s%s (%d章)", marker, chapter.Title, entry.chapterCount)})
			if app.expandedVolumes[index] && chapter.HasBody {
				appendRow(directoryRow{chapter: index, volumeBody: true, label: "    卷首正文"})
			}
			continue
		}
		if app.document.headingOnly(index) {
			continue
		}
		if entry.parent >= 0 && !app.expandedVolumes[entry.parent] {
			continue
		}
		label := chapter.Title
		if entry.parent >= 0 {
			label = "    " + label
		}
		appendRow(directoryRow{chapter: index, label: label})
	}
	return rows, selected
}

func (app *readerApp) moveDirectorySelection(delta int) {
	rows, selected := app.directoryRows()
	if len(rows) == 0 {
		return
	}
	row := rows[moveSelection(selected, delta, len(rows))]
	app.chapterPick, app.volumeBodyPick = row.chapter, row.volumeBody
}

func (app *readerApp) setVolumeExpanded(index int, expanded bool) {
	if app.expandedVolumes == nil {
		app.expandedVolumes = make(map[int]bool)
	}
	app.expandedVolumes[index] = expanded
}

// Reveal a reading target after resume, bookmarking, or percentage navigation.
// Its byte position and chapter ID are independent of collapsed UI rows.
func (app *readerApp) revealChapter(index int) {
	app.chapterPick, app.volumeBodyPick = index, false
	if app.document == nil || index < 0 || index >= len(app.document.Chapters) {
		return
	}
	outline := app.document.chapterOutline()
	entry := outline.entries[index]
	if !outline.grouped {
		if app.document.headingOnly(index) {
			if target, _, ok := app.document.readingTarget(index, app.document.Chapters[index].Start); ok {
				app.chapterPick = target
			}
		}
		return
	}
	if !entry.volume && app.document.headingOnly(index) {
		if rows, _ := app.directoryRows(); len(rows) > 0 {
			app.chapterPick = rows[0].chapter
		}
		return
	}
	if entry.volume && app.document.Chapters[index].HasBody {
		parent := index
		if entry.parent >= 0 {
			parent = entry.parent
		}
		app.setVolumeExpanded(parent, true)
		app.volumeBodyPick = true
	} else if entry.parent >= 0 {
		app.setVolumeExpanded(entry.parent, true)
		if entry.volume {
			app.chapterPick = entry.parent
		}
	}
}

func (app *readerApp) selectedVolumeHeader() bool {
	if app.document == nil || app.chapterPick < 0 || app.chapterPick >= len(app.document.Chapters) {
		return false
	}
	outline := app.document.chapterOutline()
	entry := outline.entries[app.chapterPick]
	return outline.grouped && entry.volume && entry.parent < 0 && !app.volumeBodyPick
}

func (app *readerApp) activateDirectorySelection() {
	if app.selectedVolumeHeader() {
		app.setVolumeExpanded(app.chapterPick, !app.expandedVolumes[app.chapterPick])
		return
	}
	if app.openChapter(app.chapterPick, app.resumeOffset) {
		app.readerOrigin = viewChapters
		app.resumeOffset = 0
	}
}

func (app *readerApp) directoryCollapseTarget() (int, bool) {
	if app.document == nil || app.chapterPick < 0 || app.chapterPick >= len(app.document.Chapters) {
		return 0, false
	}
	outline := app.document.chapterOutline()
	if !outline.grouped {
		return 0, false
	}
	entry := outline.entries[app.chapterPick]
	if entry.volume && app.expandedVolumes[app.chapterPick] {
		return app.chapterPick, true
	}
	if entry.parent >= 0 {
		return entry.parent, true
	}
	return 0, false
}

func (app *readerApp) leaveDirectory() {
	if parent, ok := app.directoryCollapseTarget(); ok {
		app.setVolumeExpanded(parent, false)
		app.chapterPick, app.volumeBodyPick = parent, false
		return
	}
	app.view = viewShelf
}

func (app *readerApp) directoryHint() string {
	back, forward := "←返回", "→阅读"
	if _, ok := app.directoryCollapseTarget(); ok {
		back = "←收起"
	}
	if app.selectedVolumeHeader() {
		forward = "→展开"
		if app.expandedVolumes[app.chapterPick] {
			forward = "→收起"
		}
	}
	return "↑↓选择  " + back + "  " + forward + "  P书签"
}

func (app *readerApp) directoryCount() string {
	outline := app.document.chapterOutline()
	if outline.volumes > 0 {
		return fmt.Sprintf("%d卷/%d章", outline.volumes, outline.chapters)
	}
	if outline.chapters == 0 {
		return fmt.Sprintf("%d 节", len(app.document.Chapters))
	}
	return fmt.Sprintf("%d 章", outline.chapters)
}

func (app *readerApp) renderDirectory(canvas *c1device.Canvas) {
	title := "章节"
	if len(app.books) > 0 {
		title = app.books[app.bookIndex].Name
	}
	rows, selected := app.directoryRows()
	items := make([]string, len(rows))
	for index, row := range rows {
		items[index] = row.label
	}
	app.renderList(canvas, title, items, selected, "章", app.directoryHint())
}
