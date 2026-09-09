//go:build linux && mipsle

package main

import "errors"

func writePreview(string, frame) error {
	return errors.New("PNG preview is a host-only command")
}
