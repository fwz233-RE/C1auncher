package c1device

const (
	DisplayWidth  = 296
	DisplayHeight = 152
	FrameBytes    = DisplayWidth * (DisplayHeight / 8)
)

type Frame [FrameBytes]byte

type Key uint8

const (
	KeyUnknown Key = iota
	KeyUp
	KeyDown
	KeyLeft
	KeyRight
	KeyOK
	KeyBack
	KeyPause
	KeyVolumeDown
	KeyVolumeUp
	KeyRune
)

type Event struct {
	Key    Key
	Rune   rune
	Repeat bool
}

type Platform interface {
	Draw(Frame, bool) error
	Events() <-chan Event
	Close() error
}
