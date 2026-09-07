package main

import (
	"crypto/ed25519"
	"crypto/rand"
	"net/http"
	"net/http/httptest"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

// Opt-in smoke test for the actual distributed executable. All trust material
// is synthetic and the signing key remains exclusively in test-process memory.
func TestDistributedBinaryPublicConfig(t *testing.T) {
	binary := os.Getenv("C1PUBLISH_TEST_BINARY")
	if binary == "" {
		t.Skip("set C1PUBLISH_TEST_BINARY to test a local built artifact")
	}
	public, private, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	data := []byte("C1PKG-INDEX 2\nS\t40\n")
	sig := ed25519.Sign(private, data)
	posts := 0
	tamper := false
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if _, present := r.Header["Authorization"]; present {
			t.Error("sent credentials")
		}
		switch r.URL.Path {
		case "/c1/v2/index.v1":
			_, _ = w.Write(data)
		case "/c1/v2/index.v1.sig":
			if tamper {
				_, _ = w.Write(make([]byte, 64))
			} else {
				_, _ = w.Write(sig)
			}
		case "/api/v1/publish":
			posts++
			if r.Header.Get("X-C1-Publish-Mode") != "open" {
				t.Error("missing anonymous mode")
			}
			w.WriteHeader(201)
			_, _ = w.Write([]byte(`{"sequence":41}`))
		default:
			t.Error("unexpected route", r.URL.Path)
			http.NotFound(w, r)
		}
	}))
	defer server.Close()
	dir := t.TempDir()
	name := "c1publish"
	if runtime.GOOS == "windows" {
		name += ".exe"
	}
	executable := filepath.Join(dir, name)
	body, err := os.ReadFile(binary)
	if err != nil {
		t.Fatal(err)
	}
	for name, data := range map[string][]byte{name: body, "server.url": []byte(server.URL + "\n"), "repository.ed25519.pub": public, "hello": deviceELF()} {
		if err := os.WriteFile(filepath.Join(dir, name), data, 0700); err != nil {
			t.Fatal(err)
		}
	}
	run := func(args ...string) (string, error) {
		program := executable
		if runner := os.Getenv("C1PUBLISH_TEST_RUNNER"); runner != "" {
			program = runner
			args = append([]string{executable}, args...)
		}
		cmd := exec.Command(program, args...)
		cmd.Dir = t.TempDir() // No configuration in CWD: must discover executable siblings.
		out, err := cmd.CombinedOutput()
		return string(out), err
	}
	if out, err := run("-open", "-id", "new-app", "-next-version"); err != nil || strings.TrimSpace(out) != "0.1.0" {
		t.Fatal(out, err)
	}
	args := []string{"-open", "-id", "new-app", "-name", "New App", "-version", "0.1.0", "-binary", filepath.Join(dir, "hello")}
	if out, err := run(args...); err != nil {
		t.Fatal(out, err)
	}
	if posts != 1 {
		t.Fatal(posts)
	}
	tamper = true
	if out, err := run(args...); err == nil || !strings.Contains(out, "signature rejected") {
		t.Fatal(out, err)
	}
	if posts != 1 {
		t.Fatal("tampered index still permitted upload")
	}
}
