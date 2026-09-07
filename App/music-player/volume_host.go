//go:build !linux || !mipsle

package main

func applyVolume(_ int) error { return nil }
