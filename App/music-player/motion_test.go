package main

import (
	"testing"
	"time"

	"c1device"
)

func TestMotionClockLifecycle(t *testing.T) {
	clock := &motionClock{}
	defer clock.stop()
	for _, state := range []appState{
		{}, {playing: true, visual: visualStatic},
		{playing: true, paused: true, visual: visualBars}, {visual: visualRecord},
	} {
		clock.sync(state)
		if clock.ticks != nil || clock.timer != nil {
			t.Fatal("inactive animation created a timer")
		}
	}
	state := appState{playing: true, visual: visualBars}
	clock.sync(state)
	timer, ticks := clock.timer, clock.ticks
	for i := 0; i < 100; i++ {
		clock.sync(state)
	}
	if clock.timer != timer || clock.ticks != ticks {
		t.Fatal("progress re-created the pending timer")
	}
	select {
	case <-clock.ticks:
		clock.ticks = nil
	case <-time.After(2 * time.Second):
		t.Fatal("animation waits for audio progress")
	}
	// A consumed one-shot timer remains silent until explicitly rearmed.
	select {
	case <-timer.C:
		t.Fatal("timer accumulated frames")
	default:
	}
	clock.sync(state)
	if clock.timer != timer {
		t.Fatal("timer not reused")
	}
	state.paused = true
	clock.sync(state)
	if clock.ticks != nil {
		t.Fatal("pause leaves animation selected")
	}
	select {
	case <-timer.C:
		t.Fatal("pause leaves a queued frame")
	default:
	}
	state.paused = false
	clock.sync(state)
	if clock.ticks == nil {
		t.Fatal("resume does not restart animation")
	}
	state.visual = visualStatic
	clock.sync(state)
	if clock.ticks != nil {
		t.Fatal("static mode leaves animation enabled")
	}
}

func TestCachedMotionMatchesFullRenderer(t *testing.T) {
	face, small := uiTestFaces(t)
	cache := &motionFrames{}
	for _, mode := range []visualMode{visualBars, visualRecord} {
		state := appState{
			tracks: []Track{{Title: "测试播放动效", Path: "a.mp3"}}, current: 0,
			playing: true, visual: mode, volume: 100, position: 73 * time.Second, duration: 4 * time.Minute,
		}
		base := renderMusic(state, face, small)
		for phase := 0; phase < 32; phase++ {
			state.visualTick = uint8(phase % 16)
			next := cache.apply(base, state)
			expected := renderMusic(state, face, small)
			if next != expected {
				t.Fatalf("cached frame differs: mode=%d phase=%d", mode, phase)
			}
			for offset := range next {
				x, page := offset%c1device.DisplayWidth, offset/c1device.DisplayWidth
				var coverMask byte
				if x >= musicCoverRect.Min.X && x < musicCoverRect.Max.X {
					for bit := 0; bit < 8; bit++ {
						y := page*8 + bit
						if y >= musicCoverRect.Min.Y && y < musicCoverRect.Max.Y {
							coverMask |= 0x80 >> bit
						}
					}
				}
				if (next[offset]^base[offset])&^coverMask != 0 {
					t.Fatalf("animation changed a static pixel in byte %d", offset)
				}
			}
			base = next
		}
		state.paused = true
		if cache.apply(base, state) != base {
			t.Fatal("paused cover was changed")
		}
	}
}

func TestAnimationAdvancesWithoutAudioPosition(t *testing.T) {
	state := appState{playing: true, visual: visualRecord}
	for phase := 0; phase < 16; phase++ {
		state.visualTick = uint8(phase)
		if visualPhase(state) != phase {
			t.Fatal("phase incorrectly depends on audio timestamp")
		}
	}
}

var benchmarkMotionFrame c1device.Frame

func BenchmarkFullMotion(b *testing.B) {
	face, small := uiTestFaces(b)
	state := appState{tracks: []Track{{Title: "测试播放动效", Path: "a.mp3"}}, playing: true, visual: visualBars, volume: 50}
	b.ReportAllocs()
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		state.visualTick = uint8(i % 16)
		benchmarkMotionFrame = renderMusic(state, face, small)
	}
}

func BenchmarkCachedMotion(b *testing.B) {
	cache := &motionFrames{}
	state := appState{playing: true, visual: visualBars}
	var base c1device.Frame
	for phase := 0; phase < 16; phase++ {
		state.visualTick = uint8(phase)
		base = cache.apply(base, state)
	}
	b.ReportAllocs()
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		state.visualTick = uint8(i % 16)
		benchmarkMotionFrame = cache.apply(base, state)
	}
}
