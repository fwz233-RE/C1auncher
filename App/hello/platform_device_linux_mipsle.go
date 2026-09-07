//go:build linux && mipsle

package main

import (
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"sync"

	"golang.org/x/sys/unix"
)

const (
	helloEpaperDevice   = "/dev/epaper_lcd"
	helloEpaperRefresh  = "/sys/devices/platform/e0266a128/epaper/refresh"
	helloEpaperFastOnly = "/sys/devices/platform/e0266a128/epaper/fast_refresh_only"
	helloInputEventSize = 16
)

type devicePlatform struct {
	display int
	inputs  []int
	output  chan keyInput
	last    frame
	init    bool
	closing sync.Once
	stop    chan struct{}
	done    chan struct{}
}

func openPlatform() (*devicePlatform, error) {
	display, err := unix.Open(helloEpaperDevice, unix.O_WRONLY|unix.O_NONBLOCK|unix.O_CLOEXEC, 0)
	if err != nil {
		return nil, err
	}
	platform := &devicePlatform{
		display: display,
		inputs:  openHelloInputs(),
		output:  make(chan keyInput, 16),
		stop:    make(chan struct{}),
		done:    make(chan struct{}),
	}
	if len(platform.inputs) == 0 {
		_ = unix.Close(display)
		return nil, errors.New("no input devices available")
	}
	go platform.readInput()
	return platform, nil
}

func (platform *devicePlatform) draw(next frame, full bool) error {
	if platform.init && next == platform.last {
		return nil
	}
	full = full || !platform.init
	if full {
		if err := writeHelloControl(helloEpaperFastOnly, "0"); err != nil {
			return err
		}
	}
	count, err := unix.Write(platform.display, next[:])
	if err != nil {
		return err
	}
	if count != len(next) {
		return io.ErrShortWrite
	}
	if full {
		refreshErr := writeHelloControl(helloEpaperRefresh, "1")
		fastErr := writeHelloControl(helloEpaperFastOnly, "1")
		if err := errors.Join(refreshErr, fastErr); err != nil {
			return fmt.Errorf("full refresh: %w", err)
		}
	}
	platform.last = next
	platform.init = true
	return nil
}

func (platform *devicePlatform) events() <-chan keyInput {
	return platform.output
}

func (platform *devicePlatform) close() {
	platform.closing.Do(func() {
		close(platform.stop)
		<-platform.done
		for _, descriptor := range platform.inputs {
			_ = unix.Close(descriptor)
		}
		platform.inputs = nil
		if platform.display >= 0 {
			_ = unix.Close(platform.display)
			platform.display = -1
		}
	})
}

func (platform *devicePlatform) readInput() {
	defer close(platform.done)
	defer close(platform.output)
	pollDescriptors := make([]unix.PollFd, len(platform.inputs))
	for index, descriptor := range platform.inputs {
		pollDescriptors[index] = unix.PollFd{Fd: int32(descriptor), Events: unix.POLLIN}
	}
	buffer := make([]byte, helloInputEventSize*16)
	for {
		select {
		case <-platform.stop:
			return
		default:
		}
		count, err := unix.Poll(pollDescriptors, 250)
		if err == unix.EINTR {
			continue
		}
		if err != nil {
			return
		}
		if count == 0 {
			continue
		}
		for index := range pollDescriptors {
			if pollDescriptors[index].Revents&(unix.POLLERR|unix.POLLHUP|unix.POLLNVAL) != 0 {
				return
			}
			if pollDescriptors[index].Revents&unix.POLLIN == 0 {
				continue
			}
			read, readErr := unix.Read(platform.inputs[index], buffer)
			if readErr == unix.EAGAIN || readErr == unix.EINTR {
				continue
			}
			if readErr != nil || read == 0 {
				return
			}
			for offset := 0; offset+helloInputEventSize <= read; offset += helloInputEventSize {
				if binary.LittleEndian.Uint16(buffer[offset+8:offset+10]) != 1 {
					continue
				}
				value := int32(binary.LittleEndian.Uint32(buffer[offset+12 : offset+16]))
				if value != 1 {
					continue
				}
				code := binary.LittleEndian.Uint16(buffer[offset+10 : offset+12])
				if event, ok := helloKey(code); ok {
					select {
					case platform.output <- event:
					case <-platform.stop:
						return
					}
				}
			}
		}
	}
}

func openHelloInputs() []int {
	paths := [...]string{"/dev/input/event0", "/dev/input/event1"}
	descriptors := make([]int, 0, len(paths))
	for _, path := range paths {
		descriptor, err := unix.Open(path, unix.O_RDONLY|unix.O_NONBLOCK|unix.O_CLOEXEC, 0)
		if err == nil {
			descriptors = append(descriptors, descriptor)
		}
	}
	return descriptors
}

func helloKey(code uint16) (keyInput, bool) {
	names := map[uint16]string{
		14: "BACKSPACE", 28: "ENTER", 57: "SPACE", 103: "UP",
		105: "LEFT", 106: "RIGHT", 108: "DOWN", 111: "DELETE",
		143: "WAKE", 352: "OK",
	}
	if code == 102 || code == 158 {
		return keyInput{name: "HOME", exit: true}, true
	}
	if name, ok := names[code]; ok {
		return keyInput{name: name}, true
	}
	if code >= 2 && code <= 11 {
		return keyInput{name: "NUMBER"}, true
	}
	if (code >= 16 && code <= 25) || (code >= 30 && code <= 38) || (code >= 44 && code <= 50) {
		return keyInput{name: "LETTER"}, true
	}
	return keyInput{}, false
}

func writeHelloControl(path string, value string) error {
	descriptor, err := unix.Open(path, unix.O_WRONLY|unix.O_CLOEXEC, 0)
	if err != nil {
		return err
	}
	count, writeErr := unix.Write(descriptor, []byte(value))
	closeErr := unix.Close(descriptor)
	if writeErr != nil {
		return writeErr
	}
	if count != len(value) {
		return io.ErrShortWrite
	}
	return closeErr
}
