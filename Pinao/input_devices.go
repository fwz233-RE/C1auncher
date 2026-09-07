package main

import (
	"errors"
	"fmt"
)

// C1-Slim: event0 is the matrix keyboard; event1 is gpio_keys, including
// KEY_WAKEUP (the physical power button). Keep gpio_keys shared with the core.
// Separate evdev opens each receive events; reading here does not consume the
// core's copy. Only EVIOCGRAB would prevent the core from seeing them.
func openMusicInputs(open func(string) (int, error), grab func(int) error, closeFD func(int)) ([]int, error) {
	devices := [...]struct {
		path      string
		exclusive bool
	}{
		{"/dev/input/event0", true},
		{"/dev/input/event1", false},
	}
	var inputs []int
	for _, device := range devices {
		fd, err := open(device.path)
		if err != nil {
			continue
		}
		if device.exclusive {
			if err = grab(fd); err != nil {
				closeFD(fd)
				for _, opened := range inputs {
					closeFD(opened)
				}
				return nil, fmt.Errorf("grab keyboard: %w", err)
			}
		}
		inputs = append(inputs, fd)
	}
	if len(inputs) == 0 {
		return nil, errors.New("no input devices")
	}
	return inputs, nil
}

func systemPowerKey(code uint16) bool {
	return code == 143 || code == 116 // KEY_WAKEUP / KEY_POWER belong to the core.
}
