package main

import (
	"bytes"
	"encoding/json"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"
	"time"
)

func press(m *model, code uint16, now time.Time) []soundCommand {
	commands, _ := m.handle(keyEvent{Code: code, Down: true}, now)
	return commands
}

func TestStepControlMappingAndLimits(t *testing.T) {
	now := time.Unix(100, 0)
	m := newModel(defaultSong())
	if out := press(m, 103, now); len(out) != 0 || m.Song.BPM != 115 || m.Song.Volume != 45 {
		t.Fatal("up must increase tempo only")
	}
	press(m, 108, now)
	if m.Song.BPM != 110 || m.Song.Volume != 45 {
		t.Fatal("down must decrease tempo only")
	}
	out := press(m, 115, now)
	if len(out) != 1 || out[0].Kind != "volume" || out[0].Volume != 50 || m.Song.BPM != 110 {
		t.Fatal("volume-up no longer controls volume independently")
	}
	press(m, 114, now)
	if m.Song.Volume != 45 {
		t.Fatal("volume-down")
	}
	for i := 0; i < 100; i++ {
		press(m, 103, now)
		press(m, 115, now)
	}
	if m.Song.BPM != 180 || m.Song.Volume != 100 {
		t.Fatal("upper limits")
	}
	for i := 0; i < 100; i++ {
		press(m, 108, now)
		press(m, 114, now)
	}
	if m.Song.BPM != 60 || m.Song.Volume != 0 {
		t.Fatal("lower limits")
	}
}

func TestStepNavigationUsesCurrentClockAndFreezes(t *testing.T) {
	for _, tc := range []struct {
		code uint16
		want int
	}{{105, 4}, {106, 6}} {
		m := newModel(demoSong())
		now := time.Unix(100, 0)
		press(m, 19, now)
		m.tick(now)
		at := now.Add(5*m.stepDuration() + m.stepDuration()/4)
		out := press(m, tc.code, at) // Deliberately do not tick before this input.
		if !m.StepMode || m.Playing || !m.Recording || m.Step != tc.want {
			t.Fatalf("navigation selected %d; expected %d", m.Step, tc.want)
		}
		if len(out) != 1 || out[0].Kind != "loop-off" {
			t.Fatal("entering step mode did not release loop voices")
		}
		if m.Song.BPM != 110 || m.Song.Volume != 30 || m.Dirty {
			t.Fatal("navigation changed saved settings")
		}
		if out := m.tick(at.Add(time.Hour)); len(out) != 0 || m.Step != tc.want {
			t.Fatal("manual cursor moved or replayed while waiting")
		}
	}
}

func TestStepIdleNavigationAndWrap(t *testing.T) {
	for _, code := range []uint16{105, 106} {
		m := newModel(defaultSong())
		now := time.Unix(100, 0)
		press(m, code, now)
		if !m.StepMode || m.Step != 0 || m.Recording || m.Playing {
			t.Fatal("idle navigation should select first cell for browsing")
		}
		press(m, 105, now)
		if m.Step != 15 {
			t.Fatal("left wrap")
		}
		press(m, 106, now)
		if m.Step != 0 {
			t.Fatal("right wrap")
		}
		press(m, 30, now)
		if m.noteCount() != 0 {
			t.Fatal("browsing silently armed recording")
		}
		m.handle(keyEvent{Code: 30}, now)
		press(m, 19, now)
		if !m.Playing || !m.Recording || m.StepMode || m.Step != 0 {
			t.Fatal("R did not start automatic recording from selected cell")
		}
	}
}

func TestStepRecordsChordAndDrumIntoSelectedCell(t *testing.T) {
	m := newModel(defaultSong())
	now := time.Unix(100, 0)
	press(m, 19, now)
	press(m, 106, now) // Select second cell, keep recording.
	at := now.Add(time.Hour)
	for _, code := range []uint16{30, 32, 34, 46} {
		commands := press(m, code, at)
		if len(commands) != 1 || (commands[0].Kind != "on" && commands[0].Kind != "drum") {
			t.Fatal("step input did not audition immediately")
		}
		m.handle(keyEvent{Code: code}, at)
	}
	want := []Hit{{MIDI: 60}, {MIDI: 64}, {MIDI: 67}, {Drum: 1}}
	if m.Step != 1 || !reflect.DeepEqual(m.Song.Pattern[1], want) || m.noteCount() != 4 {
		t.Fatalf("manual recording followed time or advanced cursor: %+v", m.Song.Pattern)
	}
	if !m.Dirty || !m.Changed.Equal(at) {
		t.Fatal("step edits not marked for save")
	}
	press(m, 30, at)
	m.handle(keyEvent{Code: 30}, at)
	if m.noteCount() != 4 {
		t.Fatal("duplicate input replaced or duplicated a cell note")
	}
	press(m, 106, at)
	press(m, 17, at)
	m.handle(keyEvent{Code: 17}, at)
	if len(m.Song.Pattern[2]) != 1 || m.Song.Pattern[2][0].MIDI != 61 || len(m.Song.Pattern[1]) != 4 {
		t.Fatal("moving cursor failed to preserve prior cell")
	}
}

func TestStepSpaceResumesSelectedCellAndKeepsRecording(t *testing.T) {
	for _, recording := range []bool{false, true} {
		for start := 0; start < steps; start++ {
			m := newModel(demoSong())
			m.StepMode, m.Recording, m.Step = true, recording, start
			now := time.Unix(100, 0)
			press(m, 57, now)
			if !m.Playing || m.StepMode || m.Step != start || m.Recording != recording {
				t.Fatalf("space changed cursor/record state at %d recording=%v", start, recording)
			}
			for i := 0; i <= steps; i++ {
				at := now.Add(time.Duration(i) * m.stepDuration())
				out := m.tick(at)
				want := (start + i) % steps
				if m.Step != want || len(out) != 1+len(m.Song.Pattern[want]) || out[1].MIDI != m.Song.Pattern[want][0].MIDI {
					t.Fatalf("resumed playback missed cell %d: step=%d out=%+v", want, m.Step, out)
				}
				if len(m.tick(at)) != 0 {
					t.Fatal("same clock tick retriggered")
				}
			}
		}
	}
}

func TestStepResumedRecordingQuantizationWrapsFromOffset(t *testing.T) {
	for start := 0; start < steps; start++ {
		m := newModel(defaultSong())
		m.StepMode, m.Recording, m.Step = true, true, start
		now := time.Unix(100, 0)
		press(m, 57, now)
		for i := 0; i < steps; i++ {
			at := now.Add(time.Duration(i)*m.stepDuration() + m.stepDuration()*3/4)
			m.record(Hit{MIDI: 60 + i}, at)
			want := (start + i + 1) % steps // Automatic input rounds to nearest half-beat.
			if len(m.Song.Pattern[want]) != 1 || m.Song.Pattern[want][0].MIDI != 60+i {
				t.Fatalf("recording lost resumed offset %d at tick %d", start, i)
			}
		}
	}
}

func TestStepRToggleAndSpaceStop(t *testing.T) {
	now := time.Unix(100, 0)
	m := newModel(defaultSong())
	press(m, 19, now)
	press(m, 106, now)
	press(m, 19, now)
	if m.Recording || m.Playing || !m.StepMode || m.Step != 1 {
		t.Fatal("R should disarm manual recording and keep cursor")
	}
	press(m, 30, now)
	m.handle(keyEvent{Code: 30}, now)
	if m.noteCount() != 0 {
		t.Fatal("R did not stop recording")
	}
	press(m, 19, now)
	if !m.Recording || !m.Playing || m.StepMode || m.Step != 1 {
		t.Fatal("R from manual browse should start auto recording at current cell")
	}
	press(m, 19, now)
	if m.Recording || !m.Playing {
		t.Fatal("ending auto recording should keep playback")
	}
	press(m, 19, now)
	out := press(m, 57, now)
	if !m.Recording || m.Playing || !m.StepMode || m.Step != 1 || len(out) != 1 || out[0].Kind != "loop-off" {
		t.Fatal("space during automatic recording should pause without disarming")
	}
	press(m, 19, now)
	if m.Recording || m.Playing || !m.StepMode {
		t.Fatal("R should end paused recording")
	}
	press(m, 57, now)
	if !m.Playing || m.Recording || m.Step != 1 {
		t.Fatal("space should restart playback after recording ended")
	}
}

func TestStepTempoPreservesManualCursorAndAutoPhase(t *testing.T) {
	now := time.Unix(100, 0)
	m := newModel(defaultSong())
	m.StepMode, m.Recording, m.Step = true, true, 9
	press(m, 103, now)
	if m.Step != 9 || m.Playing || !m.StepMode || !m.Recording {
		t.Fatal("tempo left manual mode")
	}
	press(m, 57, now)
	m.tick(now)
	oldDuration := m.stepDuration()
	at := now.Add(oldDuration * 7 / 2)
	m.tick(at) // Cell 13 (index 12), half way through.
	press(m, 108, at)
	if m.Step != 12 || m.StartStep != 12 || !m.Recording {
		t.Fatal("tempo jumped to another cell")
	}
	phase := at.Sub(m.Started)
	if phase < m.stepDuration()/2-time.Nanosecond || phase > m.stepDuration()/2+time.Nanosecond {
		t.Fatal("tempo lost fractional progress")
	}
	if len(m.tick(at)) != 0 {
		t.Fatal("tempo retriggered already played cell")
	}
	m.tick(m.Started.Add(m.stepDuration()))
	if m.Step != 13 {
		t.Fatal("tempo did not advance from preserved cell")
	}
}

func TestStepTempoAtUnprocessedBoundaryStillPlaysCurrentCell(t *testing.T) {
	m := newModel(demoSong())
	now := time.Unix(100, 0)
	m.startFrom(now, 14)
	m.tick(now)
	at := now.Add(m.stepDuration())
	press(m, 103, at) // main handles input before its tick.
	out := m.tick(at)
	if m.Step != 15 || len(out) != 1+len(m.Song.Pattern[15]) {
		t.Fatal("tempo swallowed a not-yet-played boundary")
	}
}

func TestStepHeldNotesReleaseAcrossModeChanges(t *testing.T) {
	m := newModel(defaultSong())
	now := time.Unix(100, 0)
	press(m, 19, now)
	press(m, 30, now)
	press(m, 106, now)
	press(m, 57, now)
	out, _ := m.handle(keyEvent{Code: 30}, now)
	if len(out) != 1 || out[0].Kind != "off" || out[0].ID != 30 || len(m.Held) != 0 {
		t.Fatal("mode switch lost live release")
	}
	if m.noteCount() != 1 {
		t.Fatal("mode change duplicated held note")
	}
}

func TestStepLimitClearAndStop(t *testing.T) {
	m := newModel(defaultSong())
	now := time.Unix(100, 0)
	m.StepMode, m.Recording, m.Step = true, true, 15
	for i := 0; i < 9; i++ {
		m.record(Hit{MIDI: 60 + i}, now)
	}
	if m.noteCount() != 8 || m.Notice != "STEP FULL" {
		t.Fatal("manual cell limit")
	}
	out := press(m, 111, now)
	if m.noteCount() != 0 || len(out) != 1 || out[0].Kind != "loop-off" {
		t.Fatal("first Delete did not clear selected cell")
	}
	out = press(m, 111, now.Add(time.Second))
	if m.noteCount() != 0 || m.Song.pageCount() != 1 || m.Step != -1 || m.StepMode || m.Recording || m.Notice != "PROJECT CLEARED - 1 PAGE" {
		t.Fatal("second Delete did not clear the project")
	}
	m.stop() // Also used when starting WAV export.
	if m.Playing || m.Recording || m.StepMode || m.Step != -1 {
		t.Fatal("export/stop left stale manual mode")
	}
}

func TestStepViewShowsModeAndSelectedCell(t *testing.T) {
	m := newModel(defaultSong())
	now := time.Unix(100, 0)
	m.startFrom(now, 7)
	m.Recording = true
	auto := render(m, now)
	m.Playing, m.StepMode = false, true
	v := m.view(now)
	if !v.StepMode || v.Step != 7 || !strings.Contains(v.Footer, "SPACE AUTO") || auto == render(m, now) {
		t.Fatal("manual recording not distinguishable from automatic recording")
	}
	first := render(m, now)
	m.Step++
	if first == render(m, now) {
		t.Fatal("cursor/step counter did not move")
	}
	m.Recording = false
	if !strings.Contains(m.view(now).Footer, "SPACE PLAY") {
		t.Fatal("browse help still says recording")
	}
}

func TestStepHelpAndExportInputDoNotMoveCursor(t *testing.T) {
	m := newModel(defaultSong())
	now := time.Unix(100, 0)
	m.StepMode, m.Recording, m.Step, m.Help = true, true, 5, true
	for _, code := range []uint16{105, 106, 103, 108, 57, 19} {
		press(m, code, now)
	}
	if m.Step != 5 || !m.StepMode || m.Playing || !m.Recording || m.Song.BPM != 110 {
		t.Fatal("help accepted hidden transport actions")
	}
	m.Help = false
	for _, code := range []uint16{105, 106, 103, 108, 57, 19} {
		handleInput(m, keyEvent{Code: code, Down: true}, now, true)
	}
	if m.Step != 5 || !m.StepMode || m.Playing || !m.Recording || m.Song.BPM != 110 {
		t.Fatal("export accepted editing actions")
	}
}

func TestStepSaveAndExportKeepOriginalSongFormat(t *testing.T) {
	m := newModel(defaultSong())
	now := time.Unix(100, 0)
	m.StepMode, m.Recording, m.Step = true, true, 15
	m.record(Hit{MIDI: 60, Tone: 1}, now)
	m.record(Hit{Drum: 1}, now)
	path := filepath.Join(t.TempDir(), "song.json")
	if err := saveSong(path, m.Song); err != nil {
		t.Fatal(err)
	}
	s, err := loadSong(path)
	if err != nil || !reflect.DeepEqual(s, m.Song) || s.Format != 1 {
		t.Fatal("step recording changed storage format", err)
	}
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	var fields map[string]json.RawMessage
	if err := json.Unmarshal(data, &fields); err != nil {
		t.Fatal(err)
	}
	wantFields := []string{"format", "bpm", "octave", "tone", "volume", "pattern"}
	if len(fields) != len(wantFields) {
		t.Fatal("transport state leaked into saved song", string(data))
	}
	for _, key := range wantFields {
		if _, ok := fields[key]; !ok {
			t.Fatalf("missing original song field %s", key)
		}
	}
	fresh := newModel(s)
	if fresh.StepMode || fresh.Recording || fresh.Playing || fresh.Step != -1 {
		t.Fatal("reopen restored armed recording")
	}
	wav := filepath.Join(t.TempDir(), "loop.wav")
	if err := exportWAV(wav, s); err != nil {
		t.Fatal(err)
	}
	audio, err := os.ReadFile(wav)
	if err != nil || len(audio) <= 44 {
		t.Fatal("step song did not export", err)
	}
	if bytes.Count(audio[44:], []byte{0}) == len(audio)-44 {
		t.Fatal("step song export silent")
	}
}

func TestStepStripShowsSelectedCellRatherThanPreviousActivity(t *testing.T) {
	m := newModel(defaultSong())
	now := time.Unix(100, 0)
	m.Song.Pattern[3] = []Hit{{MIDI: 60}, {MIDI: 72}, {Drum: 1}}
	m.Song.Pattern[4] = []Hit{{MIDI: 64}, {Drum: 2}}
	m.StepMode, m.Step = true, 3
	m.addSpark(7, false, now)
	m.addSpark(3, true, now)
	v := m.view(now)
	if v.Notes != (1<<0|1<<12) || v.Drums != 1 {
		t.Fatal("strip does not show selected cell contents")
	}
	press(m, 106, now)
	m.NoticeUntil = time.Time{}
	v = m.view(now)
	if v.Notes != 1<<4 || v.Drums != 1<<1 {
		t.Fatal("strip retained previous cell contents")
	}
	if m.view(now.Add(time.Hour)) != v {
		t.Fatal("saved cell contents expired like transient feedback")
	}
}

func TestStepPausedRefreshDoesNotAllocateOrRewrite(t *testing.T) {
	m := newModel(demoSong())
	now := time.Unix(100, 0)
	m.StepMode, m.Recording, m.Step = true, true, 8
	r := newRefreshScheduler(render(m, now), now)
	calls := 0
	submit := func(frame) { calls++ }
	r.update(m, now, true, submit)
	allocs := testing.AllocsPerRun(1000, func() {
		now = now.Add(backgroundRefreshInterval)
		m.tick(now)
		r.update(m, now, false, submit)
	})
	if allocs != 0 || calls != 0 {
		t.Fatalf("paused step mode allocated %g objects, submitted %d frames", allocs, calls)
	}
	press(m, 106, now)
	r.update(m, now, true, submit)
	if calls != 1 {
		t.Fatal("cursor input waited for background refresh")
	}
}

func TestStepPreview(t *testing.T) {
	dir := os.Getenv("PINAO_TEST_PREVIEW_DIR")
	if dir == "" {
		dir = t.TempDir()
	}
	now := time.Unix(100, 0)
	for _, mode := range []string{"auto-rec", "step-rec", "step-view", "help"} {
		m := newModel(demoSong())
		m.Step, m.Recording = 15, true
		m.Playing = mode == "auto-rec"
		m.StepMode = !m.Playing
		m.Help = mode == "help"
		if mode == "step-view" {
			m.Recording = false
		}
		f, err := os.OpenFile(filepath.Join(dir, mode+".png"), os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0600)
		if err != nil {
			t.Fatal(err)
		}
		err = writePNG(f, render(m, now), 3)
		closeErr := f.Close()
		if err != nil {
			t.Fatal(err)
		}
		if closeErr != nil {
			t.Fatal(closeErr)
		}
	}
}

func TestStepSwitchingKeepsBoundedCommandStorage(t *testing.T) {
	m := newModel(resourceFullSong())
	now := time.Unix(100, 0)
	m.startFrom(now, 15)
	var commands [10]soundCommand // Same capacity as the main loop.
	for i := 0; i < 1000; i++ {
		at := now.Add(time.Duration(i) * time.Second)
		out := commands[:0]
		out = append(out, press(m, 105, at)...)
		out = append(out, m.tick(at)...)
		if len(out) != 1 || len(out) > cap(commands) {
			t.Fatal("manual tick produced playback commands")
		}
		out = commands[:0]
		out = append(out, press(m, 57, at)...)
		out = append(out, m.tick(at)...)
		if len(out) != 9 || len(out) > cap(commands) {
			t.Fatal("resumed tick exceeded command capacity")
		}
	}
	if cap(m.Sparks) != 32 || cap(m.commandBuffer[:]) != 9 {
		t.Fatal("step mode grew fixed buffers")
	}
}
