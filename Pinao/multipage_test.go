package main

import (
	"bytes"
	"context"
	"encoding/binary"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"reflect"
	"testing"
	"time"
)

// All times are synthetic: these tests neither sleep nor open device drivers.
func mpSong(pages int) Song {
	s := defaultSong()
	s.BPM = 120
	if pages > 1 {
		s.Format = 2
		s.Pages = make([][steps][]Hit, pages-1)
	}
	return s
}

func mpTap(m *model, code uint16, at time.Time) {
	m.handle(keyEvent{Code: code, Down: true}, at)
	m.handle(keyEvent{Code: code}, at.Add(50*time.Millisecond))
}

func mpAssertPosition(t *testing.T, m *model, page, step int) {
	t.Helper()
	if m.Page != page || m.Step != step {
		t.Fatalf("position = page %d cell %d, want page %d cell %d (zero-based)", m.Page, m.Step, page, step)
	}
}

func mpRead(t *testing.T, path string) []byte {
	t.Helper()
	b, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	return b
}

func mpWrite(t *testing.T, path string, b []byte) {
	t.Helper()
	if err := os.WriteFile(path, b, 0600); err != nil {
		t.Fatal(err)
	}
}

func TestMultipageDefaultAndTapBounds(t *testing.T) {
	if steps != 16 || maxPages != 64 || pageHoldDuration != 700*time.Millisecond {
		t.Fatal("page contract requires 16 cells, 64 pages maximum, and a 700ms hold")
	}
	missing, err := loadSong(filepath.Join(t.TempDir(), "missing.json"))
	if err != nil || !reflect.DeepEqual(missing, defaultSong()) || missing.pageCount() != 1 {
		t.Fatal("missing project did not load the single-page default:", err)
	}
	now := time.Unix(100, 0)
	for _, code := range []uint16{23, 24} {
		m := newModel(defaultSong())
		if m.Song.pageCount() != 1 || m.Page != 0 || m.Step != -1 || m.Song.Format != 1 {
			t.Fatal("new project must have exactly one selected, empty, format-1 page")
		}
		for i := 0; i < 3; i++ {
			mpTap(m, code, now)
		}
		if m.Song.pageCount() != 1 || m.Page != 0 || m.Dirty || m.pageSavePending || m.noteCount() != 0 {
			t.Fatalf("single-page tap %d created/wrapped/edited a page", code)
		}
	}
	for page := 0; page < 3; page++ {
		for _, code := range []uint16{23, 24} {
			t.Run(fmt.Sprintf("page%d/key%d", page, code), func(t *testing.T) {
				m := newModel(mpSong(3))
				m.Page, m.Step = page, 9
				before := m.Song.clone()
				out, _ := m.handle(keyEvent{Code: code, Down: true}, now)
				mpAssertPosition(t, m, page, 9)
				if len(out) != 1 || out[0].Kind != "loop-off" || !m.StepMode || m.Playing {
					t.Fatal("key-down must pause, not navigate before release")
				}
				m.handle(keyEvent{Code: code}, now.Add(pageHoldDuration-time.Nanosecond))
				want := page + pageDirection(code)
				valid := want >= 0 && want < 3
				if valid {
					mpAssertPosition(t, m, want, 0)
				} else {
					mpAssertPosition(t, m, page, 9)
				}
				if !reflect.DeepEqual(m.Song, before) || m.Dirty || m.pageSavePending != valid {
					t.Fatal("tap changed song content or incorrect page save request")
				}
				m.handle(keyEvent{Code: code}, now.Add(time.Second))
				m.tick(now.Add(2 * time.Second))
				if m.Song.pageCount() != 3 || m.pageKey != 0 {
					t.Fatal("stray release/tick inserted a page")
				}
			})
		}
	}
}

func TestMultipageHoldInsertsExactlyOnceInOrder(t *testing.T) {
	now := time.Unix(100, 0)
	for page := 0; page < 3; page++ {
		for _, code := range []uint16{23, 24} {
			for _, viaTick := range []bool{false, true} {
				t.Run(fmt.Sprintf("page%d/key%d/tick%v", page, code, viaTick), func(t *testing.T) {
					s := mpSong(3)
					for p := 0; p < 3; p++ {
						s.patternAt(p)[5] = []Hit{{MIDI: 60 + p, Tone: p}, {Drum: p + 1}}
					}
					m := newModel(s.clone())
					m.Page, m.Step, m.Recording = page, 7, true
					m.handle(keyEvent{Code: code, Down: true}, now)
					m.tick(now.Add(pageHoldDuration - time.Nanosecond))
					if m.Song.pageCount() != 3 {
						t.Fatal("hold fired before 700ms")
					}
					if viaTick {
						m.tick(now.Add(pageHoldDuration))
						for i := 1; i <= 5; i++ {
							m.handle(keyEvent{Code: code, Down: true}, now.Add(time.Duration(i)*time.Second))
							m.tick(now.Add(time.Duration(i) * time.Second))
						}
						m.handle(keyEvent{Code: code}, now.Add(6*time.Second))
					} else {
						// The loop may not tick between the threshold and release.
						m.handle(keyEvent{Code: code}, now.Add(pageHoldDuration))
					}
					index := page
					if code == 24 {
						index++
					}
					mpAssertPosition(t, m, index, 0)
					if m.Song.pageCount() != 4 || m.Song.Format != 2 || !m.Dirty || !m.pageSavePending || !m.Recording || !m.StepMode || m.Playing {
						t.Fatal("hold did not insert exactly one page while keeping recording armed")
					}
					for p := 0; p < 4; p++ {
						if p == index {
							if !reflect.DeepEqual(*m.Song.patternAt(p), [steps][]Hit{}) {
								t.Fatal("inserted page is not empty")
							}
							continue
						}
						old := p
						if p > index {
							old--
						}
						if !reflect.DeepEqual(*m.Song.patternAt(p), *s.patternAt(old)) {
							t.Fatalf("page %d was lost or reordered after insertion at %d", old, index)
						}
					}
					v := m.view(now.Add(7 * time.Second))
					if v.Page != index || v.PageCount != 4 || m.noteCount() != 6 || m.pageKey != 0 {
						t.Fatal("page numbers/count or release state not updated")
					}
					m.record(Hit{MIDI: 72}, now.Add(8*time.Second))
					if m.noteCount() != 7 || len(m.currentPattern()[0]) != 1 {
						t.Fatal("recording into inserted page changed other pages")
					}
				})
			}
		}
	}
}

func TestMultipageLimit64AndLastAllowedInsertion(t *testing.T) {
	now := time.Unix(100, 0)
	for _, code := range []uint16{23, 24} {
		m := newModel(mpSong(63))
		m.Page = 62
		m.handle(keyEvent{Code: code, Down: true}, now)
		m.handle(keyEvent{Code: code}, now.Add(pageHoldDuration))
		if m.Song.pageCount() != 64 || m.Song.validate() != nil {
			t.Fatal("63 -> 64 insertion rejected")
		}
		for _, page := range []int{0, 31, 63} {
			m.Page, m.Step, m.Dirty, m.pageSavePending = page, 5, false, false
			before := m.Song.clone()
			m.handle(keyEvent{Code: code, Down: true}, now)
			m.tick(now.Add(pageHoldDuration))
			m.tick(now.Add(time.Second))
			m.handle(keyEvent{Code: code}, now.Add(2*time.Second))
			mpAssertPosition(t, m, page, 5)
			if !reflect.DeepEqual(m.Song, before) || m.Dirty || m.pageSavePending || m.Notice != "PAGE LIMIT: 64" {
				t.Fatal("limit changed song/cursor or lacked limit notice")
			}
		}
	}
}

func TestMultipageGestureCancellation(t *testing.T) {
	now := time.Unix(100, 0)
	for _, code := range []uint16{23, 24} {
		for _, kind := range []string{"explicit", "reset", "help-L", "help-slash", "power", "wake", "opposite", "note", "left", "export"} {
			t.Run(fmt.Sprintf("key%d/%s", code, kind), func(t *testing.T) {
				m := newModel(mpSong(3))
				m.Page = 1
				m.handle(keyEvent{Code: code, Down: true}, now)
				at := now.Add(100 * time.Millisecond)
				switch kind {
				case "explicit":
					m.cancelPageGesture()
				case "reset":
					out, _ := m.handle(keyEvent{Reset: true}, at)
					if len(out) != 1 || out[0].Kind != "off-all" {
						t.Fatal("reset did not release notes")
					}
				case "help-L":
					press(m, 38, at)
				case "help-slash":
					press(m, 53, at)
				case "power":
					press(m, 116, at)
				case "wake":
					press(m, 143, at)
				case "opposite":
					other := uint16(47) - code
					press(m, other, at)
					m.handle(keyEvent{Code: other}, at.Add(time.Millisecond))
				case "note":
					press(m, 30, at)
					m.handle(keyEvent{Code: 30}, at.Add(time.Millisecond))
				case "left":
					press(m, 105, at)
				case "export":
					_, action := m.handle(keyEvent{Code: 25, Down: true}, at)
					if action != "export" {
						t.Fatal("export action missing")
					}
					// Mirror main's explicit cancellation before starting export.
					m.cancelPageGesture()
					m.stop()
				}
				m.tick(now.Add(time.Second))
				m.handle(keyEvent{Code: code}, now.Add(1100*time.Millisecond))
				if m.Song.pageCount() != 3 || m.Page != 1 || m.Dirty || m.pageKey != 0 {
					t.Fatal("cancelled gesture inserted or navigated on tick/release")
				}
				m.Help = false
				mpTap(m, 24, now.Add(2*time.Second))
				if m.Page != 2 || m.Song.pageCount() != 3 {
					t.Fatal("cancelled gesture prevented next independent tap")
				}
			})
		}
	}
}

func TestMultipageHelpAndExportIgnorePageInput(t *testing.T) {
	now := time.Unix(100, 0)
	for _, exporting := range []bool{false, true} {
		m := newModel(mpSong(3))
		m.Page, m.Step, m.StepMode, m.Help = 1, 8, true, !exporting
		for _, code := range []uint16{23, 24} {
			handleInput(m, keyEvent{Code: code, Down: true}, now, exporting)
			m.tick(now.Add(time.Second))
			handleInput(m, keyEvent{Code: code}, now.Add(time.Second), exporting)
		}
		mpAssertPosition(t, m, 1, 8)
		if m.Song.pageCount() != 3 || m.Dirty || m.pageKey != 0 {
			t.Fatal("hidden/disabled page controls changed project")
		}
	}
}

func TestMultipagePageDownAnchorsCurrentPlaybackPage(t *testing.T) {
	now := time.Unix(100, 0)
	m := newModel(mpSong(3))
	m.startFrom(now, 15)
	m.Recording = true
	m.tick(now)
	at := now.Add(m.stepDuration() + m.stepDuration()/2)
	press(m, 24, at) // Input before tick must first synchronize to page two.
	mpAssertPosition(t, m, 1, 0)
	m.tick(at.Add(pageHoldDuration))
	mpAssertPosition(t, m, 2, 0)
	m.handle(keyEvent{Code: 24}, at.Add(time.Second))
	if m.Song.pageCount() != 4 || m.Playing || !m.StepMode || !m.Recording {
		t.Fatal("hold followed running clock or changed recording state")
	}
}

func TestMultipageLRStaysWithinSelectedPage(t *testing.T) {
	now := time.Unix(100, 0)
	for page := 0; page < 3; page++ {
		m := newModel(mpSong(3))
		m.Page = page
		press(m, 105, now)
		mpAssertPosition(t, m, page, 0)
		for i := 1; i <= 2*steps; i++ {
			press(m, 105, now)
			mpAssertPosition(t, m, page, (steps-i%steps)%steps)
		}
		for i := 1; i <= 2*steps; i++ {
			press(m, 106, now)
			mpAssertPosition(t, m, page, i%steps)
		}
		if m.Dirty || m.Playing || !m.StepMode {
			t.Fatal("page-local browse changed content or started playback")
		}
	}
	m := newModel(mpSong(3))
	m.startFrom(now, 15)
	press(m, 105, now.Add(m.stepDuration()))
	mpAssertPosition(t, m, 1, 15) // Sync page, then wrap locally rather than return to page one.
}

func TestMultipagePlaybackAllOffsetsAndWrap(t *testing.T) {
	now := time.Unix(100, 0)
	s := mpSong(3)
	for p := 0; p < 3; p++ {
		for cell := 0; cell < steps; cell++ {
			s.patternAt(p)[cell] = []Hit{{MIDI: 36 + p*steps + cell, Tone: p}, {Drum: cell%4 + 1}}
		}
	}
	for start := 0; start < 3*steps; start++ {
		t.Run(fmt.Sprintf("start%d", start), func(t *testing.T) {
			m := newModel(s.clone())
			m.Page, m.Step, m.StepMode = start/steps, start%steps, true
			press(m, 57, now)
			for i := 0; i <= 2*3*steps; i++ {
				at := now.Add(time.Duration(i) * m.stepDuration())
				out := m.tick(at)
				position := (start + i) % (3 * steps)
				mpAssertPosition(t, m, position/steps, position%steps)
				if len(out) != 3 || out[0].Kind != "loop-off" || out[1].Kind != "on" || out[1].ID != 300 || out[1].MIDI != 36+position || out[1].Tone != position/steps || out[2].Kind != "drum" || out[2].ID != 301 || out[2].Drum != position%4 {
					t.Fatalf("playback order at position %d: %+v", position, out)
				}
				if len(m.tick(at)) != 0 {
					t.Fatal("same cross-page clock tick retriggered")
				}
			}
			// A long host stall must play only the current cell, without a backlog.
			out := m.tick(now.Add(1001 * m.stepDuration()))
			position := (start + 1001) % (3 * steps)
			if len(out) != 3 || out[1].MIDI != 36+position {
				t.Fatal("stall replayed stale pages or lost global position")
			}
		})
	}
}

func TestMultipageSpaceStartsSelectedEmptyCell(t *testing.T) {
	now := time.Unix(100, 0)
	for _, manual := range []bool{false, true} {
		m := newModel(mpSong(3))
		m.Page = 1
		start := 0
		if manual {
			m.StepMode, m.Step = true, 7
			start = 7
		}
		m.Song.Pattern[0] = []Hit{{MIDI: 60}}
		m.Song.Pages[0][start+1] = []Hit{{MIDI: 72}}
		press(m, 57, now)
		mpAssertPosition(t, m, 1, start)
		if out := m.tick(now); len(out) != 1 || out[0].Kind != "loop-off" {
			t.Fatalf("empty start should be silent rather than skipped: %+v", out)
		}
		out := m.tick(now.Add(m.stepDuration()))
		if len(out) != 2 || out[1].MIDI != 72 {
			t.Fatal("empty-cell start lost selected page offset")
		}
	}
}

func TestMultipageTempoPreservesPagePhaseAndPendingBoundary(t *testing.T) {
	now := time.Unix(100, 0)
	for _, startPage := range []int{0, 2} {
		for _, fraction := range []int{0, 1, 3} {
			for _, processed := range []bool{false, true} {
				for _, bpm := range []int{60, 137, 180} {
					t.Run(fmt.Sprintf("page%d/phase%d/played%v/bpm%d", startPage, fraction, processed, bpm), func(t *testing.T) {
						m := newModel(mpSong(3))
						m.Page = startPage
						m.startFrom(now, 15)
						m.Recording = true
						m.tick(now)
						oldDuration := m.stepDuration()
						at := now.Add(oldDuration + oldDuration*time.Duration(fraction)/4)
						if processed {
							m.tick(at)
						}
						m.setBPM(bpm, at)
						wantPage := (startPage + 1) % 3
						mpAssertPosition(t, m, wantPage, 0)
						wantPhase := oldDuration * time.Duration(fraction) / 4 * m.stepDuration() / oldDuration
						if m.StartStep != wantPage*steps || at.Sub(m.Started) != wantPhase || !m.Playing || !m.Recording || !m.Dirty {
							t.Fatal("tempo lost page, fractional phase, or recording state")
						}
						out := m.tick(at)
						if (processed && len(out) != 0) || (!processed && len(out) != 1) {
							t.Fatalf("tempo duplicated/swallowed boundary: played=%v out=%+v", processed, out)
						}
						boundary := m.Started.Add(m.stepDuration())
						if len(m.tick(boundary.Add(-time.Nanosecond))) != 0 {
							t.Fatal("tempo advanced before preserved boundary")
						}
						m.tick(boundary)
						mpAssertPosition(t, m, wantPage, 1)
						// Quantization after reanchoring must use the new page, not old local cell.
						m.record(Hit{MIDI: 72}, m.Started.Add(m.stepDuration()*3/2))
						if len(m.Song.patternAt(wantPage)[2]) != 1 || m.noteCount() != 1 {
							t.Fatal("post-tempo recording lost global page offset")
						}
					})
				}
			}
		}
	}
}

func TestMultipageAutomaticQuantizationCrossesPagesAndTail(t *testing.T) {
	now := time.Unix(100, 0)
	for start := 0; start < 3*steps; start++ {
		for _, offset := range []time.Duration{-time.Nanosecond, 0, time.Nanosecond} {
			m := newModel(mpSong(3))
			m.Page, m.Step, m.StepMode = start/steps, start%steps, true
			press(m, 19, now)
			for i := 0; i < 3*steps; i++ {
				at := now.Add(time.Duration(i)*m.stepDuration() + m.stepDuration()/2 + offset)
				h := Hit{MIDI: 36 + i}
				m.record(h, at) // Deliberately no display/playback tick before record.
				position := (start + i) % (3 * steps)
				if offset >= 0 {
					position = (position + 1) % (3 * steps)
				}
				if got := m.Song.patternAt(position / steps)[position%steps]; len(got) != 1 || got[0] != h {
					t.Fatalf("start=%d tick=%d midpoint offset=%s: wrong quantized page/cell %d", start, i, offset, position)
				}
			}
			if m.noteCount() != 3*steps {
				t.Fatal("quantized traversal lost hits")
			}
		}
	}
}

func TestMultipageManualRecordAndDeleteResetProject(t *testing.T) {
	now := time.Unix(100, 0)
	m := newModel(mpSong(3))
	for p := 0; p < 3; p++ {
		m.Song.patternAt(p)[2] = []Hit{{MIDI: 60 + p}}
	}
	m.Page, m.Step, m.StepMode, m.Recording = 2, 15, true, true
	for i := 0; i < 9; i++ {
		m.record(Hit{MIDI: 72 + i}, now.Add(time.Hour))
	}
	m.record(Hit{MIDI: 72}, now.Add(time.Hour))
	if len(m.currentPattern()[15]) != 8 || m.noteCount() != 11 || m.Notice != "STEP FULL" {
		t.Fatal("manual recording advanced, duplicated a hit, or exceeded eight voices")
	}
	out := press(m, 111, now)
	if m.noteCount() != 3 || len(out) != 1 || out[0].Kind != "loop-off" || !m.Dirty || !m.Recording || !m.StepMode {
		t.Fatal("first Delete did not clear the selected cell")
	}
	mpAssertPosition(t, m, 2, 15)
	if len(m.Song.patternAt(0)[2]) != 1 || len(m.Song.patternAt(1)[2]) != 1 {
		t.Fatal("single-cell delete changed another page")
	}
	out = press(m, 111, now.Add(time.Second))
	if m.noteCount() != 0 || m.Song.pageCount() != 1 || m.Song.Format != 1 || m.Page != 0 || m.Step != -1 || m.Playing || m.Recording || m.StepMode || len(out) != 1 || out[0].Kind != "loop-off" || !m.Dirty || !m.pageSavePending {
		t.Fatal("second Delete did not clear the whole project")
	}

	m = newModel(mpSong(3))
	for p := 0; p < 3; p++ {
		m.Song.patternAt(p)[2] = []Hit{{MIDI: 60 + p}}
	}
	press(m, 111, now)
	out = press(m, 111, now.Add(time.Second))
	if m.noteCount() != 0 || m.Song.pageCount() != 1 || m.Song.Format != 1 || m.Page != 0 || m.Step != -1 || m.Playing || m.Recording || m.StepMode || len(out) != 1 || out[0].Kind != "loop-off" || !m.Dirty || !m.pageSavePending {
		t.Fatal("confirmed Delete did not reset the project to one empty page")
	}
}

func TestDeleteConfirmationDoesNotCrossPage(t *testing.T) {
	now := time.Unix(100, 0)
	m := newModel(mpSong(2))
	m.Song.Pattern[0], m.Song.Pages[0][0] = []Hit{{MIDI: 60}}, []Hit{{MIDI: 72}}
	press(m, 111, now)
	mpTap(m, 24, now.Add(100*time.Millisecond))
	press(m, 111, now.Add(200*time.Millisecond))
	if m.noteCount() != 1 || m.Song.pageCount() != 2 {
		t.Fatal("page navigation did not cancel project-clear confirmation before cell delete")
	}
	press(m, 111, now.Add(3*time.Second))
	if m.noteCount() != 1 || m.Song.pageCount() != 2 {
		t.Fatal("empty selected-cell delete changed the project")
	}
}

func TestMultipageLegacyMigrationBackupAndRoundtrip(t *testing.T) {
	path := filepath.Join(t.TempDir(), "song.json")
	legacy := demoSong()
	if err := saveSong(path, legacy); err != nil {
		t.Fatal(err)
	}
	original := mpRead(t, path)
	loaded, err := loadSong(path)
	if err != nil || !reflect.DeepEqual(loaded, legacy) || loaded.pageCount() != 1 {
		t.Fatal("format-1 compatibility:", err)
	}
	m := newModel(loaded)
	m.insertPage(-1, time.Unix(100, 0))
	m.Song.Pattern[3] = []Hit{{MIDI: 80, Tone: 2}}
	if err := saveSong(path, m.Song); err != nil {
		t.Fatal("first format-2 save:", err)
	}
	backup := path + ".v1-backup.json"
	if !bytes.Equal(mpRead(t, backup), original) {
		t.Fatal("migration did not preserve original byte-for-byte")
	}
	old, err := loadSong(backup)
	if err != nil || !reflect.DeepEqual(old, legacy) {
		t.Fatal("backup is not a loadable format-1 project:", err)
	}
	m.insertPage(1, time.Unix(101, 0))
	m.Song.Pages[0][15] = []Hit{{Drum: 4}}
	if err := saveSong(path, m.Song); err != nil {
		t.Fatal("subsequent format-2 save:", err)
	}
	got, err := loadSong(path)
	if err != nil || !reflect.DeepEqual(got, m.Song) {
		t.Fatal("multi-page roundtrip lost ordered pages:", err)
	}
	if !bytes.Equal(mpRead(t, backup), original) {
		t.Fatal("second save overwrote legacy backup")
	}
	var fields map[string]json.RawMessage
	if err := json.Unmarshal(mpRead(t, path), &fields); err != nil {
		t.Fatal(err)
	}
	if len(fields) != 7 || fields["pages"] == nil {
		t.Fatal("format-2 schema leaked transient transport/page gesture state")
	}
	fresh := newModel(got)
	if fresh.Page != 0 || fresh.Step != -1 || fresh.Playing || fresh.Recording || fresh.StepMode {
		t.Fatal("reopen restored transient transport state")
	}
}

func TestMultipageMigrationFailuresPreserveOriginal(t *testing.T) {
	legacy, err := json.Marshal(demoSong())
	if err != nil {
		t.Fatal(err)
	}
	for _, kind := range []string{"same-backup", "different-backup", "backup-is-directory", "invalid-original", "unknown-format", "oversized-original", "new-project"} {
		t.Run(kind, func(t *testing.T) {
			path := filepath.Join(t.TempDir(), "song.json")
			original := append([]byte(nil), legacy...)
			success := kind == "same-backup" || kind == "different-backup" || kind == "new-project"
			switch kind {
			case "invalid-original":
				original = []byte(`{"format":1`)
			case "unknown-format":
				original = []byte(`{"format":99}`)
			case "oversized-original":
				original = bytes.Repeat([]byte(" "), maxSongBytes+1)
			}
			if kind != "new-project" {
				mpWrite(t, path, original)
			}
			backup := path + ".v1-backup.json"
			if kind == "same-backup" {
				mpWrite(t, backup, original)
			} else if kind == "different-backup" {
				mpWrite(t, backup, []byte("keep me"))
			} else if kind == "backup-is-directory" {
				if err := os.Mkdir(backup, 0700); err != nil {
					t.Fatal(err)
				}
			}
			err := saveSong(path, mpSong(2))
			if (err == nil) != success {
				t.Fatalf("save success=%v, want %v: %v", err == nil, success, err)
			}
			if !success && !bytes.Equal(mpRead(t, path), original) {
				t.Fatal("failed migration changed original file")
			}
			if kind == "different-backup" && string(mpRead(t, backup)) != "keep me" {
				t.Fatal("failed migration overwrote unrelated backup")
			}
			if kind == "new-project" {
				if _, err := os.Stat(backup); !errors.Is(err, os.ErrNotExist) {
					t.Fatal("new format-2 project fabricated a legacy backup")
				}
			}
			leftovers, err := filepath.Glob(filepath.Join(filepath.Dir(path), ".song-*"))
			if err != nil || len(leftovers) != 0 {
				t.Fatal("migration left temporary files:", leftovers, err)
			}
		})
	}
}

func TestMultipageValidationChecksEveryPage(t *testing.T) {
	for _, tc := range []struct {
		name string
		edit func(*Song)
	}{
		{"format-zero", func(s *Song) { s.Format = 0 }},
		{"future-format", func(s *Song) { s.Format = 3 }},
		{"format-one-with-pages", func(s *Song) { s.Format = 1 }},
		{"65-pages", func(s *Song) { s.Pages = make([][steps][]Hit, 64) }},
		{"last-page-pitch", func(s *Song) { s.Pages[62][15] = []Hit{{MIDI: 97}} }},
		{"last-page-tone", func(s *Song) { s.Pages[62][15] = []Hit{{MIDI: 60, Tone: 3}} }},
		{"last-page-drum", func(s *Song) { s.Pages[62][15] = []Hit{{Drum: 5}} }},
		{"last-page-nine-hits", func(s *Song) { s.Pages[62][15] = make([]Hit, 9) }},
		{"bpm", func(s *Song) { s.BPM = 181 }},
		{"octave", func(s *Song) { s.Octave = 2 }},
		{"volume", func(s *Song) { s.Volume = 101 }},
	} {
		t.Run(tc.name, func(t *testing.T) {
			s := mpSong(64)
			tc.edit(&s)
			if s.validate() == nil {
				t.Fatal("invalid song accepted")
			}
			path := filepath.Join(t.TempDir(), "song.json")
			original := []byte("original must survive")
			mpWrite(t, path, original)
			if err := saveSong(path, s); err == nil || !bytes.Equal(mpRead(t, path), original) {
				t.Fatal("invalid save accepted or original changed:", err)
			}
			data, err := json.Marshal(s)
			if err != nil {
				t.Fatal(err)
			}
			mpWrite(t, path, data)
			if _, err := loadSong(path); err == nil || !bytes.Equal(mpRead(t, path), data) {
				t.Fatal("invalid load accepted or source changed:", err)
			}
		})
	}
}

func TestMultipageStorageByteLimitAndMaximumDensity(t *testing.T) {
	s := mpSong(64)
	for p := 0; p < 64; p++ {
		for cell := 0; cell < steps; cell++ {
			for n := 0; n < 8; n++ {
				s.patternAt(p)[cell] = append(s.patternAt(p)[cell], Hit{MIDI: 36 + n, Tone: n % 3})
			}
		}
	}
	path := filepath.Join(t.TempDir(), "song.json")
	if err := saveSong(path, s); err != nil {
		t.Fatal("maximum valid song cannot be saved:", err)
	}
	data := mpRead(t, path)
	if len(data) > maxSongBytes {
		t.Fatal("saved project exceeds declared byte limit")
	}
	got, err := loadSong(path)
	if err != nil || !reflect.DeepEqual(got, s) || newModel(got).noteCount() != 8192 {
		t.Fatal("maximum valid song did not roundtrip:", err)
	}
	base, err := json.Marshal(mpSong(2))
	if err != nil {
		t.Fatal(err)
	}
	exact := append(base, bytes.Repeat([]byte(" "), maxSongBytes-len(base))...)
	mpWrite(t, path, exact)
	if _, err := loadSong(path); err != nil {
		t.Fatal("exactly maxSongBytes must be readable:", err)
	}
	mpWrite(t, path, append(exact, ' '))
	if _, err := loadSong(path); err == nil {
		t.Fatal("maxSongBytes+1 accepted")
	}
	for _, suffix := range []string{` {}`, ` null`, ` garbage`} {
		mpWrite(t, path, append(append([]byte(nil), base...), suffix...))
		if _, err := loadSong(path); err == nil {
			t.Fatalf("accepted trailing JSON/data %q", suffix)
		}
	}
	unknown := bytes.Replace(base, []byte(`"format":2`), []byte(`"format":2,"surprise":true`), 1)
	mpWrite(t, path, unknown)
	if _, err := loadSong(path); err == nil {
		t.Fatal("unknown JSON field accepted")
	}
}

func TestMultipageCloneIsIndependentExportSnapshot(t *testing.T) {
	for _, pages := range []int{1, 3, 64} {
		s := mpSong(pages)
		for p := 0; p < pages; p++ {
			for cell := 0; cell < steps; cell++ {
				hits := make([]Hit, 1, 8) // Spare capacity catches shallow slice copying.
				hits[0] = Hit{MIDI: 60 + cell, Tone: p % 3}
				s.patternAt(p)[cell] = hits
			}
		}
		before, _ := json.Marshal(s)
		snapshot := s.clone()
		for p := 0; p < pages; p++ {
			for cell := 0; cell < steps; cell++ {
				s.patternAt(p)[cell][0].MIDI = 96
				s.patternAt(p)[cell] = append(s.patternAt(p)[cell], Hit{Drum: 4})
			}
		}
		s.BPM, s.Volume = 180, 0
		s.Pages = append(s.Pages, [steps][]Hit{})
		after, _ := json.Marshal(snapshot)
		if !bytes.Equal(before, after) {
			t.Fatalf("%d-page export snapshot aliases live song", pages)
		}
		snapshot.Pattern[0][0].MIDI = 36
		if s.Pattern[0][0].MIDI != 96 {
			t.Fatal("snapshot mutation changed live first page")
		}
		if pages > 1 {
			snapshot.Pages[pages-2][15][0].MIDI = 36
			if s.Pages[pages-2][15][0].MIDI != 96 {
				t.Fatal("snapshot mutation changed live final page")
			}
		}
	}
}

// An independent flat score renderer: absolute sample boundaries, one continuous
// synth, and one final release. It does not call exportWAV, patternAt, applySound,
// or encodePCM, so a page gap, reordering, or per-page rounding is observable.
func mpReferencePCM(song Song, flat [][]Hit) []byte {
	denom := int64(song.BPM * 2)
	boundary := func(cell int) int { return int((int64(cell)*sampleRate*60 + denom - 1) / denom) }
	frames := int(int64(len(flat))*sampleRate*60/denom) + sampleRate
	synth := NewSynth()
	synth.SetVolume(song.Volume)
	pcm := make([]int16, frames*2)
	for cell, hits := range flat {
		for i := 0; i < 8; i++ {
			synth.NoteOff(300 + i)
		}
		for i, hit := range hits {
			if hit.Drum != 0 {
				synth.Drum(300+i, hit.Drum-1)
			} else {
				synth.NoteOn(300+i, hit.MIDI, hit.Tone, 0.8)
			}
		}
		synth.Render(pcm[2*boundary(cell) : 2*boundary(cell+1)])
	}
	synth.AllOff()
	synth.Render(pcm[2*boundary(len(flat)):])
	raw := make([]byte, frames*4)
	for i, value := range pcm {
		binary.LittleEndian.PutUint16(raw[i*2:], uint16(value))
	}
	return raw
}

func TestMultipageWAVContinuousOrderedScoreAndSinglePageCompatibility(t *testing.T) {
	for _, pages := range []int{1, 3} {
		for _, bpm := range []int{120, 137, 180} {
			t.Run(fmt.Sprintf("pages%d/bpm%d", pages, bpm), func(t *testing.T) {
				s := mpSong(pages)
				s.BPM = bpm
				flat := make([][]Hit, pages*steps)
				for p := 0; p < pages; p++ {
					for cell := 0; cell < steps; cell++ {
						// Include empty cells, all timbres, drums, and occupied page boundaries.
						if cell%5 != 2 {
							flat[p*steps+cell] = []Hit{{MIDI: 48 + p*steps + cell, Tone: p % 3}, {Drum: cell%4 + 1}}
						}
						if p == 0 {
							s.Pattern[cell] = flat[cell]
						} else {
							s.Pages[p-1][cell] = flat[p*steps+cell]
						}
					}
				}
				path := filepath.Join(t.TempDir(), "score.wav")
				if err := exportWAV(path, s); err != nil {
					t.Fatal(err)
				}
				got := mpRead(t, path)
				want := mpReferencePCM(s, flat)
				if len(got) != 44+len(want) || string(got[:4]) != "RIFF" || string(got[8:16]) != "WAVEfmt " || string(got[36:40]) != "data" {
					t.Fatalf("invalid WAV header/duration: bytes=%d want=%d", len(got), 44+len(want))
				}
				if binary.LittleEndian.Uint32(got[4:8]) != uint32(len(got)-8) || binary.LittleEndian.Uint32(got[40:44]) != uint32(len(want)) || binary.LittleEndian.Uint16(got[22:24]) != 2 || binary.LittleEndian.Uint32(got[24:28]) != sampleRate || binary.LittleEndian.Uint16(got[34:36]) != 16 {
					t.Fatal("WAV PCM format/length fields changed")
				}
				if !bytes.Equal(got[44:], want) {
					for i := range want {
						if got[44+i] != want[i] {
							t.Fatalf("ordered gap-free reference mismatch at audio frame %d (page count %d, bpm %d)", i/4, pages, bpm)
						}
					}
				}
				if err := exportWAV(path, s); err == nil || !bytes.Equal(mpRead(t, path), got) {
					t.Fatal("export overwrote existing WAV:", err)
				}
			})
		}
	}
}

func TestMultipageExportCancellationAndValidationLeaveNoFile(t *testing.T) {
	for _, invalid := range []bool{false, true} {
		path := filepath.Join(t.TempDir(), "score.wav")
		s := mpSong(3)
		ctx := context.Background()
		if invalid {
			s.Pages[1][15] = []Hit{{MIDI: 200}}
		} else {
			var cancel context.CancelFunc
			ctx, cancel = context.WithCancel(ctx)
			cancel()
		}
		err := exportWAVContext(ctx, path, s)
		if err == nil || (!invalid && !errors.Is(err, context.Canceled)) {
			t.Fatal("cancelled/invalid export succeeded or lost cancellation:", err)
		}
		if _, err := os.Stat(path); !errors.Is(err, os.ErrNotExist) {
			t.Fatal("cancelled/invalid export left an output file")
		}
	}
}

func TestMultipageCursorRefreshBypasses250msDeadline(t *testing.T) {
	now := time.Unix(100, 0)
	for _, kind := range []string{"cell", "page-only", "playback-boundary", "hold-insert"} {
		t.Run(kind, func(t *testing.T) {
			m := newModel(mpSong(3))
			m.StepMode, m.Step = true, 0
			at := now.Add(time.Millisecond)
			if kind == "playback-boundary" {
				m.startFrom(now.Add(-m.stepDuration()+time.Millisecond), 15)
				m.tick(now)
			} else if kind == "hold-insert" {
				press(m, 24, now.Add(-pageHoldDuration+time.Millisecond))
			}
			r := newRefreshScheduler(render(m, now), now)
			calls := 0
			submit := func(frame) { calls++ }
			r.update(m, now, true, submit)
			switch kind {
			case "cell":
				m.Step = 1
			case "page-only":
				m.Page = 1 // Same cell, identical empty pattern: page alone must invalidate.
			case "playback-boundary", "hold-insert":
				m.tick(at)
			}
			if !at.Before(r.next) {
				t.Fatal("fixture failed to exercise an outstanding background deadline")
			}
			r.update(m, at, false, submit)
			if calls != 1 || r.last != render(m, at) {
				t.Fatalf("cursor change waited for 250ms deadline: %d submissions", calls)
			}
			r.update(m, at, false, submit)
			r.update(m, at, true, submit)
			if calls != 1 || !r.next.Equal(now.Add(backgroundRefreshInterval)) {
				t.Fatal("duplicate cursor frame submitted or background deadline postponed")
			}
		})
	}
}

func TestMultipageLoadRejectsOverlongPageArrays(t *testing.T) {
	// Regression for silent note loss: encoding/json truncates excess elements
	// when decoding a JSON array into [16][]Hit unless the loader checks shape.
	for _, field := range []string{"pattern", "pages"} {
		t.Run(field, func(t *testing.T) {
			cells := make([][]Hit, 17)
			cells[16] = []Hit{{MIDI: 72}}
			payload := map[string]any{"format": 2, "bpm": 120, "octave": 4, "tone": 0, "volume": 45, "pattern": make([][]Hit, 16)}
			if field == "pattern" {
				payload["pattern"] = cells
			} else {
				payload["pages"] = []any{cells}
			}
			data, err := json.Marshal(payload)
			if err != nil {
				t.Fatal(err)
			}
			path := filepath.Join(t.TempDir(), "song.json")
			mpWrite(t, path, data)
			loaded, err := loadSong(path)
			if !bytes.Equal(mpRead(t, path), data) {
				t.Fatal("loading malformed page changed original")
			}
			if err == nil {
				t.Fatalf("REPRO: write format-2 JSON with 17 cells in %s and a MIDI-72 hit in cell 17; loadSong succeeds with %d notes, silently discarding the hit; expected format validation error", field, newModel(loaded).noteCount())
			}
		})
	}
}
