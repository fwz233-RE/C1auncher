// Verify the public signature, package digest and installed entry bytes against
// a local build. No publisher credential is needed or transmitted.
package main

import (
	"archive/tar"
	"bytes"
	"compress/gzip"
	"crypto/ed25519"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"strconv"
	"strings"
	"time"
)

func main() {
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
func run() error {
	if len(os.Args) != 6 {
		return fmt.Errorf("usage: check-published ORIGIN PUBLIC_KEY ID VERSION BINARY")
	}
	origin, keyPath, id, version, binary := os.Args[1], os.Args[2], os.Args[3], os.Args[4], os.Args[5]
	key, err := os.ReadFile(keyPath)
	if err != nil {
		return err
	}
	if len(key) != ed25519.PublicKeySize {
		return fmt.Errorf("invalid public key length")
	}
	client := &http.Client{Timeout: 30 * time.Second}
	get := func(address string, limit int64) ([]byte, error) {
		r, e := client.Get(address)
		if e != nil {
			return nil, e
		}
		defer r.Body.Close()
		if r.StatusCode != 200 {
			return nil, fmt.Errorf("HTTP %d", r.StatusCode)
		}
		b, e := io.ReadAll(io.LimitReader(r.Body, limit+1))
		if int64(len(b)) > limit {
			return nil, fmt.Errorf("oversized response")
		}
		return b, e
	}
	index, err := get(origin+"/c1/v2/index.v1", 256<<10)
	if err != nil {
		return err
	}
	sig, err := get(origin+"/c1/v2/index.v1.sig", 64)
	if err != nil {
		return err
	}
	if !ed25519.Verify(key, index, sig) {
		return fmt.Errorf("index signature invalid")
	}
	for _, line := range strings.Split(string(index), "\n") {
		f := strings.Split(line, "\t")
		if len(f) != 9 || f[0] != "P" || f[1] != id {
			continue
		}
		if f[2] != version {
			return fmt.Errorf("server version %s, expected %s", f[2], version)
		}
		if (!strings.HasPrefix(f[4], "packages/") && !strings.HasPrefix(f[4], "objects/")) || strings.Contains(f[4], "..") {
			return fmt.Errorf("unexpected archive path %q", f[4])
		}
		u, err := url.Parse(origin + "/c1/v2/" + f[4])
		if err != nil {
			return err
		}
		archive, err := get(u.String(), 32<<20)
		if err != nil {
			return err
		}
		size, err := strconv.Atoi(f[6])
		if err != nil || len(archive) != size {
			return fmt.Errorf("archive size mismatch")
		}
		h := sha256.Sum256(archive)
		if hex.EncodeToString(h[:]) != f[5] {
			return fmt.Errorf("archive digest mismatch")
		}
		gz, err := gzip.NewReader(bytes.NewReader(archive))
		if err != nil {
			return err
		}
		defer gz.Close()
		tr := tar.NewReader(gz)
		for {
			header, err := tr.Next()
			if err == io.EOF {
				break
			}
			if err != nil {
				return err
			}
			if header.Name != "payload/"+f[7] {
				continue
			}
			body, err := io.ReadAll(io.LimitReader(tr, 16<<20))
			if err != nil {
				return err
			}
			local, err := os.ReadFile(binary)
			if err != nil {
				return err
			}
			if !bytes.Equal(body, local) {
				return fmt.Errorf("entry bytes differ from local build")
			}
			digest := sha256.Sum256(local)
			fmt.Printf("Verified %s %s: Ed25519 index signature, package SHA256 %s, entry SHA256 %x matches local binary (%d bytes)\n", id, version, f[5], digest, len(local))
			return nil
		}
		return fmt.Errorf("entry missing from archive")
	}
	return fmt.Errorf("application missing from index")
}
