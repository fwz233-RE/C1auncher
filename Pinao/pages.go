package main

import (
	"fmt"
	"time"
)

// Bounds keep the project small on the device and page numbers at two digits.
const maxPages = 64
const pageHoldDuration = 700 * time.Millisecond

func (s Song) pageCount() int { return 1 + len(s.Pages) }
func (s *Song) patternAt(page int) *[steps][]Hit {
	if page == 0 {
		return &s.Pattern
	}
	return &s.Pages[page-1]
}
func (s Song) clone() Song {
	out := s
	out.Pages = make([][steps][]Hit, len(s.Pages))
	for p := 0; p < s.pageCount(); p++ {
		for i, hits := range *s.patternAt(p) {
			out.patternAt(p)[i] = append([]Hit(nil), hits...)
		}
	}
	return out
}
func (m *model) currentPattern() *[steps][]Hit { return m.Song.patternAt(m.Page) }
func (m *model) clockPosition(now time.Time) int {
	total := m.Song.pageCount() * steps
	return (m.StartStep + int(m.clockTick(now)%int64(total))) % total
}
func (m *model) syncPosition(now time.Time) {
	position := m.clockPosition(now)
	m.Page, m.Step = position/steps, position%steps
}
func pageDirection(code uint16) int {
	if code == 23 {
		return -1
	} // I
	return 1 // O (24)
}
func (m *model) cancelPageGesture() {
	m.pageKey = 0
	m.pageKeyAt = time.Time{}
	m.pageKeyFired = false
}

// Page keys are resolved on release (tap) or once at the hold threshold.
// Pausing on key-down anchors insertion to the page the user is looking at,
// even when playback would otherwise cross to another page during the hold.
func (m *model) handlePageKey(e keyEvent, now time.Time) []soundCommand {
	if e.Down {
		if m.pageKey != 0 {
			if m.pageKey != e.Code {
				m.pageKeyFired = true
			} // I+O: cancel, don't insert.
			return nil
		}
		m.pageKey, m.pageKeyAt, m.pageKeyFired = e.Code, now, false
		if m.Playing {
			m.syncPosition(now)
		}
		m.Playing, m.StepMode = false, true
		if m.Step < 0 {
			m.Step = 0
		}
		m.ClearUntil = time.Time{}
		return []soundCommand{{Kind: "loop-off"}}
	}
	if e.Code != m.pageKey {
		return nil
	}
	if !m.pageKeyFired {
		if now.Sub(m.pageKeyAt) >= pageHoldDuration {
			m.insertPage(pageDirection(e.Code), now)
		} else {
			m.changePage(pageDirection(e.Code), now)
		}
	}
	m.cancelPageGesture()
	return nil
}
func (m *model) checkPageHold(now time.Time) {
	if m.pageKey != 0 && !m.pageKeyFired && now.Sub(m.pageKeyAt) >= pageHoldDuration {
		m.pageKeyFired = true
		m.insertPage(pageDirection(m.pageKey), now)
	}
}
func (m *model) changePage(delta int, now time.Time) {
	next := m.Page + delta
	if next < 0 || next >= m.Song.pageCount() {
		m.message("EDGE - HOLD I/O: INSERT PAGE", now)
		return
	}
	from := m.Page*steps + m.Step
	m.Page, m.Step = next, 0
	m.extendHeld(from, now)
	m.ClearUntil = time.Time{}
	m.pageSavePending = true
	m.message(fmt.Sprintf("PAGE %02d/%02d", m.Page+1, m.Song.pageCount()), now)
}
func (m *model) insertPage(delta int, now time.Time) {
	if m.Song.pageCount() >= maxPages {
		m.message("PAGE LIMIT: 64", now)
		return
	}
	index := m.Page
	if delta > 0 {
		index++
	}
	// Move page slice headers, preserving all note data. The inserted page is
	// blank, not a copy. Its array and all existing page arrays are independent.
	from := m.Page*steps + m.Step
	oldCount := m.Song.pageCount()
	m.Song.Pages = append(m.Song.Pages, [steps][]Hit{})
	for p := oldCount; p > index; p-- {
		*m.Song.patternAt(p) = *m.Song.patternAt(p - 1)
	}
	*m.Song.patternAt(index) = [steps][]Hit{}
	m.Song.Format = max(2, m.Song.Format)
	m.Song.repairTies()
	m.Page, m.Step = index, 0
	if delta > 0 {
		m.extendHeld(from, now)
	} else {
		m.cancelTies()
	}
	m.Playing, m.StepMode = false, true
	m.ClearUntil = time.Time{}
	m.changed(now)
	m.pageSavePending = true
	m.message(fmt.Sprintf("NEW PAGE %02d/%02d", m.Page+1, m.Song.pageCount()), now)
}
