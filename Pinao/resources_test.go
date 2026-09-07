package main

import (
	"bytes"
	"context"
	"encoding/binary"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"runtime/debug"
	"strings"
	"testing"
	"time"
)

// Build every step before allocation measurements so only model work is counted.
func resourceFullSong() Song {
	s := defaultSong()
	s.BPM = 60
	for step := range s.Pattern {
		s.Pattern[step] = make([]Hit, 8)
		for i := range s.Pattern[step] {
			s.Pattern[step][i] = Hit{MIDI: 48 + (step+i)%37, Tone: i % 3}
		}
		s.Pattern[step][7] = Hit{Drum: step%4 + 1}
	}
	return s
}

func TestResourcesSparkBufferNoAllocations(t *testing.T) {
	m := newModel(defaultSong())
	now := time.Unix(100, 0)
	if len(m.Sparks) != 0 || cap(m.Sparks) != 32 {
		t.Fatalf("initial sparks len/cap = %d/%d", len(m.Sparks), cap(m.Sparks))
	}
	next := 0
	allocs := testing.AllocsPerRun(1000, func() {
		for i := 0; i < 100; i++ {
			m.addSpark(next, next%2 == 0, now)
			next++
		}
	})
	if allocs != 0 {
		t.Fatalf("repeated addSpark allocated %g times per run", allocs)
	}
	if len(m.Sparks) != 32 || cap(m.Sparks) != 32 || &m.Sparks[0] != &m.sparkBuffer[0] {
		t.Fatal("spark storage grew or stopped using the fixed buffer")
	}
	for i, s := range m.Sparks {
		want := next - 32 + i
		if s.Key != want || s.Drum != (want%2 == 0) || !s.At.Equal(now) {
			t.Fatalf("spark %d = %+v, want key %d", i, s, want)
		}
	}
	m.tick(now.Add(2 * time.Second))
	if len(m.Sparks) != 0 || cap(m.Sparks) != 32 {
		t.Fatal("expiring sparks changed their fixed capacity")
	}
	m.addSpark(12, false, now.Add(2*time.Second))
	if &m.Sparks[0] != &m.sparkBuffer[0] {
		t.Fatal("refilling expired sparks replaced their storage")
	}
}

func TestResourcesTickBufferNoAllocations(t *testing.T) {
	m := newModel(resourceFullSong())
	now := time.Unix(100, 0)
	m.start(now)
	duration := m.stepDuration()
	var commands []soundCommand
	allocs := testing.AllocsPerRun(1000, func() {
		now = now.Add(duration)
		commands = m.tick(now)
	})
	if allocs != 0 {
		t.Fatalf("full-step tick allocated %g times", allocs)
	}
	if len(commands) != 9 || cap(commands) != 9 || &commands[0] != &m.commandBuffer[0] {
		t.Fatal("tick did not reuse its nine-command buffer")
	}
	if commands[0] != (soundCommand{Kind: "loop-off"}) {
		t.Fatalf("first command = %+v", commands[0])
	}
	for i, hit := range m.Song.Pattern[m.Step] {
		want := soundCommand{Kind: "on", ID: 300 + i, MIDI: hit.MIDI, Tone: hit.Tone}
		if hit.Drum > 0 {
			want = soundCommand{Kind: "drum", ID: 300 + i, Drum: hit.Drum - 1}
		}
		if commands[i+1] != want {
			t.Fatalf("command %d = %+v, want %+v", i+1, commands[i+1], want)
		}
	}
	if len(m.Sparks) > 32 || cap(m.Sparks) != 32 {
		t.Fatal("tick grew the spark buffer")
	}
	if got := m.tick(now); len(got) != 0 {
		t.Fatal("same timestamp replayed commands")
	}
}

func TestResourcesMainCommandMergeDoesNotAlias(t *testing.T) {
	for _, live := range []bool{false, true} {
		m := newModel(resourceFullSong())
		now := time.Unix(100, 0)
		m.start(now)
		var commands []soundCommand
		if live {
			commands, _ = handleInput(m, keyEvent{Code: 30, Down: true}, now, false)
		}
		// Mirror main.go: tick's borrowed slice is appended into this iteration's
		// independent input-command slice, then consumed before the next tick.
		commands = append(commands, m.tick(now)...)
		wantLength := 9
		if live {
			wantLength++
			if commands[0].Kind != "on" || commands[0].ID != 30 {
				t.Fatal("loop commands overwrote the live note")
			}
		}
		if len(commands) != wantLength {
			t.Fatalf("merged command count = %d, want %d", len(commands), wantLength)
		}
		snapshot := append([]soundCommand(nil), commands...)
		m.tick(now.Add(m.stepDuration()))
		for i := range commands {
			if commands[i] != snapshot[i] {
				t.Fatalf("later tick overwrote merged command %d (live=%v)", i, live)
			}
		}
	}
}

func TestResourcesSilentRenderClearsWithoutAllocations(t *testing.T) {
	s := NewSynth()
	if s.Active() {
		t.Fatal("new synth is active")
	}
	var pcm [481]int16 // Also exercise the unmatched final stereo element.
	allocs := testing.AllocsPerRun(1000, func() {
		for i := range pcm {
			pcm[i] = 12345
		}
		s.Render(pcm[:])
	})
	if allocs != 0 {
		t.Fatalf("silent Render allocated %g times", allocs)
	}
	for i, sample := range pcm {
		if sample != 0 {
			t.Fatalf("silent sample %d = %d", i, sample)
		}
	}
	s.Render(nil)
	s.Render(pcm[:0])
	for _, drum := range []bool{false, true} {
		if drum {
			s.Drum(1, 0)
		} else {
			s.NoteOn(1, 60, 0, 0.8)
		}
		if !s.Active() {
			t.Fatalf("triggered synth inactive (drum=%v)", drum)
		}
		s.Render(pcm[:])
		s.AllOff()
		for i := 0; i < releaseSamples/240+2; i++ {
			s.Render(pcm[:])
		}
		if s.Active() {
			t.Fatal("released synth never became inactive")
		}
		for i := range pcm {
			pcm[i] = -123
		}
		s.Render(pcm[:])
		for _, sample := range pcm {
			if sample != 0 {
				t.Fatal("inactive synth retained previous audio")
			}
		}
	}
}

func TestResourcesCanceledExportPreservesFiles(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	dir := t.TempDir()
	path := filepath.Join(dir, "canceled.wav")
	if err := exportWAVContext(ctx, path, resourceFullSong()); !errors.Is(err, context.Canceled) {
		t.Fatalf("pre-canceled export = %v", err)
	}
	if _, err := os.Stat(path); !errors.Is(err, os.ErrNotExist) {
		t.Fatalf("pre-canceled export created a file: %v", err)
	}
	original := []byte("existing user audio must survive")
	if err := os.WriteFile(path, original, 0600); err != nil {
		t.Fatal(err)
	}
	for _, exportCtx := range []context.Context{ctx, context.Background()} {
		if err := exportWAVContext(exportCtx, path, resourceFullSong()); err == nil {
			t.Fatal("export accepted an existing path")
		}
		got, err := os.ReadFile(path)
		if err != nil || !bytes.Equal(got, original) {
			t.Fatalf("existing file changed: %q, %v", got, err)
		}
	}
}

// Deterministically cancel at a cooperative checkpoint instead of racing a
// wall-clock timer against fast host rendering. The underlying context remains
// an ordinary cancellable context with a correctly closed Done channel.
type resourceCheckpointContext struct {
	context.Context
	cancel       context.CancelFunc
	checks       int
	beforeCancel func()
}

func (c *resourceCheckpointContext) Err() error {
	c.checks++
	if c.checks == 3 {
		if c.beforeCancel != nil {
			c.beforeCancel()
		}
		c.cancel()
	}
	return c.Context.Err()
}

func TestResourcesExportCancellationRemovesPartialFile(t *testing.T) {
	base, cancel := context.WithCancel(context.Background())
	defer cancel()
	ctx := &resourceCheckpointContext{Context: base, cancel: cancel}
	dir := t.TempDir()
	path := filepath.Join(dir, "partial.wav")
	ctx.beforeCancel = func() {
		info, err := os.Stat(path)
		if err != nil {
			t.Fatalf("cancellation did not reach an opened export: %v", err)
		}
		if info.Size() <= 44 {
			t.Fatalf("cancellation did not reach a written PCM block: %d bytes", info.Size())
		}
	}
	if err := exportWAVContext(ctx, path, resourceFullSong()); !errors.Is(err, context.Canceled) {
		t.Fatalf("checkpoint cancellation = %v", err)
	}
	if ctx.checks < 3 {
		t.Fatal("export did not check cancellation during rendering")
	}
	entries, err := os.ReadDir(dir)
	if err != nil || len(entries) != 0 {
		t.Fatalf("canceled export left output or temporary files: %v, %v", entries, err)
	}
}

func TestResourcesExportStillAcceptsExitKeys(t *testing.T) {
	m := newModel(defaultSong())
	now := time.Unix(100, 0)
	for _, exporting := range []bool{false, true} {
		for _, code := range []uint16{102, 158, 1} {
			commands, action := handleInput(m, keyEvent{Code: code, Down: true}, now, exporting)
			if action != "exit" || len(commands) != 0 {
				t.Fatalf("exit key %d during export=%v: %q, %+v", code, exporting, action, commands)
			}
			_, action = handleInput(m, keyEvent{Code: code}, now, exporting)
			if action != "" {
				t.Fatal("exit key release caused an exit")
			}
		}
	}
	for _, code := range []uint16{30, 19, 25, 28, 38, 57} {
		commands, action := handleInput(m, keyEvent{Code: code, Down: true}, now, true)
		if len(commands) != 0 || action != "" {
			t.Fatalf("non-exit key %d remained active during export", code)
		}
	}
	if m.Playing || m.Recording || m.Help || m.Dirty || len(m.Held) != 0 || len(m.Sparks) != 0 {
		t.Fatal("ignored export input modified the model")
	}
	commands, _ := handleInput(m, keyEvent{Code: 30, Down: true}, now, false)
	if len(commands) != 1 || commands[0].Kind != "on" {
		t.Fatal("normal input no longer reaches the model")
	}
	if exportResult(context.Canceled) != nil {
		t.Fatal("normal export cancellation reported as failure")
	}
	want := errors.New("storage failed")
	if !errors.Is(exportResult(want), want) {
		t.Fatal("exportResult discarded a storage error")
	}
}

func TestResourcesMemoryConfiguration(t *testing.T) {
	previousLimit := debug.SetMemoryLimit(-1)
	previousGC := debug.SetGCPercent(-1)
	defer debug.SetMemoryLimit(previousLimit)
	defer debug.SetGCPercent(previousGC)
	configureMemory()
	if got := debug.SetMemoryLimit(-1); got != 6<<20 {
		t.Fatalf("runtime soft limit = %d, want 6 MiB", got)
	}
	if got := debug.SetGCPercent(50); got != 50 {
		t.Fatalf("GC percent = %d, want 50", got)
	}
}

func TestResourcesDiagnosticsBoundedAndMetadataOnly(t *testing.T) {
	dir := t.TempDir()
	songPath := filepath.Join(dir, "song.json")
	songData := []byte("private-note-content-must-not-be-read")
	if err := os.WriteFile(songPath, songData, 0600); err != nil {
		t.Fatal(err)
	}
	allowed := map[string]bool{
		"version": true, "at": true, "state": true, "error": true,
		"heap_alloc": true, "runtime_bytes": true, "goroutines": true,
	}
	for _, message := range []string{strings.Repeat("x", 10000), strings.Repeat("\x00", 10000)} {
		writeSessionRecord(songPath, "failed", errors.New(message))
		data, err := os.ReadFile(filepath.Join(dir, "last-session.json"))
		if err != nil {
			t.Fatal(err)
		}
		// JSON control-character escaping can expand each retained byte to six
		// bytes. Bound serialized size separately from the decoded error field.
		if len(data) > 4096*6+1024 {
			t.Fatalf("diagnostic record grew to %d bytes", len(data))
		}
		var record map[string]json.RawMessage
		if err := json.Unmarshal(data, &record); err != nil {
			t.Fatal(err)
		}
		for key := range record {
			if !allowed[key] {
				t.Fatalf("unexpected diagnostic field %q; metadata only is allowed", key)
			}
		}
		for key := range allowed {
			if _, ok := record[key]; !ok {
				t.Fatalf("missing diagnostic field %q", key)
			}
		}
		var gotError, state string
		if err := json.Unmarshal(record["error"], &gotError); err != nil {
			t.Fatal(err)
		}
		if gotError != message[:4096] {
			t.Fatalf("diagnostic error was not truncated to 4096 bytes: %d", len(gotError))
		}
		if err := json.Unmarshal(record["state"], &state); err != nil || state != "failed" {
			t.Fatalf("diagnostic state = %q, %v", state, err)
		}
		if bytes.Contains(data, songData) {
			t.Fatal("diagnostic contains private song content")
		}
	}
	writeSessionRecord(songPath, "exited", nil)
	data, err := os.ReadFile(filepath.Join(dir, "last-session.json"))
	if err != nil {
		t.Fatal(err)
	}
	var record map[string]json.RawMessage
	if err := json.Unmarshal(data, &record); err != nil {
		t.Fatal(err)
	}
	if _, ok := record["error"]; ok {
		t.Fatal("new session record retained the old error")
	}
	entries, err := os.ReadDir(dir)
	if err != nil || len(entries) != 2 {
		t.Fatalf("diagnostics accumulated files: %v, %v", entries, err)
	}
	got, err := os.ReadFile(songPath)
	if err != nil || !bytes.Equal(got, songData) {
		t.Fatal("diagnostics modified the song file")
	}
}

func TestResourcesMaximumLoopWAVHeader(t *testing.T) {
	song := resourceFullSong()
	if err := song.validate(); err != nil {
		t.Fatal(err)
	}
	if got := newModel(song).noteCount(); got != 16*8 {
		t.Fatalf("test loop contains %d events, want 128", got)
	}
	path := filepath.Join(t.TempDir(), "maximum.wav")
	if err := exportWAVContext(context.Background(), path, song); err != nil {
		t.Fatal(err)
	}
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	frames := int(int64(sampleRate)*60*steps/int64(song.BPM*2)) + sampleRate
	if len(data) != 44+frames*4 {
		t.Fatalf("WAV length = %d, want %d", len(data), 44+frames*4)
	}
	if string(data[:4]) != "RIFF" || string(data[8:16]) != "WAVEfmt " || string(data[36:40]) != "data" {
		t.Fatal("incorrect WAV chunk identifiers")
	}
	for _, field := range []struct {
		offset int
		want   uint32
	}{{4, uint32(len(data) - 8)}, {16, 16}, {24, sampleRate}, {28, sampleRate * 4}, {40, uint32(frames * 4)}} {
		if got := binary.LittleEndian.Uint32(data[field.offset:]); got != field.want {
			t.Fatalf("WAV uint32 at %d = %d, want %d", field.offset, got, field.want)
		}
	}
	for _, field := range []struct {
		offset int
		want   uint16
	}{{20, 1}, {22, 2}, {32, 4}, {34, 16}} {
		if got := binary.LittleEndian.Uint16(data[field.offset:]); got != field.want {
			t.Fatalf("WAV uint16 at %d = %d, want %d", field.offset, got, field.want)
		}
	}
	if bytes.Count(data[44:], []byte{0}) == len(data)-44 {
		t.Fatal("full loop WAV is silent")
	}
}
