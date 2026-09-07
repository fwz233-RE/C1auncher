//go:build linux && mipsle

package c1device

import (
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"sync"

	"golang.org/x/sys/unix"
)

const (
	epaperDevice   = "/dev/epaper_lcd"
	epaperRefresh  = "/sys/devices/platform/e0266a128/epaper/refresh"
	epaperFastOnly = "/sys/devices/platform/e0266a128/epaper/fast_refresh_only"
	inputEventSize = 16
)

type devicePlatform struct {
	display int
	inputs  []int
	output  chan Event
	last    Frame
	ready   bool
	close   sync.Once
	stop    chan struct{}
	done    chan struct{}
}

func OpenPlatform() (Platform, error) {
	display, err := unix.Open(epaperDevice, unix.O_WRONLY|unix.O_NONBLOCK|unix.O_CLOEXEC, 0)
	if err != nil {
		return nil, fmt.Errorf("open display: %w", err)
	}
	platform := &devicePlatform{
		display: display, inputs: openInputs(), output: make(chan Event, 32),
		stop: make(chan struct{}), done: make(chan struct{}),
	}
	if len(platform.inputs) == 0 {
		_ = unix.Close(display)
		return nil, errors.New("no input devices available")
	}
	go platform.readEvents()
	return platform, nil
}

func (platform *devicePlatform) Draw(next Frame, full bool) error {
	if platform.ready && next == platform.last {
		return nil
	}
	full = full || !platform.ready
	if full {
		if err := writeControl(epaperFastOnly, "0"); err != nil {
			return err
		}
	}
	count, err := unix.Write(platform.display, next[:])
	if err != nil {
		return fmt.Errorf("write display: %w", err)
	}
	if count != len(next) {
		return io.ErrShortWrite
	}
	if full {
		if err := errors.Join(writeControl(epaperRefresh, "1"), writeControl(epaperFastOnly, "1")); err != nil {
			return fmt.Errorf("full refresh: %w", err)
		}
	}
	platform.last, platform.ready = next, true
	return nil
}

func (platform *devicePlatform) Events() <-chan Event { return platform.output }

func (platform *devicePlatform) Close() error {
	platform.close.Do(func() {
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
	return nil
}

func (platform *devicePlatform) readEvents() {
	defer close(platform.done)
	defer close(platform.output)
	poll := make([]unix.PollFd, len(platform.inputs))
	for index, descriptor := range platform.inputs {
		poll[index] = unix.PollFd{Fd: int32(descriptor), Events: unix.POLLIN}
	}
	buffer := make([]byte, inputEventSize*16)
	for {
		select {
		case <-platform.stop:
			return
		default:
		}
		count, err := unix.Poll(poll, 250)
		if err == unix.EINTR {
			continue
		}
		if err != nil {
			return
		}
		if count == 0 {
			continue
		}
		for index := range poll {
			if poll[index].Revents&(unix.POLLERR|unix.POLLHUP|unix.POLLNVAL) != 0 {
				return
			}
			if poll[index].Revents&unix.POLLIN == 0 {
				continue
			}
			read, readErr := unix.Read(int(poll[index].Fd), buffer)
			if readErr == unix.EAGAIN || readErr == unix.EINTR {
				continue
			}
			if readErr != nil || read == 0 {
				return
			}
			for offset := 0; offset+inputEventSize <= read; offset += inputEventSize {
				if binary.LittleEndian.Uint16(buffer[offset+8:offset+10]) != 1 {
					continue
				}
				value := int32(binary.LittleEndian.Uint32(buffer[offset+12 : offset+16]))
				if value != 1 && value != 2 {
					continue
				}
				code := binary.LittleEndian.Uint16(buffer[offset+10 : offset+12])
				if event, ok := mapKey(code); ok {
					event.Repeat = value == 2
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

func openInputs() []int {
	paths := [...]string{"/dev/input/event0", "/dev/input/event1"}
	result := make([]int, 0, len(paths))
	for _, path := range paths {
		descriptor, err := unix.Open(path, unix.O_RDONLY|unix.O_NONBLOCK|unix.O_CLOEXEC, 0)
		if err == nil {
			result = append(result, descriptor)
		}
	}
	return result
}

func writeControl(path, value string) error {
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
