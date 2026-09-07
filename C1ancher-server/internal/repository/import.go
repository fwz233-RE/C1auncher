package repository

import (
	"crypto/ed25519"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strconv"
	"strings"
)

// ImportLegacy verifies the OLD index using the existing device trust key.
// No network source or public key downloaded beside the index is trusted.
// Ownership and author names must be supplied explicitly in publisher config.
func ImportLegacy(s *Store, c Config, dir string) error {
	b, err := os.ReadFile(filepath.Join(dir, "index.v1"))
	if err != nil {
		return err
	}
	sig, err := os.ReadFile(filepath.Join(dir, "index.v1.sig"))
	if err != nil {
		return err
	}
	if len(b) > 256<<10 || !ed25519.Verify(s.public, b, sig) {
		return errors.New("legacy index signature rejected")
	}
	if !strings.HasSuffix(string(b), "\n") || strings.ContainsAny(string(b), "\x00\r") {
		return errors.New("invalid legacy index")
	}
	lines := strings.Split(strings.TrimSuffix(string(b), "\n"), "\n")
	if len(lines) < 2 || lines[0] != "C1PKG-INDEX 1" || !strings.HasPrefix(lines[1], "S\t") || len(lines) > 130 {
		return errors.New("invalid legacy header")
	}
	seq, err := strconv.ParseUint(strings.TrimPrefix(lines[1], "S\t"), 10, 64)
	if err != nil || seq == 0 {
		return errors.New("invalid legacy sequence")
	}
	type artifact struct {
		p Package
		b []byte
	}
	var artifacts []artifact
	last := ""
	for _, line := range lines[2:] {
		f := strings.Split(line, "\t")
		if len(f) != 8 || f[0] != "P" || f[1] <= last {
			return errors.New("invalid legacy package record")
		}
		last = f[1]
		m := Metadata{ID: f[1], Version: f[2], Name: f[3], Entry: f[7]}
		if err = m.Validate(); err != nil {
			return err
		}
		if f[4] != "packages/"+m.ID+"/"+m.Version+".tar.gz" || len(f[5]) != 64 {
			return errors.New("invalid legacy archive path")
		}
		size, e := strconv.ParseInt(f[6], 10, 64)
		if e != nil || size < 1 || size > MaxArchive {
			return errors.New("invalid legacy size")
		}
		author := ""
		for _, p := range c.Publishers {
			if p.Owns(m.ID) {
				author = p.Name
			}
		}
		if author == "" {
			return fmt.Errorf("assign publisher ownership and author for %s before import", m.ID)
		}
		file, e := os.OpenInRoot(dir, filepath.FromSlash(f[4]))
		if e != nil {
			return e
		}
		info, e := file.Stat()
		if e != nil || !info.Mode().IsRegular() || info.Size() != size {
			file.Close()
			return errors.New("legacy size mismatch")
		}
		data := make([]byte, size)
		_, e = io.ReadFull(file, data)
		file.Close()
		if e != nil {
			return e
		}
		hash := sha256.Sum256(data)
		if hex.EncodeToString(hash[:]) != f[5] {
			return errors.New("legacy digest mismatch")
		}
		artifacts = append(artifacts, artifact{Package{Metadata: m, Author: author}, data})
	}
	s.mu.Lock()
	if len(s.key) == 0 {
		s.mu.Unlock()
		return errors.New("import requires application signing key")
	}
	if seq > s.current.Catalog.Sequence {
		cat := s.current.Catalog
		cat.Sequence = seq
		v := s.sign(cat)
		if err = s.persist(v); err != nil {
			s.mu.Unlock()
			return err
		}
		s.current = v
	}
	s.mu.Unlock()
	for _, a := range artifacts {
		if _, _, err = s.Publish(a.p.Metadata, a.p.Author, a.b); err != nil {
			return fmt.Errorf("import %s: %w", a.p.ID, err)
		}
	}
	return nil
}
