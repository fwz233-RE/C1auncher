package main

import (
	"time"

	"c1device"
)

func motionActive(state appState) bool {
	return state.playing && !state.paused && (state.visual == visualBars || state.visual == visualRecord)
}

// A one-shot timer is rearmed only after a frame is submitted. Slow display
// writes therefore cannot accumulate animation frames or a catch-up burst.
// Audio progress and key events do not postpone an already pending frame.
type motionClock struct {
	timer *time.Timer
	ticks <-chan time.Time
}

func (clock *motionClock) sync(state appState) {
	if !motionActive(state) {
		clock.stop()
		return
	}
	if clock.ticks != nil {
		return
	}
	if clock.timer == nil {
		clock.timer = time.NewTimer(motionRefreshInterval)
	} else {
		clock.timer.Reset(motionRefreshInterval)
	}
	clock.ticks = clock.timer.C
}

func (clock *motionClock) stop() {
	if clock.timer != nil && !clock.timer.Stop() {
		select {
		case <-clock.timer.C:
		default:
		}
	}
	clock.ticks = nil
}

// Cache each decorative cover as packed monochrome pixels. Animation-only
// frames reuse the text and layout already rendered, avoiding font rasterization
// and full-canvas conversion on every tick. Device writes are still full frames.
type motionFrames struct {
	frames [2][16]c1device.Frame
	ready  [2][16]bool
}

func (cache *motionFrames) apply(base c1device.Frame, state appState) c1device.Frame {
	if !motionActive(state) {
		return base
	}
	mode, phase := int(state.visual-visualBars), visualPhase(state)
	if !cache.ready[mode][phase] {
		canvas := c1device.NewCanvas()
		if state.visual == visualBars {
			canvas.DrawImage(barsArtwork(phase, true), musicCoverRect)
		} else {
			canvas.DrawImage(recordArtwork(phase, true), musicCoverRect)
		}
		cache.frames[mode][phase] = canvas.Frame(128)
		cache.ready[mode][phase] = true
	}
	patch := &cache.frames[mode][phase]
	for page := musicCoverRect.Min.Y / 8; page <= (musicCoverRect.Max.Y-1)/8; page++ {
		var mask byte
		for bit := 0; bit < 8; bit++ {
			y := page*8 + bit
			if y >= musicCoverRect.Min.Y && y < musicCoverRect.Max.Y {
				mask |= 0x80 >> bit
			}
		}
		for x := musicCoverRect.Min.X; x < musicCoverRect.Max.X; x++ {
			offset := page*c1device.DisplayWidth + x
			base[offset] = base[offset]&^mask | patch[offset]&mask
		}
	}
	return base
}
