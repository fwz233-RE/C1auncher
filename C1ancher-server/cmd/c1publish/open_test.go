package main

import (
	"c1repo/internal/repository"
	"encoding/base64"
	"encoding/binary"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// Minimal synthetic device ELF; never executed and contains no credentials.
func deviceELF() []byte {
	names := []byte("\x00.shstrtab\x00.MIPS.abiflags\x00")
	abiOffset := 204 + len(names)
	b := make([]byte, abiOffset+24)
	copy(b, []byte{127, 'E', 'L', 'F', 1, 1, 1})
	binary.LittleEndian.PutUint16(b[16:], 2)
	binary.LittleEndian.PutUint16(b[18:], 8)
	binary.LittleEndian.PutUint32(b[20:], 1)
	binary.LittleEndian.PutUint32(b[28:], 52)
	binary.LittleEndian.PutUint32(b[36:], 0x70001007)
	binary.LittleEndian.PutUint16(b[40:], 52)
	binary.LittleEndian.PutUint16(b[42:], 32)
	binary.LittleEndian.PutUint16(b[44:], 1)
	binary.LittleEndian.PutUint32(b[52:], 1)
	binary.LittleEndian.PutUint32(b[76:], 5)
	binary.LittleEndian.PutUint32(b[32:], 84)
	binary.LittleEndian.PutUint16(b[46:], 40)
	binary.LittleEndian.PutUint16(b[48:], 3)
	binary.LittleEndian.PutUint16(b[50:], 1)
	binary.LittleEndian.PutUint32(b[124:], 1)
	binary.LittleEndian.PutUint32(b[128:], 3)
	binary.LittleEndian.PutUint32(b[140:], 204)
	binary.LittleEndian.PutUint32(b[144:], uint32(len(names)))
	binary.LittleEndian.PutUint32(b[164:], 11)
	binary.LittleEndian.PutUint32(b[168:], 0x7000002a)
	binary.LittleEndian.PutUint32(b[180:], uint32(abiOffset))
	binary.LittleEndian.PutUint32(b[184:], 24)
	copy(b[204:], names)
	copy(b[abiOffset:], []byte{0, 0, 32, 2, 1, 1, 0, 1})
	return b
}

func TestOpenCLIUploadsWithoutCredentials(t *testing.T) {
	file := filepath.Join(t.TempDir(), "hello")
	if err := os.WriteFile(file, deviceELF(), 0600); err != nil {
		t.Fatal(err)
	}
	calls := 0
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		calls++
		if r.Method != "POST" || r.URL.Path != "/api/v1/publish" || r.Header.Get("X-C1-Publish-Mode") != "open" {
			t.Error("unexpected publication request")
		}
		if _, present := r.Header["Authorization"]; present {
			t.Error("open CLI sent credentials")
		}
		data, err := base64.RawURLEncoding.DecodeString(r.Header.Get("X-C1-Metadata"))
		var m repository.Metadata
		if err != nil || json.Unmarshal(data, &m) != nil || m.ID != "new-id" {
			t.Error("invalid metadata")
		}
		if _, err := repository.BuildPackage(m, r.Body); err != nil {
			t.Error(err)
		}
		w.WriteHeader(201)
		_, _ = io.WriteString(w, `{"sequence":41}`)
	}))
	defer server.Close()
	args := []string{"-server", server.URL, "-open", "-id", "new-id", "-version", "0.1.0", "-name", "New App", "-binary", file}
	if err := runArgs(args); err != nil {
		t.Fatal(err)
	}
	if calls != 1 {
		t.Fatalf("got %d requests", calls)
	}
}

func TestOpenCLIValidation(t *testing.T) {
	for _, test := range []struct {
		args    []string
		message string
	}{
		{[]string{"-open", "-token-file", "must-not-be-read"}, "mutually exclusive"},
		{nil, "provide -token-file"},
		{[]string{"-open", "-server", "http://example.invalid"}, "upload tampering"},
		{[]string{"-open", "-server", "https://example.invalid/path"}, "must be an origin"},
		{[]string{"-open", "-server", "http://127.0.0.1", "-binary", "unused", "-id", "c1pkg", "-version", "0.1.0", "-name", "Core"}, "reserved"},
	} {
		if err := runArgs(test.args); err == nil || !strings.Contains(err.Error(), test.message) {
			t.Fatalf("%v: %v", test.args, err)
		}
	}
	if err := runArgs([]string{"-help"}); err != nil {
		t.Fatal(err)
	}
}

func TestOpenCLIDoesNotRetryWithCredentialsWhenDisabled(t *testing.T) {
	file := filepath.Join(t.TempDir(), "hello")
	if err := os.WriteFile(file, deviceELF(), 0600); err != nil {
		t.Fatal(err)
	}
	calls := 0
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		calls++
		if r.Header.Get("Authorization") != "" {
			t.Error("unexpected credentials")
		}
		http.Error(w, "open publishing is disabled", 403)
	}))
	defer server.Close()
	err := runArgs([]string{"-server", server.URL, "-open", "-id", "new-id", "-version", "0.1.0", "-name", "New App", "-binary", file})
	if err == nil || !strings.Contains(err.Error(), "HTTP 403") || calls != 1 {
		t.Fatal(calls, err)
	}
}
