//go:build linux && mipsle

package c1device

import (
	"encoding/binary"
	"testing"
	"time"

	"golang.org/x/sys/unix"
)

func TestDeviceCloseUnblocksFullInputQueue(t *testing.T) {
	descriptors := make([]int, 2)
	if err := unix.Pipe2(descriptors, unix.O_NONBLOCK|unix.O_CLOEXEC); err != nil {
		t.Fatal(err)
	}
	defer unix.Close(descriptors[1])
	platform := &devicePlatform{display: -1, inputs: []int{descriptors[0]}, output: make(chan Event, 1), stop: make(chan struct{}), done: make(chan struct{})}
	platform.output <- Event{}
	raw := make([]byte, inputEventSize)
	binary.LittleEndian.PutUint16(raw[8:10], 1)
	binary.LittleEndian.PutUint16(raw[10:12], 103)
	binary.LittleEndian.PutUint32(raw[12:16], 1)
	if _, err := unix.Write(descriptors[1], raw); err != nil {
		t.Fatal(err)
	}
	go platform.readEvents()
	deadline := time.Now().Add(time.Second)
	for {
		poll := []unix.PollFd{{Fd: int32(descriptors[0]), Events: unix.POLLIN}}
		count, err := unix.Poll(poll, 0)
		if err != nil {
			t.Fatal(err)
		}
		if count == 0 {
			break
		}
		if time.Now().After(deadline) {
			t.Fatal("input reader did not consume event")
		}
		time.Sleep(time.Millisecond)
	}
	closed := make(chan struct{})
	go func() { platform.Close(); platform.Close(); close(closed) }()
	select {
	case <-closed:
	case <-time.After(time.Second):
		t.Fatal("Close blocked behind a full event queue")
	}
	select {
	case <-platform.done:
	default:
		t.Fatal("Close returned before input reader stopped")
	}
	for range platform.Events() {
	}
}

func TestDeviceInvalidInputClosesEvents(t *testing.T) {
	descriptors := make([]int, 2)
	if err := unix.Pipe2(descriptors, unix.O_NONBLOCK|unix.O_CLOEXEC); err != nil {
		t.Fatal(err)
	}
	unix.Close(descriptors[0])
	defer unix.Close(descriptors[1])
	platform := &devicePlatform{display: -1, inputs: []int{descriptors[0]}, output: make(chan Event, 1), stop: make(chan struct{}), done: make(chan struct{})}
	go platform.readEvents()
	select {
	case <-platform.done:
	case <-time.After(time.Second):
		t.Fatal("invalid input descriptor caused polling loop instead of shutdown")
	}
	// Descriptor was already closed; do not close it twice if its number is reused.
	platform.inputs = nil
	platform.Close()
}
