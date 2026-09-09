package main

import _ "embed"

// Keep the preprocessed animation in read-only binary data. Reading a frame
// does not copy the whole animation to the Go heap or extract it to disk.
// build.ps1 -AssetPath prepares this build input before invoking Go.
//go:embed embedded/badapple.bap
var embeddedAnimation string
