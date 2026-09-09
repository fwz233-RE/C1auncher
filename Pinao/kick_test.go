package main

import (
	"math"
	"testing"
	"time"
)

// A simple high-pass is a regression proxy for missing low bass, NOT a model
// or measurement of the device speaker. Compare at the same master volume.
func kickUpperEnergy(pcm []int16) float64 {
	alpha := math.Exp(-2 * math.Pi * 500 / sampleRate)
	var previous, filtered, sum float64
	for i := 0; i+1 < len(pcm); i += 2 {
		x := float64(pcm[i])
		filtered = alpha * (filtered + x - previous)
		previous = x
		sum += filtered * filtered
	}
	return sum / float64(len(pcm)/2)
}

// Reference for the old kick: a pure sine sweeping from 150 towards 45 Hz.
// Only the first 80 ms are needed, before timeout/release and without steals.
func legacyKickAttack(frames int, volume int) []int16 {
	pcm := make([]int16, frames*2)
	pitch := float32(105)
	pitchDecay := float32(math.Exp(-1 / (0.025 * sampleRate)))
	decay := float32(math.Exp(-1 / (0.09 * sampleRate)))
	var phase uint32
	var envelope float32
	gain := float32(volume) / 100 * (32767 * 0.105)
	for i := 0; i < frames; i++ {
		if i < sampleRate/500 {
			envelope = float32(i+1) / float32(sampleRate/500)
		} else {
			envelope *= decay
		}
		value := sine(phase)
		phase += uint32((45 + pitch) * phaseScale)
		pitch *= pitchDecay
		pcm[i*2] = int16(value * envelope * gain)
		pcm[i*2+1] = pcm[i*2]
	}
	return pcm
}

func TestKickHasSpeakerBandAttack(t *testing.T) {
	const frames = sampleRate * 80 / 1000
	s := NewSynth()
	s.SetVolume(defaultSong().Volume)
	s.Drum(200, 0)
	pcm := renderFrames(s, frames)
	old := legacyKickAttack(frames, defaultSong().Volume)
	before, after := kickUpperEnergy(old), kickUpperEnergy(pcm)
	if after < 4*before {
		t.Fatalf("kick needs stronger upper-frequency attack at unchanged volume: old=%g new=%g", before, after)
	}
	if pcm[0] != 0 {
		t.Fatal("kick must retain a smooth zero start")
	}
}

func TestKickMixBoundsAndAllocations(t *testing.T) {
	s := NewSynth()
	s.SetVolume(100)
	var pcm [480]int16
	allocs := testing.AllocsPerRun(100, func() {
		for i := 0; i < voiceCount; i++ {
			s.Drum(200+i, 0)
		}
		s.Render(pcm[:])
	})
	if allocs != 0 {
		t.Fatalf("kick rendering allocated: %g", allocs)
	}
	for _, x := range renderFrames(s, sampleRate) {
		if math.Abs(float64(x)) > 27525 {
			t.Fatalf("kick mix exceeded existing headroom: %d", x)
		}
	}
}

func TestCKeyToKickPCM(t *testing.T) {
	m := newModel(defaultSong())
	s := NewSynth()
	s.SetVolume(m.Song.Volume)
	now := time.Unix(100, 0)
	commands, action := m.handle(keyEvent{Code: 46, Down: true}, now)
	if action != "" || len(commands) != 1 || commands[0].Kind != "drum" || commands[0].Drum != 0 {
		t.Fatalf("C must trigger kick: %v %s", commands, action)
	}
	for _, c := range commands {
		applySound(s, c)
	}
	// A short tap must not cancel this one-shot drum on key release.
	commands, _ = m.handle(keyEvent{Code: 46}, now.Add(10*time.Millisecond))
	for _, c := range commands {
		applySound(s, c)
	}
	pcm := renderFrames(s, sampleRate/10)
	if energy(pcm) == 0 || kickUpperEnergy(pcm) == 0 {
		t.Fatal("C key produced silent PCM")
	}
	renderFrames(s, sampleRate)
	if s.Active() || energy(renderFrames(s, 240)) != 0 {
		t.Fatal("kick failed to end")
	}
	s.SetVolume(0)
	applySound(s, soundCommand{Kind: "drum", ID: 200, Drum: 0})
	if energy(renderFrames(s, 2400)) != 0 {
		t.Fatal("kick bypassed mute")
	}
}
