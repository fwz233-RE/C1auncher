package repository

import (
	"bytes"
	"crypto/ed25519"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"sync"
)

type snapshot struct {
	Registry          *identityRegistry `json:"registry,omitempty"`
	RegistrySignature []byte            `json:"registrySignature,omitempty"`
	Catalog           Catalog           `json:"catalog"`
	Index1            []byte            `json:"index1"`
	Signature1        []byte            `json:"signature1"`
	Index2            []byte            `json:"index2"`
	Signature2        []byte            `json:"signature2"`
}
type Store struct {
	mu          sync.RWMutex
	root        string
	key         ed25519.PrivateKey
	public      ed25519.PublicKey
	current     snapshot
	lock        *os.File
	maxBytes    int64
	writeFailed bool
}

func index(c Catalog, version int) []byte {
	var b strings.Builder
	fmt.Fprintf(&b, "C1PKG-INDEX %d\nS\t%d\n", version, c.Sequence)
	packages := append([]Package(nil), c.Packages...)
	sort.Slice(packages, func(i, j int) bool { return packages[i].ID < packages[j].ID })
	for _, p := range packages {
		fmt.Fprintf(&b, "P\t%s\t%s\t%s\t%s\t%s\t%d\t%s", p.ID, p.Version, p.Name, p.Archive, p.SHA256, p.Size, p.Entry)
		if version == 2 {
			fmt.Fprintf(&b, "\t%s", p.Author)
		}
		b.WriteByte('\n')
	}
	return []byte(b.String())
}
func Open(c Config) (*Store, error) {
	if err := os.MkdirAll(filepath.Join(c.Root, "objects"), 0700); err != nil {
		return nil, err
	}
	lock, err := lockStore(filepath.Join(c.Root, "repository.lock"))
	if err != nil {
		return nil, err
	}
	s := &Store{root: c.Root, lock: lock, maxBytes: c.MaxRepositoryBytes}
	if s.maxBytes == 0 {
		s.maxBytes = 2 << 30
	}
	ok := false
	defer func() {
		if !ok {
			s.Close()
		}
	}()
	public, err := os.ReadFile(c.PublicKey)
	if err != nil || len(public) != ed25519.PublicKeySize {
		return nil, errors.New("trusted public key must be a raw 32-byte Ed25519 key")
	}
	s.public = public
	if c.SigningKey != "" {
		s.key, err = LoadKey(c.SigningKey)
		if err != nil {
			return nil, err
		}
		if !bytes.Equal(s.key.Public().(ed25519.PublicKey), s.public) {
			return nil, errors.New("signing key differs from provisioned device trust key")
		}
	}
	data, err := readSnapshot(filepath.Join(c.Root, "current.json"))
	if os.IsNotExist(err) {
		if len(s.key) == 0 {
			return nil, errors.New("initialization requires a signing key or imported snapshot")
		}
		seq := c.SequenceFloor
		if seq == 0 {
			seq = 1
		}
		s.current = s.sign(Catalog{Sequence: seq, Packages: []Package{}})
		if err = s.persist(s.current); err != nil {
			return nil, err
		}
	} else if err != nil {
		return nil, err
	} else {
		d := json.NewDecoder(bytes.NewReader(data))
		d.DisallowUnknownFields()
		if err = d.Decode(&s.current); err != nil {
			return nil, err
		}
		if d.Decode(new(any)) != io.EOF {
			return nil, errors.New("extra JSON after stored snapshot")
		}
		if err = s.validate(s.current); err != nil {
			return nil, err
		}
		if s.current.Catalog.Sequence < c.SequenceFloor {
			return nil, errors.New("catalog below configured migration sequence floor")
		}
	}
	if err = s.validateRegistry(c); err != nil {
		return nil, err
	}
	ok = true
	return s, nil
}
func (s *Store) Close() error {
	if s.lock != nil {
		return s.lock.Close()
	}
	return nil
}
func (s *Store) sign(c Catalog) snapshot {
	v1, v2 := index(c, 1), index(c, 2)
	v := snapshot{Catalog: c, Index1: v1, Signature1: ed25519.Sign(s.key, v1), Index2: v2, Signature2: ed25519.Sign(s.key, v2), Registry: cloneRegistry(s.current.Registry)}
	if s.current.Registry == nil {
		v.Registry = nil
	}
	s.signRegistry(&v)
	return v
}
func (s *Store) validate(v snapshot) error {
	if v.Catalog.Sequence == 0 || len(v.Catalog.Packages) > MaxPackages {
		return errors.New("invalid stored catalog")
	}
	if !bytes.Equal(index(v.Catalog, 1), v.Index1) || !bytes.Equal(index(v.Catalog, 2), v.Index2) || !ed25519.Verify(s.public, v.Index1, v.Signature1) || !ed25519.Verify(s.public, v.Index2, v.Signature2) {
		return errors.New("stored catalog signature rejected")
	}
	seen := map[string]bool{}
	for _, p := range v.Catalog.Packages {
		if p.Metadata.Validate() != nil || !label(p.Author) || seen[strings.ToLower(p.ID)] || p.Archive != "objects/"+p.SHA256+".tar.gz" || len(p.SHA256) != 64 {
			return errors.New("invalid stored package")
		}
		seen[strings.ToLower(p.ID)] = true
		if _, err := hex.DecodeString(p.SHA256); err != nil {
			return err
		}
		b, err := os.ReadFile(filepath.Join(s.root, filepath.FromSlash(p.Archive)))
		if err != nil {
			return err
		}
		h := sha256.Sum256(b)
		if p.Size != int64(len(b)) || len(b) > MaxArchive || hex.EncodeToString(h[:]) != p.SHA256 {
			return errors.New("stored package corrupted")
		}
	}
	return nil
}
func atomicWrite(file string, b []byte) error {
	f, err := os.CreateTemp(filepath.Dir(file), ".write-*")
	if err != nil {
		return err
	}
	name := f.Name()
	defer os.Remove(name)
	if _, err = f.Write(b); err != nil {
		f.Close()
		return err
	}
	if err = f.Sync(); err != nil {
		f.Close()
		return err
	}
	if err = f.Close(); err != nil {
		return err
	}
	if err = os.Rename(name, file); err != nil {
		return err
	}
	return syncDir(filepath.Dir(file))
}
func (s *Store) persist(v snapshot) error {
	b, err := json.Marshal(v)
	if err != nil {
		return err
	}
	return atomicWrite(filepath.Join(s.root, "current.json"), b)
}
func (s *Store) Catalog() Catalog {
	s.mu.RLock()
	defer s.mu.RUnlock()
	c := s.current.Catalog
	c.Packages = append([]Package(nil), c.Packages...)
	return c
}
func (s *Store) Index(version int, signature bool) []byte {
	s.mu.RLock()
	defer s.mu.RUnlock()
	v := s.current.Index2
	if version == 1 {
		v = s.current.Index1
	}
	if signature {
		v = s.current.Signature2
		if version == 1 {
			v = s.current.Signature1
		}
	}
	return append([]byte(nil), v...)
}

var ErrConflict = errors.New("version must be newer; already published versions are immutable")

var ErrProtected = errors.New("this application is protected; anonymous updates are not allowed")

func (s *Store) Publish(m Metadata, author string, archive []byte) (Package, uint64, error) {
	if author == OpenPublisherName {
		return Package{}, 0, errors.New("reserved anonymous author marker")
	}
	return s.publish(m, author, archive, false, nil, Publisher{})
}

// PublishOpen preserves provenance in the signed author column. Existing token
// or imported applications remain protected, including after config changes.
func (s *Store) PublishOpen(m Metadata, archive []byte) (Package, uint64, error) {
	return s.publish(m, OpenPublisherName, archive, true, nil, Publisher{})
}

// PublishAuthorized rechecks ownership and commits any new claim with the catalog.
func (s *Store) PublishAuthorized(c Config, publisher Publisher, m Metadata, archive []byte, open bool) (Package, uint64, error) {
	author := publisher.Name
	if open {
		author = OpenPublisherName
	}
	return s.publish(m, author, archive, open, &c, publisher)
}

func (s *Store) publish(m Metadata, author string, archive []byte, open bool, config *Config, publisher Publisher) (Package, uint64, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if config != nil {
		if err := s.authorizeLocked(*config, publisher, m.ID, open); err != nil {
			return Package{}, 0, err
		}
	}
	if s.writeFailed {
		return Package{}, 0, errors.New("repository persistence failed; restart and verify storage before publishing")
	}
	if len(s.key) == 0 {
		return Package{}, 0, errors.New("repository is read-only")
	}
	if err := m.Validate(); err != nil {
		return Package{}, 0, err
	}
	if !label(author) || len(archive) > MaxArchive {
		return Package{}, 0, errors.New("invalid author or archive size")
	}
	digest := sha256.Sum256(archive)
	hash := hex.EncodeToString(digest[:])
	p := Package{m, author, "objects/" + hash + ".tar.gz", hash, int64(len(archive))}
	c := s.current.Catalog
	c.Packages = append([]Package(nil), c.Packages...)
	found := -1
	for i, old := range c.Packages {
		if strings.EqualFold(old.ID, m.ID) && old.ID != m.ID {
			return Package{}, 0, ErrConflict
		}
		if old.ID != m.ID {
			continue
		}
		found = i
		if open && old.Author != OpenPublisherName {
			return Package{}, 0, ErrProtected
		}
		if old.Version == p.Version && old.SHA256 == p.SHA256 && old.Author == p.Author && old.Metadata == p.Metadata {
			return old, c.Sequence, nil
		}
		compare, err := CompareVersion(m.Version, old.Version)
		if err != nil || compare <= 0 {
			return Package{}, 0, ErrConflict
		}
	}
	if found < 0 {
		if len(c.Packages) >= MaxPackages {
			return Package{}, 0, errors.New("catalog full")
		}
		c.Packages = append(c.Packages, p)
	} else {
		c.Packages[found] = p
	}
	if c.Sequence == ^uint64(0) {
		return Package{}, 0, errors.New("sequence exhausted")
	}
	c.Sequence++
	object := filepath.Join(s.root, filepath.FromSlash(p.Archive))
	if old, err := os.ReadFile(object); err == nil {
		if !bytes.Equal(old, archive) {
			return Package{}, 0, errors.New("immutable object collision")
		}
	} else if !os.IsNotExist(err) {
		return Package{}, 0, err
	} else {
		entries, e := os.ReadDir(filepath.Join(s.root, "objects"))
		if e != nil {
			return Package{}, 0, e
		}
		used := int64(0)
		for _, entry := range entries {
			info, e := entry.Info()
			if e != nil {
				return Package{}, 0, e
			}
			used += info.Size()
		}
		if int64(len(archive)) > s.maxBytes-used {
			return Package{}, 0, errors.New("repository storage budget reached; archive old releases before publishing")
		}
		if err = atomicWrite(object, archive); err != nil {
			return Package{}, 0, err
		}
	}
	next := s.sign(c)
	if config != nil && config.SelfRegistration && !open && found < 0 {
		next.Registry = cloneRegistry(next.Registry)
		next.Registry.Claims = append(next.Registry.Claims, applicationClaim{ID: m.ID, Name: author})
		s.signRegistry(&next)
	}
	if err := s.persist(next); err != nil {
		// Rename may already have committed despite a directory fsync error.
		// Stop writes rather than reuse an old in-memory sequence.
		s.writeFailed = true
		return Package{}, 0, err
	}
	s.current = next
	return p, c.Sequence, nil
}
