//go:build linux && mipsle

package c1device

import "testing"

func TestVisualKeyMappingKeepsExistingControls(t *testing.T) {
	for _, test := range []struct {
		code  uint16
		event Event
	}{
		{47, Event{Key: KeyRune, Rune: 'v'}},
		{25, Event{Key: KeyPause}},
		{103, Event{Key: KeyUp}},
		{108, Event{Key: KeyDown}},
		{105, Event{Key: KeyLeft}},
		{106, Event{Key: KeyRight}},
		{28, Event{Key: KeyOK}},
		{352, Event{Key: KeyOK}},
		{114, Event{Key: KeyVolumeDown}},
		{115, Event{Key: KeyVolumeUp}},
	} {
		if got, ok := mapKey(test.code); !ok || got != test.event {
			t.Errorf("mapKey(%d)=%+v,%v; want %+v", test.code, got, ok, test.event)
		}
	}
}
