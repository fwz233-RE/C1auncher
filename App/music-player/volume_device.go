//go:build linux && mipsle

package main

import (
	"fmt"
	"os/exec"
	"strconv"
)

const maxMixerRaw = 158

func applyVolume(value int) error {
	raw := (value*maxMixerRaw + 50) / 100
	command := exec.Command("/usr/bin/amixer", "-q", "cset", "numid=1,iface=MIXER,name=DAC Playback Volume", strconv.Itoa(raw))
	if err := command.Run(); err != nil {
		return fmt.Errorf("set volume: %w", err)
	}
	return nil
}
