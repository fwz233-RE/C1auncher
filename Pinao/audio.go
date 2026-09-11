package main

import (
	"context"
	"encoding/binary"
	"fmt"
	"io"
	"os"
	"time"
)

func applySound(s *Synth, c soundCommand) {
	switch c.Kind {
	case "on":
		s.NoteOn(c.ID, c.MIDI, c.Tone, 0.8)
	case "hold":
		s.Hold(c.ID, c.MIDI, c.Tone)
	case "off":
		s.NoteOff(c.ID)
	case "off-all":
		s.AllOff()
	case "volume":
		s.SetVolume(c.Volume)
	case "drum":
		s.Drum(c.ID, c.Drum)
	case "loop-off":
		s.loopKeep = c.Keep
		for i := 0; i < 8; i++ {
			if c.Keep&(1<<uint(i)) == 0 {
				s.NoteOff(300 + i)
			}
		}
	}
}
func encodePCM(dst []byte, src []int16) {
	for i, v := range src {
		binary.LittleEndian.PutUint16(dst[i*2:], uint16(v))
	}
}

// Export is deterministic and independent of live audio / device drivers.
func exportWAV(path string, song Song) error {
	return exportWAVContext(context.Background(), path, song)
}
func exportWAVContext(ctx context.Context, path string, song Song) (err error) {
	if err = ctx.Err(); err != nil {
		return err
	}
	if err = song.validate(); err != nil {
		return err
	}
	f, err := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0600)
	if err != nil {
		return err
	}
	defer func() {
		e := f.Close()
		if err == nil {
			err = e
		}
		if err != nil {
			_ = os.Remove(path)
		}
	}()
	totalSteps := song.pageCount() * steps
	loopFrames := int(int64(sampleRate) * 60 * int64(totalSteps) / int64(song.BPM*2))
	frames := loopFrames + sampleRate
	header := make([]byte, 44)
	copy(header, "RIFF")
	binary.LittleEndian.PutUint32(header[4:], uint32(36+frames*4))
	copy(header[8:], "WAVEfmt ")
	binary.LittleEndian.PutUint32(header[16:], 16)
	binary.LittleEndian.PutUint16(header[20:], 1)
	binary.LittleEndian.PutUint16(header[22:], 2)
	binary.LittleEndian.PutUint32(header[24:], sampleRate)
	binary.LittleEndian.PutUint32(header[28:], sampleRate*4)
	binary.LittleEndian.PutUint16(header[32:], 4)
	binary.LittleEndian.PutUint16(header[34:], 16)
	copy(header[36:], "data")
	binary.LittleEndian.PutUint32(header[40:], uint32(frames*4))
	if _, err = f.Write(header); err != nil {
		return err
	}
	synth := NewSynth()
	synth.SetVolume(song.Volume)
	pcm := make([]int16, 480*2)
	raw := make([]byte, len(pcm)*2)
	step := -1
	var transport loopTransport
	var commands [9]soundCommand
	for offset := 0; offset < frames; {
		if err = ctx.Err(); err != nil {
			return err
		}
		n := min(480, frames-offset)
		idx := int(int64(offset) * int64(song.BPM*2) / (sampleRate * 60))
		if idx >= totalSteps {
			if step != totalSteps {
				synth.AllOff()
				step = totalSteps
			}
		} else {
			boundary := int((int64(idx+1)*sampleRate*60 + int64(song.BPM*2) - 1) / int64(song.BPM*2))
			n = min(n, max(1, boundary-offset))
			if idx != step {
				for _, c := range transport.commands(commands[:0], song.cell(idx), idx, true) {
					applySound(synth, c)
				}
				step = idx
			}
		}
		synth.Render(pcm[:n*2])
		encodePCM(raw[:n*4], pcm[:n*2])
		var written int
		written, err = f.Write(raw[:n*4])
		if err != nil {
			return err
		}
		if written != n*4 {
			return io.ErrShortWrite
		}
		offset += n
	}
	return f.Sync()
}
func defaultSongPath() string {
	if p := os.Getenv("PINAO_DATA_DIR"); p != "" {
		return p + "/song.json"
	}
	return deviceDataDirectory() + "/song.json"
}
func exportName(dir string, now time.Time) string {
	return fmt.Sprintf("%s/pinao-%s-%09d.wav", dir, now.Format("20060102-150405"), now.Nanosecond())
}
