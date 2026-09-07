package main

import "c1device"

type musicAction uint8

const (
	actionNone musicAction = iota
	actionSelectPrevious
	actionSelectNext
	actionPlayPause
	actionPreviousTrack
	actionNextTrack
	actionToggleShuffle
	actionCycleVisual
	actionVolumeDown
	actionVolumeUp
)

func musicActionForEvent(event c1device.Event) musicAction {
	if event.Repeat && isPlaybackToggleKey(event) {
		return actionNone
	}
	switch event.Key {
	case c1device.KeyUp:
		return actionSelectPrevious
	case c1device.KeyDown:
		return actionSelectNext
	case c1device.KeyOK:
		return actionPlayPause
	case c1device.KeyLeft:
		return actionPreviousTrack
	case c1device.KeyRight:
		return actionNextTrack
	case c1device.KeyPause:
		return actionToggleShuffle
	case c1device.KeyRune:
		if event.Rune == 'p' || event.Rune == 'P' {
			return actionToggleShuffle
		}
		if event.Rune == 'v' || event.Rune == 'V' {
			return actionCycleVisual
		}
	case c1device.KeyVolumeDown:
		return actionVolumeDown
	case c1device.KeyVolumeUp:
		return actionVolumeUp
	}
	return actionNone
}

func isPlaybackToggleKey(event c1device.Event) bool {
	if event.Key == c1device.KeyOK || event.Key == c1device.KeyPause {
		return true
	}
	return event.Key == c1device.KeyRune && (event.Rune == 'p' || event.Rune == 'P' || event.Rune == 'v' || event.Rune == 'V')
}

func moveSelection(current, count, delta int) int {
	if count <= 0 {
		return -1
	}
	if current < 0 || current >= count {
		current = 0
	}
	if delta < 0 && current == 0 {
		return count - 1
	}
	if delta > 0 && current == count-1 {
		return 0
	}
	return current + delta
}
