//go:build !linux || !mipsle

package main

import "errors"

type devicePlatform struct{ output chan keyEvent }

func openPlatform() (*devicePlatform, error) {
	return nil, errors.New("interactive mode requires the C1 Linux MIPS device; use --preview or --demo-wav on the host")
}
func (*devicePlatform) draw(frame, bool) error { return nil }
func (*devicePlatform) close()                 {}
func requestDesktop()                          {}
func deviceDataDirectory() string              { return "data" }

type audioStream struct{ Errors chan error }

func openAudio(int) (*audioStream, error)    { return nil, errors.New("live audio requires the device") }
func (*audioStream) send(soundCommand) error { return nil }
func (*audioStream) close()                  {}
