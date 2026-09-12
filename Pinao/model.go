package main

import (
	"fmt"
	"strings"
	"time"
)

const steps = 16 // Eight beats, quantized to eighth notes.
const keyFeedbackDuration = 1200 * time.Millisecond

type Hit struct {
	MIDI int  `json:"midi"`
	Tone int  `json:"tone"`
	Drum int  `json:"drum"`
	Tie  bool `json:"tie,omitempty"` // Continue the same pitch/tone from the preceding cell (format 3).
}
type Song struct {
	Format  int            `json:"format"`
	BPM     int            `json:"bpm"`
	Octave  int            `json:"octave"`
	Tone    int            `json:"tone"`
	Volume  int            `json:"volume"`
	Pattern [steps][]Hit   `json:"pattern"`         // First page; kept for format-1 compatibility.
	Pages   [][steps][]Hit `json:"pages,omitempty"` // Additional pages, in playback order.
}

func defaultSong() Song { return Song{Format: 1, BPM: 110, Octave: 4, Volume: 45} }

// firstInstallSong is written only when the device has no user song yet.
// The embedded melody is the right-hand Octave 5 arrangement supplied for the
// first-run experience; subsequent launches load the user's saved project.
func firstInstallSong() Song {
	s := Song{Format: 2, BPM: 95, Octave: 5, Volume: 75}
	midi := map[byte]int{'A': 72, 'S': 74, 'D': 76, 'F': 77, 'G': 79, 'H': 81, 'J': 83, 'K': 84}
	const score = `D+A S+A A+A S+A | D+F F+F D+F S+G |
D+A S+A A+A S+A | D+F F+F D+F S+G |
D+A S+A A+A S+A | D+F F+F D+F S+G |
D+A S+A A+A S+A | D+F F+F D+F S+G |
D+A D+A S+A F+F D+F S+G |
S+A S+A A+A A+F F+F D+F S+G |
S+A A+F S+F D+G |
0 D+G G+A K+A |
J+A K+A J+A K+A |
J+G H+G G+G G+G S+G F+G |
F+A D+A D+A G+G |
F+A D+A S+A D+G G+G |
A+A 0 A+A |
S+G A+G J+G A+G G+G A+G |
F+A D+A S+G A+A A+A |
A+A 0 A+A S+A |
D+A D+A S+A F+F D+F S+G |
S+A S+A A+A F+F D+F S+G |
S+A A+F S+F D+G |
0 D+G G+A K+A`
	cells := make([][]Hit, 0, 112)
	for _, token := range strings.Fields(score) {
		if token == "|" {
			continue
		}
		if token == "0" {
			cells = append(cells, nil)
			continue
		}
		cells = append(cells, []Hit{{MIDI: midi[token[0]], Tone: 0}})
	}
	for len(cells) < steps {
		cells = append(cells, nil)
	}
	copy(s.Pattern[:], cells[:steps])
	for offset := steps; offset < len(cells); offset += steps {
		var page [steps][]Hit
		end := offset + steps
		if end > len(cells) {
			end = len(cells)
		}
		copy(page[:], cells[offset:end])
		s.Pages = append(s.Pages, page)
	}
	return s
}
func (s Song) validate() error {
	if (s.Format < 1 || s.Format > 3) || (s.Format == 1 && len(s.Pages) != 0) || s.pageCount() > maxPages || s.BPM < 60 || s.BPM > 180 || s.Octave < 3 || s.Octave > 6 || s.Tone < 0 || s.Tone > 2 || s.Volume < 0 || s.Volume > 100 {
		return fmt.Errorf("invalid song settings")
	}
	for page := 0; page < s.pageCount(); page++ {
		for step, notes := range *s.patternAt(page) {
			if len(notes) > 8 {
				return fmt.Errorf("too many notes in a step")
			}
			for _, h := range notes {
				pos := page*steps + step
				if h.Tie && (s.Format != 3 || h.Drum != 0 || pos == 0 || noteIndex(s.cell(pos-1), h) < 0) {
					return fmt.Errorf("invalid tie")
				}
				if h.Drum < 0 || h.Drum > 4 || h.Tone < 0 || h.Tone > 2 || (h.Drum == 0 && (h.MIDI < 36 || h.MIDI > 96)) {
					return fmt.Errorf("invalid note")
				}
			}
		}
	}
	return nil
}

type keyEvent struct {
	Code  uint16
	Down  bool
	Reset bool
}
type soundCommand struct {
	Keep                         uint8 // Loop slots that continue without note-off/retrigger.
	Kind                         string
	ID, MIDI, Tone, Drum, Volume int
}
type spark struct {
	Key  int
	At   time.Time
	Drum bool
}
type model struct {
	Song                            Song
	Playing, Recording, Help, Dirty bool
	StepMode                        bool // Manual cursor: clock stopped, recording may remain armed.
	Page                            int  // Selected page; not persisted as musical content.
	pageKey                         uint16
	pageKeyAt                       time.Time
	pageKeyFired                    bool
	pageSavePending                 bool
	Step                            int
	StartStep                       int // Pattern position at Started; not persisted in Song.
	Started                         time.Time
	LastTick                        int64
	Held                            map[uint16]int
	// Feedback keeps a released key highlighted long enough for the e-paper
	// panel to show a short tap; it does not affect audio sustain.
	Feedback      [13]time.Time
	Sparks        []spark
	Notice        string
	NoticeUntil   time.Time
	ClearUntil    time.Time
	ClearPage     int // A destructive confirmation never carries to another page.
	Changed       time.Time
	captures      [128]heldCapture
	transport     loopTransport
	commandBuffer [9]soundCommand
	sparkBuffer   [32]spark
}

func newModel(song Song) *model {
	m := &model{Song: song, Step: -1, LastTick: -1, Held: make(map[uint16]int, 13)}
	m.Sparks = m.sparkBuffer[:0]
	return m
}
func keyNote(code uint16) (int, bool) {
	codes := [...]uint16{30, 17, 31, 18, 32, 33, 20, 34, 21, 35, 22, 36, 37} // A W S E D F T G Y H U J K
	for n, c := range codes {
		if c == code {
			return n, true
		}
	}
	return 0, false
}
func (m *model) message(s string, now time.Time) {
	m.Notice = s
	m.NoticeUntil = now.Add(2 * time.Second)
}
func (m *model) changed(now time.Time) {
	m.Dirty = true
	m.Changed = now
	// Any edit invalidates a pending destructive-confirmation sequence.
	m.ClearUntil = time.Time{}
	m.ClearPage = -1
}
func (m *model) stepDuration() time.Duration { return time.Minute / time.Duration(m.Song.BPM*2) }
func (m *model) start(now time.Time)         { m.startFrom(now, 0) }
func (m *model) startFrom(now time.Time, step int) {
	m.cancelTies()
	m.transport.valid = false
	m.Playing, m.StepMode = true, false
	m.Started, m.LastTick = now, -1
	m.StartStep = m.Page*steps + (step%steps+steps)%steps
	m.Step = m.StartStep % steps
}
func (m *model) stop() {
	m.cancelTies()
	m.transport.valid = false
	m.Playing, m.Recording, m.StepMode = false, false, false
	m.Step = -1
}
func (m *model) clockTick(now time.Time) int64 {
	return int64(max(time.Duration(0), now.Sub(m.Started)) / m.stepDuration())
}
func (m *model) clockStep(now time.Time) int {
	return m.clockPosition(now) % steps
}
func (m *model) selectStep(delta int, now time.Time) {
	manual := m.StepMode
	from := m.Page*steps + m.Step
	if m.Playing {
		// Input is handled before tick in the main loop. Use the current clock
		// position rather than a potentially stale last-rendered step.
		m.syncPosition(now)
	}
	if m.Step < 0 {
		m.Step = 0 // First navigation from idle selects the first cell.
	} else {
		m.Step = (m.Step + delta + steps) % steps
	}
	m.Playing, m.StepMode = false, true
	if manual {
		m.extendHeld(from, now)
	} else {
		m.cancelTies()
	}
}
func (m *model) setBPM(bpm int, now time.Time) {
	if bpm == m.Song.BPM {
		return
	}
	if m.Playing {
		// Preserve both the selected cell and its fractional progress, so a
		// speed change neither jumps to cell 1 nor retriggers an already played cell.
		oldDuration := m.stepDuration()
		elapsed := max(time.Duration(0), now.Sub(m.Started))
		played := m.clockTick(now) <= m.LastTick
		m.syncPosition(now)
		step := m.Step
		m.Song.BPM = bpm
		phase := (elapsed % oldDuration) * m.stepDuration() / oldDuration
		previousTransport := m.transport
		preserveTransport := int64(elapsed/oldDuration) <= m.LastTick+1
		m.startFrom(now.Add(-phase), step)
		if preserveTransport {
			m.transport = previousTransport
		}
		if played {
			m.LastTick = 0
		}
	} else {
		m.Song.BPM = bpm
	}
	m.changed(now)
}
func (m *model) record(h Hit, now time.Time) {
	if !m.Recording {
		return
	}
	idx := m.Step
	page := m.Page
	if !m.StepMode {
		tick := int64((max(time.Duration(0), now.Sub(m.Started)) + m.stepDuration()/2) / m.stepDuration())
		total := m.Song.pageCount() * steps
		position := (m.StartStep + int(tick%int64(total))) % total
		page, idx = position/steps, position%steps
	}
	if idx < 0 || idx >= steps {
		return
	}
	pattern := m.Song.patternAt(page)
	notes := pattern[idx]
	for i, old := range notes {
		if old == h {
			return
		}
		if sameNote(old, h) {
			notes[i] = h // Pressing again explicitly rearticulates a tied note.
			m.changed(now)
			return
		}
	}
	if len(notes) >= 8 {
		m.message("STEP FULL", now)
		return
	}
	pattern[idx] = append(notes, h)
	m.changed(now)
}
func (m *model) addSpark(k int, drum bool, now time.Time) {
	if len(m.Sparks) >= 32 {
		copy(m.Sparks, m.Sparks[1:])
		m.Sparks = m.Sparks[:31]
	}
	m.Sparks = append(m.Sparks, spark{k, now, drum})
}

// handle returns commands plus an action (save/exit). Repeats are filtered at input.
func (m *model) handle(e keyEvent, now time.Time) ([]soundCommand, string) {
	if e.Reset {
		m.cancelTies()
		m.cancelPageGesture()
		clear(m.Held)
		m.Feedback = [13]time.Time{}
		m.Sparks = m.Sparks[:0]
		return []soundCommand{{Kind: "off-all"}}, ""
	}
	if systemPowerKey(e.Code) {
		m.cancelPageGesture()
		return nil, ""
	}
	if e.Code == 23 || e.Code == 24 {
		if m.Help {
			return nil, ""
		}
		return m.handlePageKey(e, now), ""
	}
	if e.Down && m.pageKey != 0 {
		m.pageKeyFired = true
	} // Chords never create pages.
	if !e.Down {
		if _, ok := m.Held[e.Code]; ok {
			delete(m.Held, e.Code)
			if e.Code < 128 {
				m.captures[e.Code].Valid = false
			}
			if n, ok := keyNote(e.Code); ok {
				m.Feedback[n] = now.Add(keyFeedbackDuration)
			}
			return []soundCommand{{Kind: "off", ID: int(e.Code)}}, ""
		}
		return nil, ""
	}
	if exitKey(e) {
		return nil, "exit"
	}
	if e.Code == 38 || e.Code == 53 {
		m.cancelTies()
		m.cancelPageGesture()
		m.Help = !m.Help
		return nil, ""
	}
	if m.Help {
		return nil, ""
	}
	if n, ok := keyNote(e.Code); ok {
		if _, held := m.Held[e.Code]; held {
			return nil, ""
		}
		midi := 12*(m.Song.Octave+1) + n
		m.Held[e.Code] = midi
		m.Feedback[n] = now.Add(keyFeedbackDuration)
		m.addSpark(n, false, now)
		h := Hit{MIDI: midi, Tone: m.Song.Tone}
		m.record(h, now)
		if m.Recording && m.StepMode && m.Step >= 0 && noteIndex(m.currentPattern()[m.Step], h) >= 0 {
			m.captures[e.Code] = heldCapture{Hit: h, Position: m.Page*steps + m.Step, Valid: true}
		}
		return []soundCommand{{Kind: "on", ID: int(e.Code), MIDI: midi, Tone: m.Song.Tone}}, ""
	}
	drum := -1
	if e.Code >= 46 && e.Code <= 49 {
		drum = int(e.Code) - 46
	}
	if e.Code >= 2 && e.Code <= 5 {
		drum = int(e.Code) - 2
	} // Optional external keyboard aliases.
	if drum >= 0 {
		m.addSpark(drum, true, now)
		m.record(Hit{Drum: drum + 1}, now)
		return []soundCommand{{Kind: "drum", ID: 200 + drum, Drum: drum}}, ""
	}
	switch e.Code {
	case 57: // Space pauses auto-recording without disarming it; R ends recording.
		if m.StepMode {
			m.startFrom(now, m.Step)
		} else if m.Playing && m.Recording {
			m.syncPosition(now)
			m.Playing, m.StepMode = false, true
			return []soundCommand{{Kind: "loop-off"}}, ""
		} else if m.Playing {
			m.syncPosition(now)
			m.stop()
			return []soundCommand{{Kind: "loop-off"}}, ""
		} else {
			m.start(now)
		}
	case 19: // R starts automatic recording; a second press only disarms recording.
		m.Recording = !m.Recording
		m.cancelTies()
		if m.Recording && !m.Playing {
			if m.StepMode {
				m.startFrom(now, m.Step)
			} else {
				m.start(now)
			}
		}
		if m.Recording {
			m.message("AUTO REC - LEFT/RIGHT: STEP", now)
		} else {
			m.message("RECORDING STOPPED", now)
		}
	case 16, 15:
		m.Song.Tone = (m.Song.Tone + 1) % 3
		m.changed(now)
	case 44:
		if m.Song.Octave > 3 {
			m.Song.Octave--
			m.changed(now)
		}
	case 45:
		if m.Song.Octave < 6 {
			m.Song.Octave++
			m.changed(now)
		}
	case 105, 106: // Left/right: move exactly one cell and stop the automatic clock.
		delta := 1
		if e.Code == 105 {
			delta = -1
		}
		m.selectStep(delta, now)
		if !m.Recording {
			m.message("STEP VIEW - R: START RECORDING", now)
		}
		return []soundCommand{{Kind: "loop-off"}}, ""
	case 103, 108: // Up/down controls tempo; dedicated volume keys control loudness.
		bpm := m.Song.BPM
		if e.Code == 103 {
			bpm += 5
		} else {
			bpm -= 5
		}
		m.setBPM(max(60, min(180, bpm)), now)
	case 114, 115:
		v := m.Song.Volume
		if e.Code == 115 {
			v += 5
		} else {
			v -= 5
		}
		m.Song.Volume = max(0, min(100, v))
		m.changed(now)
		return []soundCommand{{Kind: "volume", Volume: m.Song.Volume}}, ""
	case 25:
		return nil, "export"
	case 28, 352:
		return nil, "save"
	case 14, 111:
		m.cancelTies()
		if m.Playing {
			m.syncPosition(now)
		}
		// In step mode the first Delete removes the selected cell. A second
		// Delete in the confirmation window resets the whole project, matching
		// the global clear action while still allowing precise single-cell undo.
		if m.StepMode && m.Step >= 0 && m.Step < steps {
			if now.Before(m.ClearUntil) && m.ClearPage == m.Page {
				m.Song.Pattern = [steps][]Hit{}
				m.Song.Pages = nil
				m.Song.Format = 1
				m.Page, m.Step = 0, -1
				m.stop()
				m.ClearUntil = time.Time{}
				m.ClearPage = -1
				m.changed(now)
				m.pageSavePending = true
				m.message("PROJECT CLEARED - 1 PAGE", now)
				return []soundCommand{{Kind: "loop-off"}}, ""
			}
			cell := &m.currentPattern()[m.Step]
			if len(*cell) == 0 {
				m.message("CELL ALREADY EMPTY", now)
			} else {
				*cell = nil
				m.Song.repairTies()
				m.changed(now)
				m.message("CELL CLEARED - DEL AGAIN: CLEAR ALL", now)
			}
			m.ClearPage = m.Page
			m.ClearUntil = now.Add(2 * time.Second)
			return []soundCommand{{Kind: "loop-off"}}, ""
		}
		if now.Before(m.ClearUntil) && m.ClearPage == m.Page {
			m.Song.Pattern = [steps][]Hit{}
			m.Song.Pages = nil
			m.Song.Format = 1
			m.Page = 0
			m.stop()
			m.ClearUntil = time.Time{}
			m.ClearPage = -1
			m.changed(now)
			m.pageSavePending = true
			m.message("PROJECT CLEARED - 1 PAGE", now)
			return []soundCommand{{Kind: "loop-off"}}, ""
		}
		m.ClearPage = m.Page
		m.ClearUntil = now.Add(2 * time.Second)
		m.message("DEL AGAIN: CLEAR ALL TO 1 PAGE", now)
	}
	return nil, ""
}
func (m *model) keyLit(code uint16, now time.Time) bool {
	if _, held := m.Held[code]; held {
		return true
	}
	n, ok := keyNote(code)
	return ok && now.Before(m.Feedback[n])
}

func (m *model) tick(now time.Time) []soundCommand {
	m.checkPageHold(now)
	live := m.Sparks[:0]
	for _, s := range m.Sparks {
		if now.Sub(s.At) < keyFeedbackDuration {
			live = append(live, s)
		}
	}
	m.Sparks = live
	if !m.Playing {
		return nil
	}
	tick := m.clockTick(now)
	if tick <= m.LastTick {
		return nil
	}
	// On a stall resume at the current step; never play a burst of stale notes.
	contiguous := tick == m.LastTick+1
	m.LastTick = tick
	m.syncPosition(now)
	commands := m.transport.commands(m.commandBuffer[:0], m.currentPattern()[m.Step], m.Page*steps+m.Step, contiguous)
	for _, h := range m.currentPattern()[m.Step] {
		if h.Drum > 0 {
			m.addSpark(h.Drum-1, true, now)
		} else {
			m.addSpark(h.MIDI%12, false, now)
		}
	}
	return commands
}
func (m *model) noteCount() int {
	n := 0
	for p := 0; p < m.Song.pageCount(); p++ {
		for _, hits := range *m.Song.patternAt(p) {
			n += len(hits)
		}
	}
	return n
}
