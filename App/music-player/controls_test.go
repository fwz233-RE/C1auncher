package main

import (
	"testing"

	"c1device"
)

func TestFullRefreshPolicyProtectsPlayingAudio(t *testing.T) {
	tests := []struct {
		name                                                  string
		initial, wasPlaying, wasPaused, playing, paused, want bool
	}{
		{name: "initial screen", initial: true, playing: true, want: true},
		{name: "playing progress uses fast refresh", wasPlaying: true, playing: true},
		{name: "entering pause clears ghosting", wasPlaying: true, playing: true, paused: true, want: true},
		{name: "stopping clears ghosting", wasPlaying: true, want: true},
		{name: "idle volume avoids full refresh"},
		{name: "paused mode change avoids full refresh", wasPlaying: true, wasPaused: true, playing: true, paused: true},
		{name: "resume avoids full refresh", wasPlaying: true, wasPaused: true, playing: true},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			if got := shouldFullRefresh(test.initial, test.wasPlaying, test.wasPaused, test.playing, test.paused); got != test.want {
				t.Fatalf("shouldFullRefresh() = %v, want %v", got, test.want)
			}
		})
	}
}

func TestMusicActionAssignments(t *testing.T) {
	tests := []struct {
		name  string
		event c1device.Event
		want  musicAction
	}{
		{name: "up selects previous", event: c1device.Event{Key: c1device.KeyUp}, want: actionSelectPrevious},
		{name: "down selects next", event: c1device.Event{Key: c1device.KeyDown}, want: actionSelectNext},
		{name: "ok toggles playback", event: c1device.Event{Key: c1device.KeyOK}, want: actionPlayPause},
		{name: "left plays previous", event: c1device.Event{Key: c1device.KeyLeft}, want: actionPreviousTrack},
		{name: "right plays next", event: c1device.Event{Key: c1device.KeyRight}, want: actionNextTrack},
		{name: "device p toggles order", event: c1device.Event{Key: c1device.KeyPause}, want: actionToggleShuffle},
		{name: "host p toggles order", event: c1device.Event{Key: c1device.KeyRune, Rune: 'P'}, want: actionToggleShuffle},
		{name: "volume down changes volume", event: c1device.Event{Key: c1device.KeyVolumeDown}, want: actionVolumeDown},
		{name: "volume up changes volume", event: c1device.Event{Key: c1device.KeyVolumeUp}, want: actionVolumeUp},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			if got := musicActionForEvent(test.event); got != test.want {
				t.Fatalf("action = %d, want %d", got, test.want)
			}
		})
	}
}

func TestMusicActionIgnoresRepeatForToggleKeys(t *testing.T) {
	for _, key := range []c1device.Key{c1device.KeyOK, c1device.KeyPause} {
		if got := musicActionForEvent(c1device.Event{Key: key, Repeat: true}); got != actionNone {
			t.Fatalf("repeat for key %d = %d, want actionNone", key, got)
		}
	}
	for _, character := range []rune{'p', 'P'} {
		if got := musicActionForEvent(c1device.Event{Key: c1device.KeyRune, Rune: character, Repeat: true}); got != actionNone {
			t.Fatalf("repeat for rune %q = %d, want actionNone", character, got)
		}
	}
	if got := musicActionForEvent(c1device.Event{Key: c1device.KeyVolumeUp, Repeat: true}); got != actionVolumeUp {
		t.Fatalf("volume repeat = %d, want actionVolumeUp", got)
	}
}

func TestMoveSelectionWrapsAtBothEnds(t *testing.T) {
	tests := []struct {
		current int
		count   int
		delta   int
		want    int
	}{
		{current: 0, count: 3, delta: -1, want: 2},
		{current: 2, count: 3, delta: 1, want: 0},
		{current: 1, count: 3, delta: -1, want: 0},
		{current: 1, count: 3, delta: 1, want: 2},
		{current: -1, count: 3, delta: 1, want: 1},
		{current: 0, count: 0, delta: 1, want: -1},
	}
	for _, test := range tests {
		if got := moveSelection(test.current, test.count, test.delta); got != test.want {
			t.Errorf("moveSelection(%d, %d, %d) = %d, want %d", test.current, test.count, test.delta, got, test.want)
		}
	}
}
