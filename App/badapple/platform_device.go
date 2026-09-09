//go:build linux && mipsle

package main

import (
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"os"
	"strconv"
	"strings"
	"time"

	"golang.org/x/sys/unix"
)

const controls = "/sys/devices/platform/e0266a128/epaper/"
const leasePath = "/dev/shm/c1ancher-external-app.lock"
const modePath = "/dev/shm/c1ancher-external-app.mode"
const modeGuard = "/dev/shm/c1ancher-external-app.mode-guard"
const evIOCGrab = 0x80044590 // MIPS _IOW('E', 0x90, int)

type deviceDisplay struct {
	fd, lease    int
	inputs       []int
	keys         chan uint16
	stop, done   chan struct{}
	previousFast string
	last         frame
	ready        bool
}

func secureOpen(path string) (int, error) {
	fd, err := unix.Open(path, unix.O_CREAT|unix.O_RDWR|unix.O_NOFOLLOW|unix.O_NONBLOCK|unix.O_CLOEXEC, 0600)
	if err != nil {
		return -1, err
	}
	var st unix.Stat_t
	err = unix.Fstat(fd, &st)
	if err == nil && (st.Mode&unix.S_IFMT != unix.S_IFREG || st.Mode&0777 != 0600 || st.Uid != uint32(os.Geteuid()) || st.Nlink != 1) {
		err = errors.New("invalid application lock file")
	}
	if err != nil {
		unix.Close(fd)
		return -1, err
	}
	return fd, nil
}
func acquireLease() (int, error) {
	fd, err := secureOpen(leasePath)
	if err != nil {
		return -1, err
	}
	var want unix.Stat_t
	if err = unix.Fstat(fd, &want); err != nil {
		unix.Close(fd)
		return -1, err
	}
	// The package manager may pass the exclusive lease across exec.
	entries, _ := os.ReadDir("/proc/self/fd")
	for _, e := range entries {
		inherited, err := strconv.Atoi(e.Name())
		if err != nil || inherited < 3 || inherited == fd {
			continue
		}
		var st unix.Stat_t
		if unix.Fstat(inherited, &st) == nil && st.Dev == want.Dev && st.Ino == want.Ino && unix.Flock(inherited, unix.LOCK_EX|unix.LOCK_NB) == nil {
			unix.Close(fd)
			unix.CloseOnExec(inherited)
			return inherited, nil
		}
	}
	if err = unix.Flock(fd, unix.LOCK_EX|unix.LOCK_NB); err != nil {
		unix.Close(fd)
		return -1, fmt.Errorf("display already owned: %w", err)
	}
	return fd, nil
}
func setDirectMode() error {
	guard, err := secureOpen(modeGuard)
	if err != nil {
		return err
	}
	defer unix.Close(guard)
	for attempt := 0; ; attempt++ {
		err = unix.Flock(guard, unix.LOCK_EX|unix.LOCK_NB)
		if err == nil {
			break
		}
		if attempt >= 99 || (err != unix.EAGAIN && err != unix.EINTR) {
			return err
		}
		time.Sleep(time.Millisecond)
	}
	fd, err := secureOpen(modePath)
	if err != nil {
		return err
	}
	defer unix.Close(fd)
	if err = unix.Ftruncate(fd, 0); err != nil {
		return err
	}
	n, err := unix.Write(fd, []byte("direct"))
	if err == nil && n != 6 {
		err = io.ErrShortWrite
	}
	return err
}
func control(name, value string) error { return os.WriteFile(controls+name, []byte(value), 0600) }
func openDisplay() (display, error) {
	lease, err := acquireLease()
	if err != nil {
		return nil, err
	}
	d := &deviceDisplay{fd: -1, lease: lease, keys: make(chan uint16, 32), stop: make(chan struct{}), done: make(chan struct{})}
	fail := func(err error) (display, error) {
		for _, fd := range d.inputs {
			unix.Close(fd)
		}
		if d.fd >= 0 {
			unix.Close(d.fd)
		}
		unix.Close(lease)
		return nil, err
	}
	if err = setDirectMode(); err != nil {
		return fail(err)
	}
	data, err := os.ReadFile(controls + "fast_refresh_only")
	if err != nil {
		return fail(err)
	}
	d.previousFast = strings.TrimSpace(string(data))
	if d.previousFast != "0" && d.previousFast != "1" {
		return fail(errors.New("unexpected fast_refresh_only value"))
	}
	d.fd, err = unix.Open("/dev/epaper_lcd", unix.O_WRONLY|unix.O_NONBLOCK|unix.O_CLOEXEC, 0)
	if err != nil {
		return fail(err)
	}
	for i, path := range []string{"/dev/input/event0", "/dev/input/event1"} {
		fd, err := unix.Open(path, unix.O_RDONLY|unix.O_NONBLOCK|unix.O_CLOEXEC, 0)
		if err != nil {
			return fail(err)
		}
		d.inputs = append(d.inputs, fd)
		// GPIO keys (including power) stay shared with the core.
		if i == 0 {
			if err = unix.IoctlSetInt(fd, evIOCGrab, 1); err != nil {
				return fail(err)
			}
		}
	}
	go d.readInput()
	time.Sleep(150 * time.Millisecond) // let the core observe exclusive ownership
	return d, nil
}
func (d *deviceDisplay) events() <-chan uint16 { return d.keys }
func (d *deviceDisplay) draw(f frame, full bool) (err error) {
	if d.ready && f == d.last && !full {
		return nil
	}
	full = full || !d.ready
	if full {
		if err = control("fast_refresh_only", "0"); err != nil {
			return err
		}
		defer func() { err = errors.Join(err, control("fast_refresh_only", "1")) }()
	}
	var n int
	for {
		n, err = unix.Write(d.fd, f[:])
		if err != unix.EINTR {
			break
		}
	}
	if err != nil {
		return err
	}
	if n != len(f) {
		return io.ErrShortWrite
	}
	if full {
		if err = control("refresh", "1"); err != nil {
			return err
		}
	}
	d.last = f
	d.ready = true
	return nil
}
func (d *deviceDisplay) close() error {
	close(d.stop)
	<-d.done
	for _, fd := range d.inputs {
		unix.Close(fd)
	}
	// Restore the original global refresh policy before yielding the screen.
	err := control("fast_refresh_only", d.previousFast)
	err = errors.Join(err, unix.Close(d.fd), unix.Close(d.lease))
	return err
}
func (d *deviceDisplay) readInput() {
	defer close(d.done)
	defer close(d.keys)
	polls := make([]unix.PollFd, len(d.inputs))
	for i, fd := range d.inputs {
		polls[i] = unix.PollFd{Fd: int32(fd), Events: unix.POLLIN}
	}
	var buf [16 * 32]byte
	dropped := make([]bool, len(polls))
	for {
		select {
		case <-d.stop:
			return
		default:
		}
		n, err := unix.Poll(polls, 50)
		if err == unix.EINTR {
			continue
		}
		if err != nil {
			return
		}
		if n == 0 {
			continue
		}
		for i, p := range polls {
			if p.Revents&(unix.POLLERR|unix.POLLHUP|unix.POLLNVAL) != 0 {
				return
			}
			if p.Revents&unix.POLLIN == 0 {
				continue
			}
			n, err = unix.Read(int(p.Fd), buf[:])
			if err == unix.EINTR || err == unix.EAGAIN {
				continue
			}
			if err != nil || n == 0 || n%16 != 0 {
				return
			}
			for off := 0; off < n; off += 16 {
				typ := binary.LittleEndian.Uint16(buf[off+8:])
				code := binary.LittleEndian.Uint16(buf[off+10:])
				value := binary.LittleEndian.Uint32(buf[off+12:])
				if typ == 0 && code == 3 {
					dropped[i] = true
					continue
				}
				if dropped[i] {
					if typ == 0 && code == 0 {
						dropped[i] = false
					}
					continue
				}
				if typ != 1 || value != 1 || code == 143 || code == 116 {
					continue
				}
				select {
				case d.keys <- code:
				case <-d.stop:
					return
				}
			}
		}
	}
}
