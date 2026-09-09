package main

import (
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"os"
	"strings"
	"time"
)

const (
	width       = 296
	height      = 152
	frameBytes  = width * height / 8
	headerBytes = 32
	assetMagic  = "C1BA0001"
)

type frame [frameBytes]byte

type asset struct {
	file           *os.File // nil for the built-in animation
	reader         io.ReaderAt
	count          uint32
	fpsNum, fpsDen uint32
}

// Independent, fixed-size frames permit seeking directly to the current time.
// Never allocate from untrusted header fields or load the entire animation.
func openAsset(path string) (*asset, error) {
	if path == "" {
		return parseAsset(strings.NewReader(embeddedAnimation), int64(len(embeddedAnimation)))
	}
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	st, err := f.Stat()
	if err != nil {
		_ = f.Close()
		return nil, err
	}
	if !st.Mode().IsRegular() {
		_ = f.Close()
		return nil, errors.New("animation must be a regular file")
	}
	a, err := parseAsset(f, st.Size())
	if err != nil {
		_ = f.Close()
		return nil, err
	}
	a.file = f
	return a, nil
}
func parseAsset(reader io.ReaderAt, size int64) (*asset, error) {
	var h [headerBytes]byte
	if _, err := reader.ReadAt(h[:], 0); err != nil {
		return nil, fmt.Errorf("animation header: %w", err)
	}
	u32 := func(off int) uint32 { return binary.LittleEndian.Uint32(h[off:]) }
	a := &asset{reader: reader, fpsNum: u32(12), fpsDen: u32(16), count: u32(20)}
	if string(h[:8]) != assetMagic || binary.LittleEndian.Uint16(h[8:]) != width || binary.LittleEndian.Uint16(h[10:]) != height || u32(24) != frameBytes || u32(28) != 0 {
		return nil, errors.New("unsupported animation format; use tools/prepare.py")
	}
	if a.fpsNum == 0 || a.fpsNum > 30000 || a.fpsDen == 0 || a.fpsDen > 1000 || uint64(a.fpsNum) > 30*uint64(a.fpsDen) || a.fpsNum < a.fpsDen || a.count == 0 || a.count > 108000 {
		return nil, errors.New("invalid animation timing (1..30 fps, maximum 108000 frames)")
	}
	if size != headerBytes+int64(a.count)*frameBytes {
		return nil, errors.New("animation payload size mismatch")
	}
	return a, nil
}
func (a *asset) close() {
	if a.file != nil {
		_ = a.file.Close()
	}
}
func (a *asset) read(index uint32) (frame, error) {
	var f frame
	if index >= a.count {
		return f, errors.New("frame index out of range")
	}
	_, err := a.reader.ReadAt(f[:], headerBytes+int64(index)*frameBytes)
	return f, err
}
func (a *asset) duration() time.Duration {
	return time.Duration((uint64(a.count)*uint64(a.fpsDen)*uint64(time.Second) + uint64(a.fpsNum) - 1) / uint64(a.fpsNum))
}
func (a *asset) index(at time.Duration) uint32 {
	if at <= 0 {
		return 0
	}
	if at >= a.duration() {
		return a.count - 1
	}
	// Split seconds to keep multiplication bounded even for long assets.
	n := uint64(at/time.Second) * uint64(a.fpsNum) / uint64(a.fpsDen)
	rem := uint64(at/time.Second) * uint64(a.fpsNum) % uint64(a.fpsDen)
	n += (rem*uint64(time.Second) + uint64(at%time.Second)*uint64(a.fpsNum)) / (uint64(a.fpsDen) * uint64(time.Second))
	if n >= uint64(a.count) {
		return a.count - 1
	}
	return uint32(n)
}

type playback struct {
	base    time.Duration
	started time.Time
	paused  bool
	length  time.Duration
	loop    bool
}

func (p *playback) position(now time.Time) time.Duration {
	at := p.base
	if !p.paused && now.After(p.started) {
		at += now.Sub(p.started)
	}
	if p.loop && p.length > 0 {
		return at % p.length
	}
	if at > p.length {
		return p.length
	}
	return at
}
func (p *playback) toggle(now time.Time) {
	p.base = p.position(now)
	p.started = now
	p.paused = !p.paused
}
func (p *playback) restart(now time.Time) { p.base = 0; p.started = now }
func (p *playback) seek(delta time.Duration, now time.Time) {
	p.base = p.position(now) + delta
	if p.base < 0 {
		p.base = 0
	}
	if p.base > p.length {
		p.base = p.length
	}
	p.started = now
}
