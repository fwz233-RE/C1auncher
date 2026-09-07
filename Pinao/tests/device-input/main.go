// A device-only integration fixture. Never included in the published binary.
package main

import (
	"encoding/binary"
	"fmt"
	"os"
	"strconv"
	"strings"
	"time"
)

func main() {
	if len(os.Args) != 2 {
		panic("expected Pinao test process PID")
	}
	pid, e := strconv.Atoi(os.Args[1])
	if e != nil || pid < 2 {
		panic("invalid PID")
	}
	data, e := os.ReadFile(fmt.Sprintf("/proc/%d/cmdline", pid))
	if e != nil || !strings.Contains(string(data), "pinao-test") || !strings.Contains(string(data), "/usr/data/pinao-validation-") {
		panic("not the isolated Pinao test process")
	}
	keyboard, e := os.OpenFile("/dev/input/event0", os.O_WRONLY, 0)
	if e != nil {
		panic(e)
	}
	defer keyboard.Close()
	buttons, e := os.OpenFile("/dev/input/event1", os.O_WRONLY, 0)
	if e != nil {
		panic(e)
	}
	defer buttons.Close()
	send := func(f *os.File, code uint16, value uint32) {
		b := make([]byte, 32)
		binary.LittleEndian.PutUint16(b[8:], 1)
		binary.LittleEndian.PutUint16(b[10:], code)
		binary.LittleEndian.PutUint32(b[12:], value)
		if n, e := f.Write(b); e != nil || n != len(b) {
			panic("input write failed")
		}
	}
	press := func(f *os.File, code uint16) {
		send(f, code, 1)
		time.Sleep(45 * time.Millisecond)
		send(f, code, 0)
		time.Sleep(55 * time.Millisecond)
	}
	press(keyboard, 19) // R record.
	send(keyboard, 30, 1)
	send(keyboard, 32, 1)
	send(keyboard, 34, 1)
	time.Sleep(220 * time.Millisecond)
	send(keyboard, 30, 0)
	send(keyboard, 32, 0)
	send(keyboard, 34, 0)
	press(keyboard, 46)
	press(keyboard, 47)
	press(keyboard, 48)
	press(keyboard, 49) // Four drums.
	press(keyboard, 16)
	press(keyboard, 36) // Q bell and J note.
	press(keyboard, 19) // Stop recording, preserve looping.
	press(buttons, 28)  // Save.
	time.Sleep(300 * time.Millisecond)
	press(buttons, 25) // Export.
	time.Sleep(4 * time.Second)
	press(keyboard, 102) // HOME exits and releases display/audio/keyboard.
	fmt.Println("PINAO_KEYS_SENT")
}
