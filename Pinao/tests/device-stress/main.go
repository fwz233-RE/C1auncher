// Bounded evdev stress fixture; never included in published application.
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
	if len(os.Args) < 2 {
		panic("PID required")
	}
	pid, e := strconv.Atoi(os.Args[1])
	if e != nil || pid < 2 {
		panic("bad PID")
	}
	data, e := os.ReadFile(fmt.Sprintf("/proc/%d/cmdline", pid))
	if e != nil || !strings.Contains(string(data), "pinao") || !strings.Contains(string(data), "/usr/data/pinao-validation-") {
		panic("not an isolated Pinao test")
	}
	f, e := os.OpenFile("/dev/input/event0", os.O_WRONLY, 0)
	if e != nil {
		panic(e)
	}
	defer f.Close()
	b, e := os.OpenFile("/dev/input/event1", os.O_WRONLY, 0)
	if e != nil {
		panic(e)
	}
	defer b.Close()
	send := func(f *os.File, c uint16, v uint32) {
		var data [32]byte
		binary.LittleEndian.PutUint16(data[8:], 1)
		binary.LittleEndian.PutUint16(data[10:], c)
		binary.LittleEndian.PutUint32(data[12:], v)
		if n, e := f.Write(data[:]); e != nil || n != 32 {
			panic("evdev write")
		}
	}
	tap := func(f *os.File, c uint16) { send(f, c, 1); time.Sleep(4 * time.Millisecond); send(f, c, 0) }
	for i := 0; i < 5; i++ {
		tap(b, 108)
	} // 20% software volume.
	tap(f, 19)
	until := time.Now().Add(60 * time.Second)
	round := 0
	for time.Now().Before(until) {
		if _, e := os.Stat(fmt.Sprintf("/proc/%d", pid)); e != nil {
			panic("app exited during stress")
		}
		keys := []uint16{30, 17, 31, 18, 32, 33, 34, 35}
		for _, k := range keys {
			send(f, k, 1)
		}
		time.Sleep(28 * time.Millisecond)
		for _, k := range keys {
			send(f, k, 0)
		}
		for _, k := range []uint16{46, 47, 48, 49} {
			tap(f, k)
		}
		if round%10 == 0 {
			tap(f, 16)
		}
		if round%20 == 0 {
			tap(f, 38)
			time.Sleep(25 * time.Millisecond)
			tap(f, 38)
		}
		time.Sleep(45 * time.Millisecond)
		round++
	}
	tap(f, 19)
	tap(b, 28)
	time.Sleep(100 * time.Millisecond)
	tap(b, 25)
	if len(os.Args) > 2 && os.Args[2] == "cancel-export" {
		time.Sleep(30 * time.Millisecond)
		tap(f, 102)
		fmt.Println("HOME_DURING_EXPORT_SENT")
	} else {
		time.Sleep(4 * time.Second)
		tap(f, 102)
	}
	fmt.Printf("STRESS_ROUNDS=%d\n", round)
}
