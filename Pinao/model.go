package main

import (
	"fmt"
	"time"
)

const steps = 16 // Eight beats, quantized to eighth notes.
const keyFeedbackDuration = 1200 * time.Millisecond

type Hit struct {
	MIDI int `json:"midi"`
	Tone int `json:"tone"`
	Drum int `json:"drum"`
}
type Song struct {
	Format  int          `json:"format"`
	BPM     int          `json:"bpm"`
	Octave  int          `json:"octave"`
	Tone    int          `json:"tone"`
	Volume  int          `json:"volume"`
	Pattern [steps][]Hit `json:"pattern"`
}

func defaultSong() Song { return Song{Format: 1, BPM: 110, Octave: 4, Volume: 45} }
func (s Song) validate() error {
	if s.Format != 1 || s.BPM < 60 || s.BPM > 180 || s.Octave < 3 || s.Octave > 6 || s.Tone < 0 || s.Tone > 2 || s.Volume < 0 || s.Volume > 100 {
		return fmt.Errorf("invalid song settings")
	}
	for _, notes := range s.Pattern {
		if len(notes) > 8 {
			return fmt.Errorf("too many notes in a step")
		}
		for _, h := range notes {
			if h.Drum < 0 || h.Drum > 4 || h.Tone < 0 || h.Tone > 2 || (h.Drum == 0 && (h.MIDI < 36 || h.MIDI > 96)) {
				return fmt.Errorf("invalid note")
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
	Changed       time.Time
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
func (m *model) changed(now time.Time)       { m.Dirty = true; m.Changed = now }
func (m *model) stepDuration() time.Duration { return time.Minute / time.Duration(m.Song.BPM*2) }
func (m *model) start(now time.Time)         { m.startFrom(now, 0) }
func (m *model) startFrom(now time.Time, step int) {
	m.Playing, m.StepMode = true, false
	m.Started, m.LastTick = now, -1
	m.StartStep = (step%steps + steps) % steps
	m.Step = m.StartStep
}
func (m *model) stop() {
	m.Playing, m.Recording, m.StepMode = false, false, false
	m.Step = -1
}
func (m *model) clockTick(now time.Time) int64 {
	return int64(max(time.Duration(0), now.Sub(m.Started)) / m.stepDuration())
}
func (m *model) clockStep(now time.Time) int {
	return (m.StartStep + int(m.clockTick(now)%steps)) % steps
}
func (m *model) selectStep(delta int, now time.Time) {
	if m.Playing {
		// Input is handled before tick in the main loop. Use the current clock
		// position rather than a potentially stale last-rendered step.
		m.Step = m.clockStep(now)
	}
	if m.Step < 0 {
		m.Step = 0 // First navigation from idle selects the first cell.
	} else {
		m.Step = (m.Step + delta + steps) % steps
	}
	m.Playing, m.StepMode = false, true
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
		step := m.clockStep(now)
		m.Song.BPM = bpm
		phase := (elapsed % oldDuration) * m.stepDuration() / oldDuration
		m.startFrom(now.Add(-phase), step)
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
	if !m.StepMode {
		tick := int64((max(time.Duration(0), now.Sub(m.Started)) + m.stepDuration()/2) / m.stepDuration())
		idx = (m.StartStep + int(tick%steps)) % steps
	}
	if idx < 0 || idx >= steps {
		return
	}
	notes := m.Song.Pattern[idx]
	for _, old := range notes {
		if old == h {
			return
		}
	}
	if len(notes) >= 8 {
		m.message("STEP FULL", now)
		return
	}
	m.Song.Pattern[idx] = append(notes, h)
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
		clear(m.Held)
		m.Feedback = [13]time.Time{}
		m.Sparks = m.Sparks[:0]
		return []soundCommand{{Kind: "off-all"}}, ""
	}
	if systemPowerKey(e.Code) {
		return nil, ""
	}
	if !e.Down {
		if _, ok := m.Held[e.Code]; ok {
			delete(m.Held, e.Code)
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
		m.record(Hit{MIDI: midi, Tone: m.Song.Tone}, now)
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
	case 57: // Manual -> auto keeps both the current cell and recording state.
		if m.StepMode {
			m.startFrom(now, m.Step)
		} else if m.Playing {
			m.stop()
			return []soundCommand{{Kind: "loop-off"}}, ""
		} else {
			m.start(now)
		}
	case 19: // R starts automatic recording; a second press only disarms recording.
		m.Recording = !m.Recording
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
		if m.Recording {
			m.message("STEP REC - PLAY NOTES INTO CELL", now)
		} else {
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
		if now.Before(m.ClearUntil) {
			m.Song.Pattern = [steps][]Hit{}
			m.ClearUntil = time.Time{}
			m.changed(now)
			m.message("LOOP CLEARED", now)
			return []soundCommand{{Kind: "loop-off"}}, ""
		}
		m.ClearUntil = now.Add(2 * time.Second)
		m.message("DEL AGAIN: CLEAR LOOP", now)
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
	m.LastTick = tick
	m.Step = (m.StartStep + int(tick%steps)) % steps
	commands := m.commandBuffer[:1]
	commands[0] = soundCommand{Kind: "loop-off"}
	for i, h := range m.Song.Pattern[m.Step] {
		if h.Drum > 0 {
			commands = append(commands, soundCommand{Kind: "drum", ID: 300 + i, Drum: h.Drum - 1})
			m.addSpark(h.Drum-1, true, now)
		} else {
			commands = append(commands, soundCommand{Kind: "on", ID: 300 + i, MIDI: h.MIDI, Tone: h.Tone})
			m.addSpark(h.MIDI%12, false, now)
		}
	}
	return commands
}
func (m *model) noteCount() int {
	n := 0
	for _, hits := range m.Song.Pattern {
		n += len(hits)
	}
	return n
}
