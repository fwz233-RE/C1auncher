package main

import (
	"crypto/ed25519"
	"crypto/rand"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestSiblingPublicConfigAndSignedCatalog(t *testing.T) {
	public, private, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	} // Synthetic signing material remains only in memory.
	index := []byte("C1PKG-INDEX 2\nS\t40\n")
	signature := ed25519.Sign(private, index)
	tamper := false
	posts := 0
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if _, present := r.Header["Authorization"]; present {
			t.Error("public key used as credentials")
		}
		switch r.URL.Path {
		case "/c1/v2/index.v1":
			if tamper {
				_, _ = w.Write(append(append([]byte(nil), index...), 'x'))
			} else {
				_, _ = w.Write(index)
			}
		case "/c1/v2/index.v1.sig":
			_, _ = w.Write(signature)
		case "/api/v1/publish":
			posts++
			if r.Header.Get("X-C1-Publish-Mode") != "open" {
				t.Error("missing explicit open mode")
			}
			w.WriteHeader(201)
			_, _ = w.Write([]byte(`{"sequence":41}`))
		default:
			t.Error("unexpected path", r.URL.Path)
			http.NotFound(w, r)
		}
	}))
	defer server.Close()
	dir := t.TempDir()
	for name, data := range map[string][]byte{"server.url": []byte("\ufeff" + server.URL + "\r\n"), "repository.ed25519.pub": public, "hello": deviceELF()} {
		if err := os.WriteFile(filepath.Join(dir, name), data, 0600); err != nil {
			t.Fatal(err)
		}
	}
	if err := runArgsInDir([]string{"-open", "-id", "new-app", "-next-version"}, dir); err != nil {
		t.Fatal(err)
	}
	args := []string{"-open", "-id", "new-app", "-version", "0.1.0", "-name", "New App", "-binary", filepath.Join(dir, "hello")}
	if err := runArgsInDir(args, dir); err != nil {
		t.Fatal(err)
	}
	if posts != 1 {
		t.Fatal(posts)
	}
	tamper = true
	if err := runArgsInDir(args, dir); err == nil || !strings.Contains(err.Error(), "signature rejected") {
		t.Fatal(err)
	}
	if posts != 1 {
		t.Fatal("uploaded after failed verification")
	}
	if err := runArgsInDir([]string{"-open", "-id", "new-app", "-next-version"}, dir); err == nil {
		t.Fatal("accepted tampered version source")
	}
}

func TestPublicConfigFailureAndOverrides(t *testing.T) {
	dir := t.TempDir()
	origin, public, err := loadPublicConfig(dir, "", "")
	if err != nil || origin != "http://www.fwz233.com" || len(public) != 0 {
		t.Fatal(origin, err)
	}
	if err := os.WriteFile(filepath.Join(dir, "server.url"), []byte("https://configured.invalid\n"), 0600); err != nil {
		t.Fatal(err)
	}
	if _, _, err := loadPublicConfig(dir, "", ""); err == nil {
		t.Fatal("sidecar config silently ignored missing public key")
	}
	if err := os.WriteFile(filepath.Join(dir, "repository.ed25519.pub"), []byte("not a public key"), 0600); err != nil {
		t.Fatal(err)
	}
	if _, _, err := loadPublicConfig(dir, "", ""); err == nil {
		t.Fatal("accepted malformed public key")
	}
	if err := os.WriteFile(filepath.Join(dir, "repository.ed25519.pub"), make([]byte, 32), 0600); err != nil {
		t.Fatal(err)
	}
	origin, public, err = loadPublicConfig(dir, "https://override.invalid", "")
	if err != nil || origin != "https://override.invalid" || len(public) != 32 {
		t.Fatal(origin, err)
	}
	if err := os.WriteFile(filepath.Join(dir, "server.url"), []byte("https://one.invalid\nhttps://two.invalid"), 0600); err != nil {
		t.Fatal(err)
	}
	if _, _, err := loadPublicConfig(dir, "", ""); err == nil {
		t.Fatal("accepted multiple origins")
	}
}
