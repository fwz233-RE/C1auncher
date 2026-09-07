//go:build linux && mipsle

package main

import (
	"encoding/binary"
	"errors"
	"fmt"
	"golang.org/x/sys/unix"
	"io"
	"os"
	"strconv"
	"strings"
	"sync"
	"time"
)

// MIPS _IOW('E', 0x90, int): its ioctl write direction differs from x86.
const evIOCGrab = 0x80044590
const epaperDevice = "/dev/epaper_lcd"
const epaperControl = "/sys/devices/platform/e0266a128/epaper/"
const leasePath = "/dev/shm/c1ancher-external-app.lock"

type devicePlatform struct {
	display    int
	inputs     []int
	lease      int
	output     chan keyEvent
	stop, done chan struct{}
	once       sync.Once
	last       frame
	ready      bool
}

func deviceDataDirectory() string { return "/storage/c1/pinao" }
func acquireLease() (int, error) {
	var expected unix.Stat_t
	if err := unix.Lstat(leasePath, &expected); err == nil {
		if expected.Mode&unix.S_IFMT != unix.S_IFREG || expected.Uid != uint32(os.Geteuid()) || expected.Mode&0777 != 0600 || expected.Nlink != 1 {
			return -1, errors.New("invalid display lease")
		}
		// c1pkg intentionally passes the exclusive lease across exec. Reuse only
		// an inherited descriptor identifying the same regular file.
		entries, _ := os.ReadDir("/proc/self/fd")
		for _, entry := range entries {
			fd, err := strconv.Atoi(entry.Name())
			if err != nil || fd < 3 {
				continue
			}
			var st unix.Stat_t
			if unix.Fstat(fd, &st) == nil && st.Ino == expected.Ino && st.Dev == expected.Dev {
				if err = unix.Flock(fd, unix.LOCK_EX|unix.LOCK_NB); err != nil {
					return -1, err
				}
				unix.CloseOnExec(fd)
				return fd, nil
			}
		}
	} else if !errors.Is(err, unix.ENOENT) {
		return -1, err
	}
	fd, err := unix.Open(leasePath, unix.O_CREAT|unix.O_RDWR|unix.O_NOFOLLOW|unix.O_CLOEXEC, 0600)
	if err != nil {
		return -1, err
	}
	if err = unix.Flock(fd, unix.LOCK_EX|unix.LOCK_NB); err != nil {
		unix.Close(fd)
		return -1, fmt.Errorf("another app owns the display: %w", err)
	}
	return fd, nil
}
func openPlatform() (*devicePlatform, error) {
	lease, err := acquireLease()
	if err != nil {
		return nil, err
	}
	p := &devicePlatform{display: -1, lease: lease, output: make(chan keyEvent, 128), stop: make(chan struct{}), done: make(chan struct{})}
	fail := func(e error) (*devicePlatform, error) {
		for _, fd := range p.inputs {
			unix.Close(fd)
		}
		if p.display >= 0 {
			unix.Close(p.display)
		}
		unix.Close(lease)
		return nil, e
	}
	// Publish direct mode before sharing gpio_keys so the core suppresses normal
	// app navigation while retaining its system power-key handling.
	if err = os.WriteFile("/dev/shm/c1ancher-external-app.mode", []byte("direct"), 0600); err != nil {
		return fail(err)
	}
	p.inputs, err = openMusicInputs(
		func(path string) (int, error) {
			return unix.Open(path, unix.O_RDONLY|unix.O_NONBLOCK|unix.O_CLOEXEC, 0)
		},
		func(fd int) error { return unix.IoctlSetInt(fd, evIOCGrab, 1) },
		func(fd int) { _ = unix.Close(fd) },
	)
	if err != nil {
		return fail(err)
	}
	p.display, err = unix.Open(epaperDevice, unix.O_WRONLY|unix.O_NONBLOCK|unix.O_CLOEXEC, 0)
	if err != nil {
		return fail(err)
	}
	go p.readInput()
	// Allow the launcher to observe the lease before our first full frame.
	time.Sleep(150 * time.Millisecond)
	return p, nil
}
func writeControl(name, value string) error {
	return os.WriteFile(epaperControl+name, []byte(value), 0600)
}
func (p *devicePlatform) draw(f frame, full bool) error {
	if p.ready && f == p.last && !full {
		return nil
	}
	full = full || !p.ready
	if full {
		if err := writeControl("fast_refresh_only", "0"); err != nil {
			return err
		}
	}
	n, err := unix.Write(p.display, f[:])
	if err == nil && n != len(f) {
		err = io.ErrShortWrite
	}
	if full {
		var refreshErr error
		if err == nil {
			refreshErr = writeControl("refresh", "1")
		}
		err = errors.Join(err, refreshErr, writeControl("fast_refresh_only", "1"))
	}
	if err == nil {
		p.last = f
		p.ready = true
	}
	return err
}
func (p *devicePlatform) close() {
	p.once.Do(func() {
		close(p.stop)
		<-p.done
		for _, fd := range p.inputs {
			// Closing releases any grab owned by this descriptor. Never issue an
			// ungrab against shared gpio_keys, which we deliberately did not grab.
			unix.Close(fd)
		}
		unix.Close(p.display)
		unix.Close(p.lease)
	})
}
func (p *devicePlatform) readInput() {
	defer close(p.done)
	defer close(p.output)
	polls := make([]unix.PollFd, len(p.inputs))
	for i, fd := range p.inputs {
		polls[i] = unix.PollFd{Fd: int32(fd), Events: unix.POLLIN}
	}
	var buf [16 * 32]byte
	down := make(map[uint16]bool)
	dropped := make([]bool, len(p.inputs))
	emit := func(e keyEvent) bool {
		select {
		case p.output <- e:
			return true
		case <-p.stop:
			return false
		}
	}
	for {
		select {
		case <-p.stop:
			return
		default:
		}
		n, e := unix.Poll(polls, 50)
		if e == unix.EINTR {
			continue
		}
		if e != nil {
			return
		}
		if n == 0 {
			continue
		}
		for i, poll := range polls {
			if poll.Revents&(unix.POLLERR|unix.POLLHUP|unix.POLLNVAL) != 0 {
				return
			}
			if poll.Revents&unix.POLLIN == 0 {
				continue
			}
			n, e = unix.Read(p.inputs[i], buf[:])
			if e == unix.EINTR || e == unix.EAGAIN {
				continue
			}
			if e != nil || n == 0 || n%16 != 0 {
				return
			}
			for off := 0; off < n; off += 16 {
				typ := binary.LittleEndian.Uint16(buf[off+8:])
				code := binary.LittleEndian.Uint16(buf[off+10:])
				value := int32(binary.LittleEndian.Uint32(buf[off+12:]))
				if typ == 0 && code == 3 {
					dropped[i] = true
					clear(down)
					if !emit(keyEvent{Reset: true}) {
						return
					}
					continue
				}
				if dropped[i] {
					if typ == 0 && code == 0 {
						dropped[i] = false
					}
					continue
				}
				if typ != 1 || (value != 0 && value != 1) {
					continue
				}
				if value == 1 {
					if down[code] {
						continue
					}
					down[code] = true
				} else {
					delete(down, code)
				}
				if !emit(keyEvent{Code: code, Down: value == 1}) {
					return
				}
			}
		}
	}
}
func requestDesktop() {
	if os.Getenv("C1_C1ANCHER_TERMINAL") != "1" {
		return
	}
	// Match the existing app convention, but never signal an arbitrary parent.
	parent := os.Getppid()
	data, err := os.ReadFile(fmt.Sprintf("/proc/%d/comm", parent))
	if err == nil && (strings.TrimSpace(string(data)) == "bash" || strings.TrimSpace(string(data)) == "sh") {
		_ = unix.Kill(parent, unix.SIGKILL)
	}
}
