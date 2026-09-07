//go:build windows

package main

// Windows host tests exercise the write/rename path; directory fsync is a
// device/Linux durability guarantee rather than a Windows directory operation.
func syncWallpaperDirectory(string) error { return nil }
