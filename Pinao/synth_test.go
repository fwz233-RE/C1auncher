package main

import (
	"math"
	"testing"
)

func renderFrames(s *Synth, frames int) []int16 {
	pcm := make([]int16, frames*2)
	s.Render(pcm)
	return pcm
}

func energy(pcm []int16) float64 {
	var sum float64
	for i := 0; i+1 < len(pcm); i += 2 {
		x := float64(pcm[i])
		sum += x * x
	}
	if len(pcm) < 2 {
		return 0
	}
	return sum / float64(len(pcm)/2)
}

func liveVoices(s *Synth) int {
	n := 0
	for _, v := range s.voices {
		if v.active {
			n++
		}
	}
	return n
}

func hasID(s *Synth, id int) bool {
	for _, v := range s.voices {
		if v.active && v.id == id {
			return true
		}
	}
	return false
}

func TestSilenceAndStereo(t *testing.T) {
	s := NewSynth()
	pcm := make([]int16, 2001)
	for i := range pcm {
		pcm[i] = 1234
	}
	s.Render(pcm)
	for i, x := range pcm {
		if x != 0 {
			t.Fatalf("silence[%d]=%d", i, x)
		}
	}
	s.NoteOn(1, 69, 0, 1)
	s.Render(pcm)
	if energy(pcm) == 0 {
		t.Fatal("note is silent")
	}
	for i := 0; i+1 < len(pcm); i += 2 {
		if pcm[i] != pcm[i+1] {
			t.Fatal("channels differ")
		}
	}
	if pcm[len(pcm)-1] != 0 {
		t.Fatal("odd trailing sample not zero")
	}
}

func TestPitchAndScale(t *testing.T) {
	for timbre := 0; timbre < 3; timbre++ {
		for _, midi := range []int{36, 48, 60, 69, 72, 84, 96} {
			s := NewSynth()
			s.NoteOn(1, midi, timbre, 1)
			renderFrames(s, sampleRate/2)
			pcm := renderFrames(s, sampleRate)
			crossings := 0
			for i := 2; i < len(pcm); i += 2 {
				if pcm[i-2] <= 0 && pcm[i] > 0 {
					crossings++
				}
			}
			want := 440 * math.Exp2(float64(midi-69)/12)
			if math.Abs(float64(crossings)-want) > 2 {
				t.Fatalf("timbre %d MIDI %d: measured %d Hz, want %.3f", timbre, midi, crossings, want)
			}
		}
	}
}

func TestEnvelopeReleaseAndTimeout(t *testing.T) {
	s := NewSynth()
	s.NoteOn(1, 69, 0, 1)
	pcm := renderFrames(s, 2000)
	if pcm[0] != 0 {
		t.Fatal("attack does not start at zero")
	}
	if energy(pcm[:96]) >= energy(pcm[600:1000]) {
		t.Fatal("attack does not rise")
	}
	if energy(renderFrames(s, sampleRate*2)) == 0 {
		t.Fatal("missing sustain")
	}
	before := s.voices[0].envelope
	s.NoteOff(1)
	s.NoteOff(1)
	if s.voices[0].envelope != before {
		t.Fatal("NoteOff abruptly changed envelope")
	}
	renderFrames(s, releaseSamples/2)
	if got := s.voices[0].envelope; got <= 0 || got >= before {
		t.Fatal("release is not gradual")
	}
	left := s.voices[0].releaseLeft
	s.NoteOff(1)
	if s.voices[0].releaseLeft != left {
		t.Fatal("repeated NoteOff restarted release")
	}
	renderFrames(s, releaseSamples)
	if liveVoices(s) != 0 || energy(renderFrames(s, 512)) != 0 {
		t.Fatal("release did not become silent")
	}
	for kind := 0; kind < 3; kind++ {
		s.NoteOn(kind, 69, kind, 1)
	}
	renderFrames(s, maxHoldSamples+releaseSamples+1)
	if liveVoices(s) != 0 {
		t.Fatal("lost-keyup timeout failed")
	}
}

func TestRetriggerStealingAndUniqueIDs(t *testing.T) {
	s := NewSynth()
	for i := 0; i < voiceCount; i++ {
		s.NoteOn(i, 60+i, 0, 1)
	}
	renderFrames(s, 1111)
	s.NoteOn(3, 72, 1, 0.8)
	if liveVoices(s) != voiceCount {
		t.Fatal("retrigger consumed a slot")
	}
	s.NoteOn(8, 69, 0, 1)
	if hasID(s, 0) || !hasID(s, 8) || !hasID(s, 3) {
		t.Fatal("oldest voice was not stolen")
	}
	s.NoteOff(0) // A late keyup for the stolen voice must not release its replacement.
	for _, v := range s.voices {
		if v.releasing {
			t.Fatal("stale keyup released another ID")
		}
	}
	s.NoteOff(3)
	for _, v := range s.voices {
		if v.releasing != (v.id == 3) {
			t.Fatal("NoteOff was not ID-specific")
		}
	}
	s.AllOff()
	renderFrames(s, releaseSamples+1)
	if liveVoices(s) != 0 {
		t.Fatal("AllOff failed")
	}
}

func TestStealContinuity(t *testing.T) {
	s := NewSynth()
	s.NoteOn(1, 69, 0, 1)
	pcm := renderFrames(s, 555)
	last := pcm[len(pcm)-2]
	s.NoteOn(1, 84, 2, 1)
	pcm = renderFrames(s, 1)
	if d := math.Abs(float64(pcm[0]) - float64(last)); d > 1 {
		t.Fatalf("retrigger discontinuity: %g", d)
	}
}

func TestTimbresAndDrums(t *testing.T) {
	var sounds [][]int16
	for kind := 0; kind < 3; kind++ {
		s := NewSynth()
		s.NoteOn(1, 69, kind, 1)
		pcm := renderFrames(s, sampleRate*2)
		if energy(pcm[:sampleRate/2]) <= energy(pcm[len(pcm)-sampleRate/2:]) {
			t.Fatalf("timbre %d does not decay", kind)
		}
		sounds = append(sounds, pcm[:4096])
	}
	for kind := 0; kind < 4; kind++ {
		s := NewSynth()
		s.Drum(1, kind)
		pcm := renderFrames(s, sampleRate)
		if energy(pcm[:4096]) == 0 {
			t.Fatalf("drum %d silent", kind)
		}
		if energy(pcm[len(pcm)-4096:]) != 0 || liveVoices(s) != 0 {
			t.Fatalf("drum %d did not end", kind)
		}
		if energy(pcm[:2048]) <= energy(pcm[8000:10048]) {
			t.Fatalf("drum %d did not decay", kind)
		}
		sounds = append(sounds, pcm[:4096])
	}
	for i := range sounds {
		for j := 0; j < i; j++ {
			equal := true
			for k := range sounds[i] {
				if sounds[i][k] != sounds[j][k] {
					equal = false
					break
				}
			}
			if equal {
				t.Fatalf("sounds %d and %d identical", i, j)
			}
		}
	}
}

func TestPolyphonyVolumeAndBounds(t *testing.T) {
	one, many := NewSynth(), NewSynth()
	one.SetVolume(100)
	many.SetVolume(100)
	one.NoteOn(0, 69, 2, 1)
	for i := 0; i < voiceCount; i++ {
		many.NoteOn(i, 69, 2, 1)
	}
	a, b := renderFrames(one, 4096), renderFrames(many, 4096)
	ratio := energy(b) / energy(a)
	if ratio < 63 || ratio > 65 {
		t.Fatalf("eight voices energy ratio = %g, want 64", ratio)
	}
	for _, x := range b {
		if x >= 32767 || x <= -32767 {
			t.Fatal("clipped PCM")
		}
	}
	many.SetVolume(-100)
	if energy(renderFrames(many, 512)) != 0 {
		t.Fatal("volume zero not silent")
	}
	many.SetVolume(1000)
	if many.volume != 1 {
		t.Fatal("volume not clamped")
	}
	// Exercise overlapping replacement fades, drums, and notes at full volume.
	for n := 0; n < 300; n++ {
		if n%2 == 0 {
			many.Drum(n%13, n%4)
		} else {
			many.NoteOn(n%13, 36+n%61, n%3, 10)
		}
		pcm := renderFrames(many, 37)
		for _, x := range pcm {
			if math.Abs(float64(x)) > 27525 {
				t.Fatalf("mix exceeded headroom: %d", x)
			}
		}
		for _, v := range many.voices {
			for _, x := range []float32{v.envelope, v.last, v.transient, v.pitch} {
				if math.IsNaN(float64(x)) || math.IsInf(float64(x), 0) {
					t.Fatal("nonfinite state")
				}
			}
		}
	}
	low, high := NewSynth(), NewSynth()
	low.SetVolume(50)
	high.SetVolume(100)
	low.NoteOn(1, 69, 0, 1)
	high.NoteOn(1, 69, 0, 1)
	ratio = energy(renderFrames(high, 4096)) / energy(renderFrames(low, 4096))
	if ratio < 3.99 || ratio > 4.02 {
		t.Fatalf("master volume scaling = %g", ratio)
	}
}

func TestInvalidInputs(t *testing.T) {
	s := NewSynth()
	s.Render(nil)
	s.Render([]int16{})
	s.Render([]int16{123})
	s.NoteOff(-999)
	s.AllOff()
	for _, velocity := range []float64{math.NaN(), math.Inf(1), math.Inf(-1), -1, 0} {
		s.NoteOn(1, 69, 0, velocity)
		if liveVoices(s) != 0 {
			t.Fatal("invalid velocity created a voice")
		}
	}
	for _, midi := range []int{-int(^uint(0)>>1) - 1, -100, 0, 36, 96, 200, int(^uint(0) >> 1)} {
		s.NoteOn(midi, midi, -99, 100)
	}
	renderFrames(s, 1000)
	for _, v := range s.voices {
		if !v.active {
			continue
		}
		f := float64(v.step[0]) / phaseScale
		if f < 65.40 || f > 2093.01 {
			t.Fatalf("MIDI clamp failed: %g", f)
		}
	}
	s.Drum(-1, -1)
	s.Drum(-1, 99)
	renderFrames(s, sampleRate)
}

func TestRenderNoAllocations(t *testing.T) {
	s := NewSynth()
	pcm := make([]int16, 512)
	allocs := testing.AllocsPerRun(100, func() {
		for i := 0; i < voiceCount; i++ {
			s.NoteOn(i, 60+i, i%3, 0.8)
		}
		s.Drum(0, 1)
		s.Render(pcm)
		s.NoteOff(1)
		s.AllOff()
	})
	if allocs != 0 {
		t.Fatalf("control/render allocated %g times", allocs)
	}
}

func BenchmarkRenderEightVoices(b *testing.B) {
	s := NewSynth()
	var pcm [512]int16
	b.ReportAllocs()
	for n := 0; n < b.N; n++ {
		if n%100 == 0 {
			for i := 0; i < voiceCount; i++ {
				s.NoteOn(i, 60+i, i%3, 0.8)
			}
		}
		s.Render(pcm[:])
	}
}
