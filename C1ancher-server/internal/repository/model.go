// Package repository implements a small, single-process signed application repository.
package repository

import (
	"crypto/ed25519"
	"crypto/sha256"
	"crypto/subtle"
	"crypto/x509"
	"encoding/hex"
	"encoding/json"
	"encoding/pem"
	"errors"
	"fmt"
	"io"
	"os"
	"path"
	"regexp"
	"strconv"
	"strings"
	"unicode/utf8"
)

const (
	MaxArchive  = 32 << 20
	MaxFile     = 16 << 20
	MaxUnpacked = 64 << 20
	MaxPackages = 128
)

// OpenPublisherName is a signed provenance marker, not a verified identity.
// Anonymous open applications can be updated by any anonymous publisher.
const OpenPublisherName = "Anonymous (open)"

// Author is assigned by the server, never trusted from uploads.
type Metadata struct {
	ID      string `json:"id"`
	Version string `json:"version"`
	Name    string `json:"name"`
	Entry   string `json:"entry"`
	// Empty preserves legacy packages byte-for-byte. Explicit modes require a
	// device that supports C1PKG-PACKAGE 2; the archive hash authenticates mode.
	Mode string `json:"mode,omitempty"`
}
type Package struct {
	Metadata
	Author  string `json:"author"`
	Archive string `json:"archive"`
	SHA256  string `json:"sha256"`
	Size    int64  `json:"size"`
}
type Catalog struct {
	Sequence uint64    `json:"sequence"`
	Packages []Package `json:"packages"`
}
type Publisher struct {
	Name        string   `json:"name"`
	TokenSHA256 string   `json:"tokenSHA256"`
	Packages    []string `json:"packages"`
}
type Config struct {
	// Opt in to durable self-service identities and authenticated new-ID claims.
	SelfRegistration bool `json:"selfRegistration,omitempty"`
	// Empty or token preserves assigned publisher ownership. Open additionally
	// permits explicit anonymous publication of unassigned ordinary apps.
	PublishMode        string      `json:"publishMode,omitempty"`
	MaxRepositoryBytes int64       `json:"maxRepositoryBytes"`
	CoreRoot           string      `json:"coreRoot"`
	Root               string      `json:"root"`
	Listen             string      `json:"listen"`
	SigningKey         string      `json:"signingKey"`
	PublicKey          string      `json:"publicKey"`
	Publishers         []Publisher `json:"publishers"`
	Downloads          int         `json:"downloads"`
	Queue              int         `json:"queue"`
	BytesPerSecond     int64       `json:"bytesPerSecond"`
	// Signed catalog sequence floor imported from the old repository; never reset it.
	SequenceFloor uint64 `json:"sequenceFloor"`
}

var idPattern = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9._-]{0,31}$`)
var versionPattern = regexp.MustCompile(`^(0|[1-9][0-9]*)(\.(0|[1-9][0-9]*)){1,3}$`)
var pathPattern = regexp.MustCompile(`^[A-Za-z0-9_.+/-]+$`)
var reserved = map[string]bool{"c1pkg": true, "c1updater": true, "c1ancher": true, "c1ancher-launcher": true, "app_daemon": true}

func validID(s string) bool {
	return idPattern.MatchString(s) && !strings.Contains(s, "..") && !strings.HasSuffix(s, ".")
}

// forbiddenLabelRune is intentionally an explicit, version-stable policy
// shared with C1ancher's src/pkg/text.c. Reject controls, bidi/format/invisible
// modifiers, line separators and noncharacters. Reject U+FFFD as well so JSON
// decoder replacement of malformed input cannot turn it into a valid label.
func forbiddenLabelRune(c rune) bool {
	ranges := [...][2]rune{
		{0x0000, 0x001f}, {0x007f, 0x009f}, {0x00ad, 0x00ad},
		{0x034f, 0x034f}, {0x0600, 0x0605}, {0x061c, 0x061c},
		{0x06dd, 0x06dd}, {0x070f, 0x070f}, {0x0890, 0x0891},
		{0x08e2, 0x08e2}, {0x115f, 0x1160}, {0x17b4, 0x17b5},
		{0x180b, 0x180f}, {0x200b, 0x200f}, {0x2028, 0x202e},
		{0x2060, 0x206f}, {0x3164, 0x3164}, {0xfe00, 0xfe0f},
		{0xfeff, 0xfeff}, {0xffa0, 0xffa0}, {0xfff9, 0xfffd},
		{0x110bd, 0x110bd}, {0x110cd, 0x110cd}, {0x13430, 0x1345f},
		{0x1bca0, 0x1bca3}, {0x1d173, 0x1d17a}, {0xe0000, 0xe0fff},
	}
	if (c >= 0xfdd0 && c <= 0xfdef) || (c&0xffff) >= 0xfffe {
		return true
	}
	for _, span := range ranges {
		if c >= span[0] && c <= span[1] {
			return true
		}
	}
	return false
}

func label(s string) bool {
	// Keep the existing 40-BYTE wire limit: device name/author buffers have
	// 41 bytes including NUL. Never truncate UTF-8 or count runes instead.
	if len(s) < 1 || len(s) > 40 || !utf8.ValidString(s) || strings.TrimSpace(s) != s {
		return false
	}
	for _, c := range s {
		if forbiddenLabelRune(c) {
			return false
		}
	}
	return true
}
func safePath(s string) bool {
	if len(s) < 1 || len(s) > 180 || !pathPattern.MatchString(s) || path.Clean(s) != s || strings.HasPrefix(s, "/") {
		return false
	}
	for _, p := range strings.Split(s, "/") {
		if p == ".." || p == "." || strings.HasPrefix(p, ".c1pkg-") {
			return false
		}
	}
	return true
}
func (m Metadata) Validate() error {
	if !validID(m.ID) || reserved[strings.ToLower(m.ID)] {
		return errors.New("invalid or reserved application ID")
	}
	if !versionPattern.MatchString(m.Version) || len(m.Version) > 48 {
		return errors.New("version must contain 2-4 numeric components, e.g. 0.1.12")
	}
	for _, v := range strings.Split(m.Version, ".") {
		if _, err := strconv.ParseUint(v, 10, 64); err != nil {
			return errors.New("version component overflow")
		}
	}
	if m.Mode != "" && m.Mode != "terminal" && m.Mode != "direct" {
		return errors.New("mode must be terminal or direct (requires package-v2 device support)")
	}
	if !label(m.Name) || !safePath(m.Entry) {
		return errors.New("invalid name or entry path")
	}
	return nil
}
func CompareVersion(a, b string) (int, error) {
	if !versionPattern.MatchString(a) || !versionPattern.MatchString(b) {
		return 0, errors.New("unordered version")
	}
	aa, bb := strings.Split(a, "."), strings.Split(b, ".")
	for i := 0; i < 4; i++ {
		var x, y uint64
		var err error
		if i < len(aa) {
			x, err = strconv.ParseUint(aa[i], 10, 64)
			if err != nil {
				return 0, err
			}
		}
		if i < len(bb) {
			y, err = strconv.ParseUint(bb[i], 10, 64)
			if err != nil {
				return 0, err
			}
		}
		if x < y {
			return -1, nil
		}
		if x > y {
			return 1, nil
		}
	}
	return 0, nil
}
func NextVersion(s string) (string, error) {
	if s == "" {
		return "0.1.0", nil
	}
	if !versionPattern.MatchString(s) {
		return "", errors.New("legacy version requires explicit replacement")
	}
	p := strings.Split(s, ".")
	for len(p) < 3 {
		p = append(p, "0")
	}
	n, err := strconv.ParseUint(p[len(p)-1], 10, 64)
	if err != nil || n == ^uint64(0) {
		return "", errors.New("version overflow")
	}
	p[len(p)-1] = strconv.FormatUint(n+1, 10)
	return strings.Join(p, "."), nil
}
func TokenHash(token string) string {
	h := sha256.Sum256([]byte(token))
	return hex.EncodeToString(h[:])
}
func (c Config) Authenticate(token string) (Publisher, bool) {
	if len(token) < 32 || len(token) > 256 {
		return Publisher{}, false
	}
	digest := TokenHash(token)
	for _, p := range c.Publishers {
		if subtle.ConstantTimeCompare([]byte(digest), []byte(p.TokenSHA256)) == 1 {
			return p, true
		}
	}
	return Publisher{}, false
}
func (p Publisher) Owns(id string) bool {
	for _, v := range p.Packages {
		if v == id {
			return true
		}
	}
	return false
}
func LoadConfig(file string) (Config, error) {
	var c Config
	b, err := os.ReadFile(file)
	if err != nil {
		return c, err
	}
	d := json.NewDecoder(strings.NewReader(string(b)))
	d.DisallowUnknownFields()
	if err = d.Decode(&c); err != nil {
		return c, err
	}
	if d.Decode(new(any)) != io.EOF {
		return c, errors.New("extra JSON after config")
	}
	if c.PublishMode != "" && c.PublishMode != "token" && c.PublishMode != "open" {
		return c, errors.New("publishMode must be token or open")
	}
	if c.Root == "" || c.Listen == "" || c.PublicKey == "" {
		return c, errors.New("root, listen, publicKey required")
	}
	if c.MaxRepositoryBytes == 0 {
		c.MaxRepositoryBytes = 2 << 30
	}
	if c.MaxRepositoryBytes < MaxArchive {
		return c, errors.New("repository storage budget must be at least 32 MiB")
	}
	if c.Downloads < 1 || c.Downloads > 8 || c.Queue < 0 || c.Queue > 100 || c.BytesPerSecond < 1024 {
		return c, errors.New("invalid bandwidth or queue limits")
	}
	seen := map[string]bool{}
	tokens := map[string]bool{}
	var publisherNames []string
	for _, p := range c.Publishers {
		if c.SelfRegistration {
			for _, name := range publisherNames {
				if strings.EqualFold(name, p.Name) {
					return c, errors.New("self registration requires unique configured publisher names")
				}
			}
			if !validPublisherName(p.Name) {
				return c, errors.New("invalid configured publisher name")
			}
		}
		publisherNames = append(publisherNames, p.Name)
		digest, e := hex.DecodeString(p.TokenSHA256)
		if !label(p.Name) || p.Name == OpenPublisherName || e != nil || len(digest) != 32 || strings.ToLower(p.TokenSHA256) != p.TokenSHA256 || tokens[p.TokenSHA256] {
			return c, errors.New("invalid publisher or repeated token")
		}
		tokens[p.TokenSHA256] = true
		for _, id := range p.Packages {
			low := strings.ToLower(id)
			if !validID(id) || reserved[low] || seen[low] {
				return c, fmt.Errorf("invalid/duplicate ownership: %s", id)
			}
			seen[low] = true
		}
	}
	return c, nil
}
func LoadKey(file string) (ed25519.PrivateKey, error) {
	b, err := os.ReadFile(file)
	if err != nil {
		return nil, err
	}
	block, rest := pem.Decode(b)
	if block == nil || block.Type != "PRIVATE KEY" || len(strings.TrimSpace(string(rest))) != 0 {
		return nil, errors.New("expected PKCS8 PEM private key")
	}
	k, err := x509.ParsePKCS8PrivateKey(block.Bytes)
	if err != nil {
		return nil, err
	}
	key, ok := k.(ed25519.PrivateKey)
	if !ok {
		return nil, errors.New("expected Ed25519 key")
	}
	return key, nil
}
