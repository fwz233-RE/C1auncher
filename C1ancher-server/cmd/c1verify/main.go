// c1verify verifies a detached Ed25519 signature against a pre-provisioned key.
// It neither downloads nor changes any trust root.
package main

import (
	"crypto/ed25519"
	"fmt"
	"io"
	"os"
)

func read(path string, maximum int64) ([]byte, error) {
	f, e := os.Open(path)
	if e != nil {
		return nil, e
	}
	defer f.Close()
	b, e := io.ReadAll(io.LimitReader(f, maximum+1))
	if e != nil {
		return nil, e
	}
	if int64(len(b)) > maximum {
		return nil, fmt.Errorf("input exceeds limit")
	}
	return b, nil
}
func main() {
	if len(os.Args) != 4 {
		fmt.Fprintln(os.Stderr, "usage: c1verify RAW_PUBLIC_KEY PAYLOAD SIGNATURE")
		os.Exit(2)
	}
	key, e := read(os.Args[1], 32)
	if e != nil || len(key) != 32 {
		fmt.Fprintln(os.Stderr, "invalid trusted public key")
		os.Exit(1)
	}
	data, e := read(os.Args[2], 256<<10)
	if e != nil {
		fmt.Fprintln(os.Stderr, "cannot read bounded payload")
		os.Exit(1)
	}
	signature, e := read(os.Args[3], 64)
	if e != nil || len(signature) != 64 || !ed25519.Verify(key, data, signature) {
		fmt.Fprintln(os.Stderr, "signature rejected")
		os.Exit(1)
	}
	fmt.Println("Ed25519 signature verified against existing trusted key")
}
