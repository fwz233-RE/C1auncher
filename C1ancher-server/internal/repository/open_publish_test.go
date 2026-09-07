package repository

import (
	"archive/tar"
	"bytes"
	"compress/gzip"
	"crypto/ed25519"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func openRequest(m Metadata, body []byte) *http.Request {
	data, _ := json.Marshal(m)
	r := httptest.NewRequest("POST", "/api/v1/publish", bytes.NewReader(body))
	r.Header.Set("X-C1-Publish-Mode", "open")
	r.Header.Set("X-C1-Metadata", base64.RawURLEncoding.EncodeToString(data))
	return r
}

func openPublish(t *testing.T, h http.Handler, m Metadata, body []byte, want int) {
	t.Helper()
	w := httptest.NewRecorder()
	h.ServeHTTP(w, openRequest(m, body))
	if w.Code != want {
		t.Fatalf("open publish %s: HTTP %d, want %d: %s", m.ID, w.Code, want, w.Body.String())
	}
}

func TestOpenPublishingExplicitOptInAndOwnership(t *testing.T) {
	c, s := fixture(t)
	m := meta()
	m.ID = "self-service"
	body := goodPayload(t)
	for _, mode := range []string{"", "token"} {
		c.PublishMode = mode
		openPublish(t, NewServer(c, s), m, body, 403)
	}
	c.PublishMode = "open"
	h := NewServer(c, s)
	// Missing/invalid credentials never silently fall back to anonymous mode.
	r := openRequest(m, body)
	r.Header.Del("X-C1-Publish-Mode")
	w := httptest.NewRecorder()
	h.ServeHTTP(w, r)
	if w.Code != 401 {
		t.Fatal(w.Code)
	}
	if w := publishRequest(t, h, m, "wrong", body); w.Code != 401 {
		t.Fatal(w.Code)
	}
	// Public verification material never grants publishing identity.
	if w := publishRequest(t, h, m, base64.RawURLEncoding.EncodeToString(s.public), body); w.Code != 401 {
		t.Fatal("public key accepted as a credential", w.Code)
	}
	r = openRequest(m, body)
	r.Header.Set("Authorization", "Bearer synthetic-test-only")
	w = httptest.NewRecorder()
	h.ServeHTTP(w, r)
	if w.Code != 400 {
		t.Fatal(w.Code)
	}

	// Assigned but not yet published IDs are protected, including case variants.
	openPublish(t, h, meta(), body, 403)
	assigned := meta()
	assigned.ID = "HELLO"
	openPublish(t, h, assigned, body, 403)
	if w := publishRequest(t, h, meta(), strings.Repeat("b", 40), body); w.Code != 403 {
		t.Fatal(w.Code)
	}
	if w := publishRequest(t, h, meta(), strings.Repeat("a", 40), body); w.Code != 201 {
		t.Fatal(w.Code)
	}
	c.Publishers = nil
	h = NewServer(c, s)
	assigned = meta()
	assigned.Version = "0.2.0"
	openPublish(t, h, assigned, body, 403) // Removing config ownership does not expose legacy apps.

	openPublish(t, h, m, body, 201)
	seq := s.Catalog().Sequence
	openPublish(t, h, m, body, 201)
	if s.Catalog().Sequence != seq {
		t.Fatal("retry changed sequence")
	}
	changed := m
	changed.Name = "Changed"
	openPublish(t, h, changed, body, 409)
	changed = m
	changed.Version = "0.0.9"
	openPublish(t, h, changed, body, 409)
	changed = m
	changed.ID = "SELF-SERVICE"
	openPublish(t, h, changed, body, 409)
	m.Version = "0.1.1"
	openPublish(t, h, m, body, 201) // No owner secret: any anonymous developer may update.
	for _, p := range s.Catalog().Packages {
		if p.ID == m.ID && p.Author != OpenPublisherName {
			t.Fatal(p.Author)
		}
	}
	for _, v := range []int{1, 2} {
		if !ed25519.Verify(s.public, s.Index(v, false), s.Index(v, true)) {
			t.Fatal("bad signature")
		}
	}
	if !bytes.Contains(s.Index(2, false), []byte("\t"+OpenPublisherName+"\n")) {
		t.Fatal("unsigned anonymous provenance")
	}

	s.Close()
	reopened, err := Open(c)
	if err != nil {
		t.Fatal(err)
	}
	defer reopened.Close()
	m.Version = "0.1.2"
	openPublish(t, NewServer(c, reopened), m, body, 201)
	openPublish(t, NewServer(c, reopened), assigned, body, 403)
	c.PublishMode = "token"
	openPublish(t, NewServer(c, reopened), m, body, 403)
}

func TestOpenPublishingValidationAndLimits(t *testing.T) {
	c, s := fixture(t)
	c.PublishMode = "open"
	h := NewServer(c, s)
	m := meta()
	m.ID = "new-app"
	body := goodPayload(t)
	for _, id := range []string{"c1pkg", "C1ancher", "C1UPDATER", "app_daemon", "../escape"} {
		bad := m
		bad.ID = id
		openPublish(t, h, bad, body, 400)
	}
	bad := m
	bad.Version = "1.01.0"
	openPublish(t, h, bad, body, 400)
	bad = m
	bad.Mode = "invalid"
	openPublish(t, h, bad, body, 400)
	openPublish(t, h, m, []byte("not gzip"), 400)
	for _, header := range []*tar.Header{
		{Name: "../escape", Typeflag: tar.TypeReg},
		{Name: "hello", Typeflag: tar.TypeSymlink, Linkname: "/etc/passwd"},
	} {
		openPublish(t, h, m, payload(t, []*tar.Header{header}, nil), 400)
	}
	openPublish(t, h, m, payload(t, []*tar.Header{{Name: "hello", Typeflag: tar.TypeReg, Size: 3}}, [][]byte{[]byte("bad")}), 400)
	// A hostile tar size is rejected before allocating the claimed file.
	var oversized bytes.Buffer
	gz := gzip.NewWriter(&oversized)
	tw := tar.NewWriter(gz)
	if err := tw.WriteHeader(&tar.Header{Name: "hello", Typeflag: tar.TypeReg, Size: MaxFile + 1}); err != nil {
		t.Fatal(err)
	}
	if err := gz.Close(); err != nil {
		t.Fatal(err)
	}
	openPublish(t, h, m, oversized.Bytes(), 400)
	r := openRequest(m, body)
	r.ContentLength = MaxArchive + 1
	w := httptest.NewRecorder()
	h.ServeHTTP(w, r)
	if w.Code != 413 {
		t.Fatal(w.Code)
	}
	h.uploads <- struct{}{}
	w = httptest.NewRecorder()
	h.ServeHTTP(w, openRequest(m, body))
	<-h.uploads
	if w.Code != 503 || w.Header().Get("Retry-After") != "5" {
		t.Fatal("upload gate bypass")
	}
	s.maxBytes = 1
	openPublish(t, h, m, body, 500)
	if s.Catalog().Sequence != 40 {
		t.Fatal("rejected upload changed catalog")
	}
	s.key = nil
	openPublish(t, h, m, body, 503)
	w = httptest.NewRecorder()
	h.ServeHTTP(w, httptest.NewRequest("POST", "/c1/core/v1/stable/manifest.v1", bytes.NewReader(body)))
	if w.Code != 405 {
		t.Fatal("core upload route exposed")
	}
}

func TestOpenCatalogLimit(t *testing.T) {
	_, s := fixture(t)
	m := meta()
	for i := 0; i < MaxPackages; i++ {
		m.ID = fmt.Sprintf("open-%d", i)
		if _, _, err := s.PublishOpen(m, []byte("same synthetic stored object")); err != nil {
			t.Fatal(err)
		}
	}
	m.ID = "one-too-many"
	if _, _, err := s.PublishOpen(m, []byte("same synthetic stored object")); err == nil {
		t.Fatal("catalog limit bypass")
	}
}

func TestPublishModeConfig(t *testing.T) {
	for _, mode := range []string{"", "token", "open", "OPEN", "unknown"} {
		c := Config{Root: "unused", Listen: "127.0.0.1:0", PublicKey: "unused", Downloads: 1, BytesPerSecond: 1024, PublishMode: mode}
		data, _ := json.Marshal(c)
		file := filepath.Join(t.TempDir(), "config.json")
		if err := os.WriteFile(file, data, 0600); err != nil {
			t.Fatal(err)
		}
		_, err := LoadConfig(file)
		if valid := mode == "" || mode == "token" || mode == "open"; (err == nil) != valid {
			t.Fatalf("mode %q: %v", mode, err)
		}
	}
}
