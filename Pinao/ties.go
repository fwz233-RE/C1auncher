package main

import "time"

// Tie marks a continuation from the immediately preceding chronological cell.
// Pitch AND timbre must match. It never means a second attack or a drum roll.
func sameNote(a, b Hit) bool {
	return a.Drum == 0 && b.Drum == 0 && a.MIDI == b.MIDI && a.Tone == b.Tone
}
func noteIndex(notes []Hit, h Hit) int {
	for i, n := range notes {
		if sameNote(n, h) {
			return i
		}
	}
	return -1
}
func (s *Song) cell(position int) []Hit { return s.patternAt(position / steps)[position%steps] }

type heldCapture struct {
	Hit      Hit
	Position int
	Valid    bool
}

func (m *model) cancelTies() { m.captures = [128]heldCapture{} }

// Editing backwards and wrapping within a page are navigation, never a tie
// across a nonchronological boundary. Live-held notes retain their original tone.
func (m *model) extendHeld(from int, now time.Time) {
	to := m.Page*steps + m.Step
	if !m.Recording || !m.StepMode || to != from+1 {
		m.cancelTies()
		return
	}
	for code := range m.captures { // Stable order when a chord fills a cell.
		c := &m.captures[code]
		if _, down := m.Held[uint16(code)]; !down || !c.Valid || c.Position != from {
			c.Valid = false
			continue
		}
		if noteIndex(m.Song.cell(from), c.Hit) < 0 {
			c.Valid = false
			continue
		}
		cell := &m.currentPattern()[m.Step]
		if i := noteIndex(*cell, c.Hit); i >= 0 {
			// Preserve an independently recorded attack rather than merging it.
			c.Valid = (*cell)[i].Tie
			c.Position = to
			continue
		}
		if len(*cell) >= voiceCount {
			c.Valid = false
			m.message("STEP FULL - TIE NOT ADDED", now)
			continue
		}
		h := c.Hit
		h.Tie = true
		*cell = append(*cell, h)
		m.Song.Format = 3
		c.Position = to
		m.changed(now)
	}
}

// After removing cells or inserting pages, a detached continuation becomes an
// ordinary attack. Later cells can still continue that attack; no dangling link
// is ever drawn across a gap, or from the end of the song into its beginning.
func (s *Song) repairTies() {
	for pos := 0; pos < s.pageCount()*steps; pos++ {
		for i, h := range s.cell(pos) {
			if h.Tie && (pos == 0 || noteIndex(s.cell(pos-1), h) < 0) {
				s.cell(pos)[i].Tie = false
			}
		}
	}
}

// loopTransport assigns stable IDs to continued notes even when chord ordering
// changes. Both the live sequencer and WAV export use this bounded planner.
// Without ties it retains the exact historical commands and ID ordering.
type loopTransport struct {
	notes    [voiceCount]Hit
	active   [voiceCount]bool
	position int
	valid    bool
}

func (p *loopTransport) commands(dst []soundCommand, hits []Hit, position int, contiguous bool) []soundCommand {
	var slots [voiceCount]int
	var used [voiceCount]bool
	var keep uint8
	for i := range slots {
		slots[i] = -1
	}
	if contiguous && p.valid && position == p.position+1 {
		for i, h := range hits {
			if !h.Tie || h.Drum != 0 {
				continue
			}
			for j, old := range p.notes {
				if p.active[j] && !used[j] && sameNote(old, h) {
					slots[i], used[j] = j, true
					keep |= 1 << uint(j)
					break
				}
			}
		}
	}
	for i := range hits {
		if slots[i] >= 0 {
			continue
		}
		for j := range used {
			if !used[j] {
				slots[i], used[j] = j, true
				break
			}
		}
	}
	dst = append(dst, soundCommand{Kind: "loop-off", Keep: keep})
	p.active = [voiceCount]bool{}
	for i, h := range hits {
		slot := slots[i]
		c := soundCommand{Kind: "on", ID: 300 + slot, MIDI: h.MIDI, Tone: h.Tone}
		if h.Drum > 0 {
			c.Kind, c.Drum = "drum", h.Drum-1
		} else if keep&(1<<uint(slot)) != 0 {
			c.Kind = "hold"
		}
		dst = append(dst, c)
		p.notes[slot], p.active[slot] = h, h.Drum == 0
	}
	p.position, p.valid = position, true
	return dst
}
