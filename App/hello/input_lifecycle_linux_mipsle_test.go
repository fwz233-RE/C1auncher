//go:build linux && mipsle

package main

import (
	"encoding/binary"
	"testing"
	"time"

	"golang.org/x/sys/unix"
)

func TestHelloCloseUnblocksFullInputQueue(t *testing.T) {
	descriptors := make([]int, 2)
	if err := unix.Pipe2(descriptors, unix.O_NONBLOCK|unix.O_CLOEXEC); err != nil {
		t.Fatal(err)
	}
	defer unix.Close(descriptors[1])
	platform := &devicePlatform{display: -1, inputs: []int{descriptors[0]}, output: make(chan keyInput, 1), stop: make(chan struct{}), done: make(chan struct{})}
	platform.output <- keyInput{}
	raw := make([]byte, helloInputEventSize)
	binary.LittleEndian.PutUint16(raw[8:10], 1)
	binary.LittleEndian.PutUint16(raw[10:12], 103)
	binary.LittleEndian.PutUint32(raw[12:16], 1)
	if _, err := unix.Write(descriptors[1], raw); err != nil {
		t.Fatal(err)
	}
	go platform.readInput()
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
	go func() { platform.close(); platform.close(); close(closed) }()
	select {
	case <-closed:
	case <-time.After(time.Second):
		t.Fatal("close blocked behind a full event queue")
	}
	select {
	case <-platform.done:
	default:
		t.Fatal("close returned before input reader stopped")
	}
	for range platform.events() {
	}
}

func TestHelloDisconnectedInputClosesEvents(t *testing.T) {
	descriptors := make([]int, 2)
	if err := unix.Pipe2(descriptors, unix.O_NONBLOCK|unix.O_CLOEXEC); err != nil {
		t.Fatal(err)
	}
	unix.Close(descriptors[1])
	platform := &devicePlatform{display: -1, inputs: []int{descriptors[0]}, output: make(chan keyInput, 1), stop: make(chan struct{}), done: make(chan struct{})}
	go platform.readInput()
	select {
	case <-platform.done:
	case <-time.After(time.Second):
		t.Fatal("input hangup caused polling loop instead of shutdown")
	}
	platform.close()
}
