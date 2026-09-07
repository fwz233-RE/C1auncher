package main

import (
	"c1repo/internal/repository"
	"crypto/ed25519"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"strconv"
	"strings"
)

func readPublicFile(path string, limit int64) ([]byte, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	b, err := io.ReadAll(io.LimitReader(f, limit+1))
	if err != nil {
		return nil, err
	}
	if int64(len(b)) > limit {
		return nil, errors.New("public configuration exceeds size limit")
	}
	return b, nil
}

// Only executable-adjacent public files are discovered; never consult the
// payload/current directory for credentials or trust material.
func loadPublicConfig(directory, origin, keyFile string) (string, ed25519.PublicKey, error) {
	fromSidecar := false
	if origin == "" {
		b, err := readPublicFile(filepath.Join(directory, "server.url"), 4096)
		if err == nil {
			origin = strings.TrimSpace(strings.TrimPrefix(string(b), "\ufeff"))
			if origin == "" || strings.ContainsAny(origin, "\r\n") {
				return "", nil, errors.New("server.url must contain one repository origin")
			}
			fromSidecar = true
		} else if !os.IsNotExist(err) {
			return "", nil, fmt.Errorf("read server.url: %w", err)
		}
		if !fromSidecar {
			origin = "http://www.fwz233.com"
		}
	}
	explicitKey := keyFile != ""
	if !explicitKey {
		keyFile = filepath.Join(directory, "repository.ed25519.pub")
	}
	key, err := readPublicFile(keyFile, ed25519.PublicKeySize)
	if os.IsNotExist(err) && !explicitKey && !fromSidecar {
		return origin, nil, nil
	} // Legacy standalone compatibility.
	if err != nil {
		return "", nil, fmt.Errorf("read trusted public key: %w", err)
	}
	if len(key) != ed25519.PublicKeySize {
		return "", nil, errors.New("repository.ed25519.pub must be a raw 32-byte Ed25519 public key, never a token")
	}
	return origin, ed25519.PublicKey(key), nil
}

func fetchPublic(client *http.Client, address string, limit int64) ([]byte, error) {
	r, err := client.Get(address)
	if err != nil {
		return nil, err
	}
	defer r.Body.Close()
	if r.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("signed catalog request: HTTP %d", r.StatusCode)
	}
	b, err := io.ReadAll(io.LimitReader(r.Body, limit+1))
	if err != nil {
		return nil, err
	}
	if int64(len(b)) > limit {
		return nil, errors.New("signed catalog response exceeds size limit")
	}
	return b, nil
}

// The public key authenticates repository metadata, not publisher identity.
// Fetches are anonymous and never include the key as an HTTP credential.
func verifiedCatalog(client *http.Client, origin string, key ed25519.PublicKey) (repository.Catalog, error) {
	var c repository.Catalog
	data, err := fetchPublic(client, origin+"/c1/v2/index.v1", 256<<10)
	if err != nil {
		return c, err
	}
	sig, err := fetchPublic(client, origin+"/c1/v2/index.v1.sig", ed25519.SignatureSize)
	if err != nil {
		return c, err
	}
	if len(sig) != ed25519.SignatureSize || !ed25519.Verify(key, data, sig) {
		return c, errors.New("repository signature rejected by configured public key")
	}
	if !strings.HasSuffix(string(data), "\n") || strings.ContainsAny(string(data), "\x00\r") {
		return c, errors.New("invalid signed catalog")
	}
	lines := strings.Split(strings.TrimSuffix(string(data), "\n"), "\n")
	if len(lines) < 2 || len(lines) > repository.MaxPackages+2 || lines[0] != "C1PKG-INDEX 2" || !strings.HasPrefix(lines[1], "S\t") {
		return c, errors.New("invalid signed catalog header")
	}
	c.Sequence, err = strconv.ParseUint(strings.TrimPrefix(lines[1], "S\t"), 10, 64)
	if err != nil || c.Sequence == 0 {
		return c, errors.New("invalid signed catalog sequence")
	}
	seen := make(map[string]bool)
	for _, line := range lines[2:] {
		f := strings.Split(line, "\t")
		if len(f) != 9 || f[0] != "P" {
			return c, errors.New("invalid signed catalog package")
		}
		m := repository.Metadata{ID: f[1], Version: f[2], Name: f[3], Entry: f[7]}
		if m.Validate() != nil || seen[strings.ToLower(m.ID)] {
			return c, errors.New("invalid signed catalog metadata")
		}
		seen[strings.ToLower(m.ID)] = true
		c.Packages = append(c.Packages, repository.Package{Metadata: m, Author: f[8]})
	}
	return c, nil
}
