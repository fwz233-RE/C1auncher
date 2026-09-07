//go:build windows

package main

// Windows host previews do not model Linux directory durability.
func syncDirectory(string) error { return nil }
