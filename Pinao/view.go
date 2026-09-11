package main

import "time"

// Only values that affect visible pixels belong here. Keeping this comparable
// lets the main loop check expiry/progress without allocating or rasterizing an
// unchanged screen. Held/recent keys are visual only; audio uses model.Held.
type viewState struct {
	Tone, BPM, Octave, Volume int
	Playing, Recording, Help  bool
	StepMode                  bool
	Management                bool
	ManagerIndex              int
	ManagerCount              int
	ManagerSelectedWAV        bool
	ManagerNames              [6]string
	Page, PageCount           int
	Step                      int
	Ties                      uint16 // Bits mark continuations into each cell, including the previous page.
	TieOut                    bool
	Pattern, Keys, Notes      uint16
	Drums                     uint8
	Footer                    string
}

func (m *model) view(now time.Time) viewState {
	v := viewState{Tone: m.Song.Tone, Help: m.Help, Page: m.Page, PageCount: m.Song.pageCount(), Management: m.Management, ManagerIndex: m.ManagerIndex, ManagerCount: len(m.ManagerItems)}
	if m.Management {
		if len(m.ManagerItems) > 0 {
			v.ManagerSelectedWAV = m.ManagerItems[m.ManagerIndex].IsWAV
		}
		for i := 0; i < min(len(m.ManagerItems), len(v.ManagerNames)); i++ {
			v.ManagerNames[i] = archiveDisplayName(m.ManagerItems[i].Path)
		}
		return v
	}
	if m.Help {
		return v // Hidden playback/feedback must not redraw the help screen.
	}
	v.BPM, v.Octave, v.Volume = m.Song.BPM, m.Song.Octave, m.Song.Volume
	v.Playing, v.Recording, v.Step = m.Playing, m.Recording, m.Step
	v.StepMode = m.StepMode
	for i, hits := range *m.currentPattern() {
		for _, h := range hits {
			if h.Tie {
				v.Ties |= 1 << uint(i)
			}
		}
		if len(hits) > 0 {
			v.Pattern |= 1 << uint(i)
		}
	}
	if m.Page+1 < m.Song.pageCount() {
		for _, h := range m.Song.patternAt(m.Page + 1)[0] {
			if h.Tie {
				v.TieOut = true
			}
		}
	}
	for i, until := range m.Feedback {
		if now.Before(until) {
			v.Keys |= 1 << uint(i)
		}
	}
	for code := range m.Held {
		if n, ok := keyNote(code); ok {
			v.Keys |= 1 << uint(n)
		}
	}
	if m.StepMode && m.Step >= 0 && m.Step < steps {
		// In manual mode the note/drum strip describes this cell, not recent
		// activity in the cell we just left. Keyboard highlights still audition taps.
		for _, h := range m.currentPattern()[m.Step] {
			if h.Drum > 0 {
				v.Drums |= 1 << uint(h.Drum-1)
			} else {
				n := h.MIDI % 12
				if h.MIDI == 12*(m.Song.Octave+1)+12 {
					n = 12
				}
				v.Notes |= 1 << uint(n)
			}
		}
	} else {
		for _, s := range m.Sparks {
			if now.Before(s.At) || now.Sub(s.At) >= keyFeedbackDuration {
				continue
			}
			if s.Drum {
				if s.Key >= 0 && s.Key < 4 {
					v.Drums |= 1 << uint(s.Key)
				}
			} else if s.Key >= 0 && s.Key < 13 {
				v.Notes |= 1 << uint(s.Key)
			}
		}
	}
	v.Footer = "SPACE LOOP  R REC  Q TONE  L HELP"
	if m.StepMode {
		v.Footer = "L/R STEP  R REC  SPACE PLAY"
		if m.Recording {
			v.Footer = "L/R STEP  SPACE AUTO  R END REC"
		}
	} else if m.Recording {
		v.Footer = "R END REC  L/R STEP  SPACE STOP"
	}
	if now.Before(m.NoticeUntil) {
		v.Footer = m.Notice
	} else if m.Dirty && !m.StepMode && !m.Recording {
		v.Footer = "EDITED - AUTO SAVE   ENTER: SAVE"
	}
	return v
}
