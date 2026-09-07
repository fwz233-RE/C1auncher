package main

import (
	"errors"
	"reflect"
	"testing"
	"time"
)

func TestPowerSharedGPIOInput(t *testing.T) {
	var opened []string
	var grabbed, closed []int
	inputs, err := openMusicInputs(func(path string) (int, error) {
		opened = append(opened, path)
		return 40 + len(opened), nil
	}, func(fd int) error {
		grabbed = append(grabbed, fd)
		return nil
	}, func(fd int) { closed = append(closed, fd) })
	if err != nil || !reflect.DeepEqual(inputs, []int{41, 42}) {
		t.Fatalf("inputs=%v err=%v", inputs, err)
	}
	if !reflect.DeepEqual(opened, []string{"/dev/input/event0", "/dev/input/event1"}) ||
		!reflect.DeepEqual(grabbed, []int{41}) || len(closed) != 0 {
		t.Fatalf("GPIO must remain shared: opened=%v grabbed=%v closed=%v", opened, grabbed, closed)
	}
}

func TestPowerInputOpenFailures(t *testing.T) {
	for _, missing := range []string{"/dev/input/event0", "/dev/input/event1", "both"} {
		t.Run(missing, func(t *testing.T) {
			grabs := 0
			inputs, err := openMusicInputs(func(path string) (int, error) {
				if missing == path || missing == "both" {
					return -1, errors.New("not available")
				}
				return 42, nil
			}, func(int) error { grabs++; return nil }, func(int) { t.Fatal("unexpected close") })
			if missing == "both" {
				if err == nil || len(inputs) != 0 || grabs != 0 {
					t.Fatal("missing inputs accepted")
				}
				return
			}
			if err != nil || len(inputs) != 1 {
				t.Fatalf("%v %v", inputs, err)
			}
			wantGrabs := 0
			if missing == "/dev/input/event1" {
				wantGrabs = 1
			}
			if grabs != wantGrabs {
				t.Fatalf("grabs=%d want=%d", grabs, wantGrabs)
			}
		})
	}
	want := errors.New("grab failed")
	var closed []int
	inputs, err := openMusicInputs(func(string) (int, error) { return 51, nil },
		func(int) error { return want }, func(fd int) { closed = append(closed, fd) })
	if !errors.Is(err, want) || inputs != nil || !reflect.DeepEqual(closed, []int{51}) {
		t.Fatalf("failed grab leaked input: %v %v %v", inputs, err, closed)
	}
}

func TestPowerKeysAreReservedInEveryAppMode(t *testing.T) {
	now := time.Unix(100, 0)
	setups := map[string]func(*model){
		"idle":   func(*model) {},
		"help":   func(m *model) { m.Help = true },
		"loop":   func(m *model) { m.start(now) },
		"record": func(m *model) { m.handle(keyEvent{Code: 19, Down: true}, now) },
		"step-record": func(m *model) {
			m.handle(keyEvent{Code: 19, Down: true}, now)
			m.handle(keyEvent{Code: 106, Down: true}, now)
		},
	}
	for name, setup := range setups {
		t.Run(name, func(t *testing.T) {
			for _, exporting := range []bool{false, true} {
				m, before := newModel(defaultSong()), newModel(defaultSong())
				setup(m)
				setup(before)
				for _, code := range []uint16{143, 116} {
					// Repeated down events and release after the core's hold deadline
					// must not exit, cancel export, stop recording or modify the song.
					for _, down := range []bool{true, true, true, false} {
						e := keyEvent{Code: code, Down: down}
						if exitKey(e) {
							t.Fatalf("power key %d is still an exit key", code)
						}
						commands, action := handleInput(m, e, now.Add(4*time.Second), exporting)
						if len(commands) != 0 || action != "" || !reflect.DeepEqual(m, before) {
							t.Fatalf("power key changed %s export=%v: action=%q commands=%v", name, exporting, action, commands)
						}
						commands, action = m.handle(e, now.Add(4*time.Second))
						if len(commands) != 0 || action != "" || !reflect.DeepEqual(m, before) {
							t.Fatal("direct model dispatch handled a reserved system key")
						}
					}
				}
			}
		})
	}
}
