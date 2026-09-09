//go:build linux && mipsle

package main

import "errors"

func writePreview(string, string) error {
	return errors.New("preview is only available in a desktop build")
}
func writePreviewAt(string, string, uint32) error {
	return errors.New("preview is only available in a desktop build")
}
