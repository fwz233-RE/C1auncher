package c1device

// Linux evdev key codes. Kept platform-independent so the actual hardware map
// can be covered by host tests as well as MIPS device tests.
func mapKey(code uint16) (Event, bool) {
	switch code {
	case 103:
		return Event{Key: KeyUp}, true
	case 108:
		return Event{Key: KeyDown}, true
	case 105:
		return Event{Key: KeyLeft}, true
	case 106:
		return Event{Key: KeyRight}, true
	case 28, 352:
		return Event{Key: KeyOK}, true
	case 102, 143, 158:
		return Event{Key: KeyBack}, true
	case 25:
		return Event{Key: KeyPause}, true
	case 16, 17, 18, 19, 20, 21, 22, 23, 24:
		// Preserve letters here. Numeric forms printed on Q-P are interpreted
		// by the reader's numeric dialog, not globally by every application.
		return Event{Key: KeyRune, Rune: rune("qwertyuio"[code-16])}, true
	case 44:
		return Event{Key: KeyRune, Rune: 'z'}, true
	case 52:
		return Event{Key: KeyRune, Rune: '.'}, true
	case 14, 111:
		return Event{Key: KeyRune, Rune: '\b'}, true
	case 47:
		return Event{Key: KeyRune, Rune: 'v'}, true
	case 114:
		return Event{Key: KeyVolumeDown}, true
	case 115:
		return Event{Key: KeyVolumeUp}, true
	}
	if code >= 2 && code <= 11 {
		return Event{Key: KeyRune, Rune: rune("1234567890"[code-2])}, true
	}
	return Event{}, false
}
