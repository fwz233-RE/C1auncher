package c1device

import "testing"

func TestTopRowAndNumericEditingHardwareCodes(t *testing.T) {
	for index, letter := range "qwertyuio" {
		event, ok := mapKey(uint16(16 + index))
		if !ok || event.Key != KeyRune || event.Rune != letter {
			t.Fatalf("row code %d: %+v", 16+index, event)
		}
	}
	for index, digit := range "1234567890" {
		event, ok := mapKey(uint16(2 + index))
		if !ok || event.Key != KeyRune || event.Rune != digit {
			t.Fatalf("digit code %d: %+v", 2+index, event)
		}
	}
	for _, tc := range []struct {
		code uint16
		want rune
	}{{44, 'z'}, {52, '.'}, {14, '\b'}, {111, '\b'}} {
		event, ok := mapKey(tc.code)
		if !ok || event.Key != KeyRune || event.Rune != tc.want {
			t.Fatalf("editing code %d: %+v", tc.code, event)
		}
	}
	event, ok := mapKey(25)
	if !ok || event.Key != KeyPause {
		t.Fatal("P mapping changed globally")
	}
}

func TestHardwareOIsDistinctFromConfirmAndBack(t *testing.T) {
	for _, tc := range []struct {
		code uint16
		want Event
	}{
		{24, Event{Key: KeyRune, Rune: 'o'}}, {28, Event{Key: KeyOK}},
		{352, Event{Key: KeyOK}}, {102, Event{Key: KeyBack}},
		{25, Event{Key: KeyPause}}, {47, Event{Key: KeyRune, Rune: 'v'}},
	} {
		got, ok := mapKey(tc.code)
		if !ok || got != tc.want {
			t.Fatalf("keycode %d: got %+v, want %+v", tc.code, got, tc.want)
		}
	}
}
