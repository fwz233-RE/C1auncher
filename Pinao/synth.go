package main

import "math"

const (
	sampleRate     = 48000
	voiceCount     = 8
	waveBits       = 11
	waveSize       = 1 << waveBits
	phaseScale     = 4294967296.0 / sampleRate
	attackSamples  = sampleRate / 200 // 5 ms
	releaseSamples = sampleRate / 40  // 25 ms
	tailSamples    = sampleRate / 200
	maxHoldSamples = 8 * sampleRate
)

// The immutable table is shared by all engines. No trigonometry or allocation
// is performed in Render. Linear interpolation keeps the table small (8 KiB).
var sineTable = func() [waveSize + 1]float32 {
	var table [waveSize + 1]float32
	for i := range table {
		table[i] = float32(math.Sin(2 * math.Pi * float64(i) / waveSize))
	}
	return table
}()

func sine(phase uint32) float32 {
	index := phase >> (32 - waveBits)
	fraction := float32(phase&((1<<(32-waveBits))-1)) / (1 << (32 - waveBits))
	return sineTable[index] + (sineTable[index+1]-sineTable[index])*fraction
}

type synthVoice struct {
	active         bool
	midi           int
	id             int
	serial         uint64
	timbre         int
	drum           bool
	age            int
	duration       int
	phase          [4]uint32
	step           [4]uint32
	gain           [4]float32
	harmonic       [4]float32
	harmonicDecay  [4]float32
	velocity       float32
	envelope       float32
	attack         int
	sustain        float32
	decay          float32
	transient      float32
	transientDecay float32
	releasing      bool
	releaseStep    float32
	releaseLeft    int
	noise          uint32
	previousNoise  float32
	pitch          float32
	pitchDecay     float32
	// A tiny fixed fade preserves continuity when replacing an occupied voice.
	tail     float32
	tailLeft int
	last     float32
}

// Synth is a fixed-capacity, single-threaded stereo PCM engine. The caller must
// serialize control calls and Render. IDs identify notes, not MIDI pitches;
// drums share the same ID space and the same eight slots.
type Synth struct {
	voices   [voiceCount]synthVoice
	serial   uint64
	volume   float32
	loopKeep uint8 // Protect surviving continuations from sequencer voice recovery.
}

func NewSynth() *Synth {
	return &Synth{volume: 0.8}
}

// SetVolume clamps the master volume to 0..100. It does not alter envelopes.
func (s *Synth) SetVolume(v int) {
	if v < 0 {
		v = 0
	}
	if v > 100 {
		v = 100
	}
	s.volume = float32(v) / 100
}

func (s *Synth) allocate(id int) *synthVoice {
	index := -1
	for i := range s.voices {
		if s.voices[i].active && s.voices[i].id == id {
			index = i
			break
		}
	}
	if index < 0 {
		for i := range s.voices {
			if !s.voices[i].active {
				index = i
				break
			}
		}
	}
	if index < 0 {
		for i := range s.voices {
			v := &s.voices[i]
			// A sequencer recovery/new note may replace live or released voices,
			// but must not cascade through other sustained notes in this batch.
			// Live keys retain the original oldest-voice stealing behavior.
			if id >= 300 && id < 308 && v.id >= 300 && v.id < 308 && s.loopKeep&(1<<uint(v.id-300)) != 0 {
				continue
			}
			if index < 0 || v.serial < s.voices[index].serial {
				index = i
			}
		}
		if index < 0 {
			index = 0
		} // Defensive fallback for inconsistent external commands.
	}
	v := &s.voices[index]
	tail := v.last
	s.serial++
	*v = synthVoice{active: true, id: id, serial: s.serial, velocity: 1,
		attack: attackSamples, transient: 1, tail: tail, tailLeft: tailSamples}
	return v
}

// NoteOn clamps MIDI to 36..96 and velocity to 0..1. Unknown timbres use piano.
// Nonpositive or nonfinite velocity releases the ID instead of creating a note.
// Any integer ID is valid. Repeated IDs retrigger rather than consume a slot.
func (s *Synth) NoteOn(id int, midi int, timbre int, velocity float64) {
	if math.IsNaN(velocity) || math.IsInf(velocity, 0) || velocity <= 0 {
		s.NoteOff(id)
		return
	}
	if midi < 36 {
		midi = 36
	}
	if midi > 96 {
		midi = 96
	}
	if timbre < 0 || timbre > 2 {
		timbre = 0
	}
	if velocity > 1 {
		velocity = 1
	}
	frequency := 440 * math.Exp2(float64(midi-69)/12)
	v := s.allocate(id)
	v.timbre = timbre
	v.midi = midi
	v.velocity = float32(velocity)
	v.duration = maxHoldSamples
	v.sustain = 0.28
	v.decay = float32(math.Exp(-1 / (0.65 * sampleRate)))
	v.transientDecay = float32(math.Exp(-1 / (0.45 * sampleRate)))
	v.noise = 0x9e3779b9
	ratios := [4]float64{1.0, 2.01, 3.99, 5.02}
	v.gain = [4]float32{0.66, 0.21, 0.085, 0.045}
	v.harmonic = [4]float32{1, 1, 1, 1}
	v.harmonicDecay = [4]float32{v.decay, v.decay, v.decay, v.decay}
	switch timbre {
	case 0:
		// Piano: a slightly inharmonic, detuned partial stack with a short
		// hammer transient. Higher partials decay faster than the fundamental.
		v.transientDecay = float32(math.Exp(-1 / (0.045 * sampleRate)))
		v.harmonicDecay = [4]float32{
			float32(math.Exp(-1 / (1.20 * sampleRate))),
			float32(math.Exp(-1 / (0.72 * sampleRate))),
			float32(math.Exp(-1 / (0.38 * sampleRate))),
			float32(math.Exp(-1 / (0.20 * sampleRate))),
		}
	case 1:
		ratios = [4]float64{1, 2.01, 3.98, 5.43}
		v.gain = [4]float32{0.68, 0.20, 0.09, 0.03}
		v.sustain = 0.16
		v.decay = float32(math.Exp(-1 / (1.1 * sampleRate)))
		v.transientDecay = float32(math.Exp(-1 / (0.9 * sampleRate)))
	case 2:
		// A band-limited triangle approximation, not a discontinuous square/saw.
		ratios = [4]float64{1, 3, 5, 7}
		v.gain = [4]float32{0.85, -0.095, 0.034, -0.017}
		v.sustain = 0.38
	}
	for i, ratio := range ratios {
		f := frequency * ratio
		if f >= 0.45*sampleRate {
			v.gain[i] = 0
		}
		v.step[i] = uint32(f * phaseScale)
	}
}

func (v *synthVoice) release() {
	if !v.active || v.releasing {
		return
	}
	v.releasing = true
	v.releaseLeft = releaseSamples
	v.releaseStep = v.envelope / releaseSamples
}

// Hold extends a sequenced note's timeout without restarting its oscillator or
// envelope. If live playing stole the voice, re-create it rather than keeping a
// wrong pitch alive. Manual keys still use the original eight-second guard.
func (s *Synth) Hold(id, midi, tone int) {
	for i := range s.voices {
		v := &s.voices[i]
		if v.active && !v.releasing && !v.drum && v.id == id && v.midi == midi && v.timbre == tone {
			if v.duration-v.age < sampleRate*2 {
				v.duration = v.age + sampleRate*2
			}
			return
		}
	}
	s.NoteOn(id, midi, tone, 0.8)
}

func (s *Synth) NoteOff(id int) {
	for i := range s.voices {
		if s.voices[i].active && s.voices[i].id == id {
			s.voices[i].release()
			return
		}
	}
}

// AllOff releases all voices smoothly; call Render to drain the 25 ms release.
func (s *Synth) AllOff() {
	s.loopKeep = 0
	for i := range s.voices {
		s.voices[i].release()
	}
}

// Drum triggers a procedural kick (0), snare (1), hat (2), or clap (3).
// Unknown kinds use kick. Sounds are generated from oscillators and bounded
// deterministic noise, without recordings or external assets.
func (s *Synth) Drum(id int, kind int) {
	if kind < 0 || kind > 3 {
		kind = 0
	}
	v := s.allocate(id)
	v.drum = true
	v.timbre = kind
	v.noise = 0x6d2b79f5
	v.attack = sampleRate / 2000
	v.duration = sampleRate / 3
	v.decay = float32(math.Exp(-1 / (0.045 * sampleRate)))
	v.pitch = 105
	v.pitchDecay = float32(math.Exp(-1 / (0.025 * sampleRate)))
	switch kind {
	case 0:
		v.attack = sampleRate / 500
		v.duration = sampleRate / 2
		v.decay = float32(math.Exp(-1 / (0.09 * sampleRate)))
		// Keep the bass body, but add an audible beater on small speakers.
		// Its noise transient decays independently, rather than becoming a hat.
		v.transientDecay = float32(math.Exp(-1 / (0.012 * sampleRate)))
	case 1:
		v.step[0] = uint32(180 * (uint64(1) << 32) / sampleRate)
	case 2:
		v.duration = sampleRate / 8
		v.decay = float32(math.Exp(-1 / (0.02 * sampleRate)))
	case 3:
		v.duration = sampleRate / 3
		v.decay = float32(math.Exp(-1 / (0.055 * sampleRate)))
	}
}

func (v *synthVoice) oscillator() float32 {
	if !v.drum {
		var value float32
		for i := range v.phase {
			gain := v.gain[i] * v.harmonic[i]
			if i > 0 && v.timbre != 2 {
				gain *= v.transient
			}
			value += gain * sine(v.phase[i])
			v.phase[i] += v.step[i]
			v.harmonic[i] *= v.harmonicDecay[i]
		}
		// A very short, deterministic hammer component adds the woody attack
		// without requiring a sample bank or an extra oscillator.
		if v.timbre == 0 && v.age < sampleRate/100 {
			x := v.noise
			x ^= x << 13
			x ^= x >> 17
			x ^= x << 5
			v.noise = x
			value += 0.12 * float32(x>>8) / 8388608 * (1 - float32(v.age)/(sampleRate/100))
		}
		v.transient *= v.transientDecay
		return value
	}
	// Xorshift32, mapped to [-1,1]; differencing emphasizes high frequencies.
	x := v.noise
	x ^= x << 13
	x ^= x >> 17
	x ^= x << 5
	v.noise = x
	noise := float32(x>>8)/8388608 - 1
	high := (noise - v.previousNoise) * 0.5
	v.previousNoise = noise
	switch v.timbre {
	case 0:
		// The original 150->45 Hz sine can be barely audible on a small
		// speaker. A 1050->420 Hz body and a brief noise attack preserve a
		// percussive identity without changing master volume or other drums.
		// Weights sum to one, preserving the existing eight-voice headroom.
		onset := min(float32(v.age)/float32(v.attack), float32(1))
		value := 0.30*sine(v.phase[0]) + 0.55*sine(v.phase[1]) + 0.15*high*v.transient*onset
		v.phase[0] += uint32((45 + v.pitch) * phaseScale)
		v.phase[1] += uint32((420 + 6*v.pitch) * phaseScale)
		v.pitch *= v.pitchDecay
		v.transient *= v.transientDecay
		return value
	case 1:
		value := 0.72*high + 0.28*sine(v.phase[0])
		v.phase[0] += v.step[0]
		return value
	case 2:
		return high
	default:
		// Three short, smoothly rising noise bursts followed by a diffuse tail.
		burst := float32(1)
		if v.age < 3*480 {
			position := v.age % 480
			if position < 48 {
				burst = float32(position) / 48
			} else {
				burst = float32(480-position) / 432
			}
		}
		return (0.65*high + 0.35*noise) * burst
	}
}

func (v *synthVoice) next() float32 {
	if !v.active {
		return 0
	}
	if v.age >= v.duration {
		v.release()
	}
	if v.releasing {
		v.envelope -= v.releaseStep
		v.releaseLeft--
		if v.envelope < 0 {
			v.envelope = 0
		}
	} else if v.age < v.attack {
		v.envelope = float32(v.age+1) / float32(v.attack)
	} else {
		v.envelope = v.sustain + (v.envelope-v.sustain)*v.decay
	}
	value := v.oscillator() * v.envelope * v.velocity
	if v.tailLeft > 0 {
		value += v.tail * float32(v.tailLeft) / tailSamples
		v.tailLeft--
	}
	// Each slot is bounded even during repeated steals. Eight full-scale slots
	// fit below PCM full scale with a fixed 0.105 gain, without a hard mix clip.
	if value > 1 {
		value = 1
	}
	if value < -1 {
		value = -1
	}
	v.age++
	if v.releasing && v.releaseLeft <= 0 {
		v.active = false
		value = 0
	}
	v.last = value
	return value
}

// Render overwrites dst with interleaved signed 16-bit stereo PCM. Both channels
// are identical. An unmatched final element is zeroed and consumes no frame.
// Nil and empty slices are valid. Rendering never allocates or takes a lock.
func (s *Synth) Active() bool {
	for i := range s.voices {
		if s.voices[i].active {
			return true
		}
	}
	return false
}
func (s *Synth) Render(dst []int16) {
	if !s.Active() {
		clear(dst)
		return
	}
	gain := s.volume * (32767 * 0.105)
	for i := 0; i+1 < len(dst); i += 2 {
		var mixed float32
		for j := range s.voices {
			mixed += s.voices[j].next()
		}
		pcm := int16(mixed * gain)
		dst[i], dst[i+1] = pcm, pcm
	}
	if len(dst)%2 != 0 {
		dst[len(dst)-1] = 0
	}
}
