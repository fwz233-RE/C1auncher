package main

import (
	"bytes"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
	"unicode/utf8"
)

const syntheticToken = "synthetic-test-only-token-never-a-real-credential-12345"

func TestRegistrationTokenPersistsAndNeverOverwrites(t *testing.T) {
	path := filepath.Join(t.TempDir(), "publisher.token")
	first, err := registrationToken(path)
	if err != nil {
		t.Fatal(err)
	}
	raw, err := base64.RawURLEncoding.DecodeString(first)
	if err != nil || len(raw) != 32 {
		t.Fatal("expected a 256-bit random token")
	}
	before, err := os.ReadFile(path)
	if err != nil || string(before) != first+"\n" {
		t.Fatal("token not persisted")
	}
	second, err := registrationToken(path)
	after, readErr := os.ReadFile(path)
	if err != nil || readErr != nil || second != first || !bytes.Equal(before, after) {
		t.Fatal("retry changed token")
	}
	info, err := os.Stat(path)
	if err != nil {
		t.Fatal(err)
	}
	if runtime.GOOS != "windows" && info.Mode().Perm() != 0600 {
		t.Fatal("token permissions are not private")
	}
	other, err := registrationToken(filepath.Join(t.TempDir(), "publisher.token"))
	if err != nil || other == first {
		t.Fatal("independent registrations reused a token")
	}
}

func TestRegistrationRejectsInvalidFilesWithoutReplacing(t *testing.T) {
	for _, data := range []string{"", "short", strings.Repeat("x", 1025), strings.Repeat("x", 32) + "\nembedded", strings.Repeat("x", 32) + "\x7f"} {
		t.Run(fmt.Sprintf("size-%d", len(data)), func(t *testing.T) {
			path := filepath.Join(t.TempDir(), "publisher.token")
			if err := os.WriteFile(path, []byte(data), 0600); err != nil {
				t.Fatal(err)
			}
			if _, err := registrationToken(path); err == nil {
				t.Fatal("invalid existing token accepted")
			}
			after, err := os.ReadFile(path)
			if err != nil || string(after) != data {
				t.Fatal("existing token was replaced")
			}
		})
	}
	if _, err := registrationToken(t.TempDir()); err == nil {
		t.Fatal("directory accepted as token")
	}
}

func TestTokenSymlinkRejected(t *testing.T) {
	dir := t.TempDir()
	original, link := filepath.Join(dir, "original"), filepath.Join(dir, "link")
	if err := os.WriteFile(original, []byte(syntheticToken), 0600); err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink(original, link); err != nil {
		t.Skip("symlink creation not permitted on this platform")
	}
	if _, err := registrationToken(link); err == nil {
		t.Fatal("symlink credential accepted")
	}
}

func TestRegistrationLostResponseRetriesPersistedCredential(t *testing.T) {
	path := filepath.Join(t.TempDir(), "publisher.token")
	calls := 0
	var first string
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		calls++
		if r.Method != http.MethodPost || r.URL.Path != "/api/v1/publishers/register" || r.Header.Get("Content-Type") != "application/json" {
			t.Error("unexpected registration request")
		}
		saved, err := readToken(path)
		if err != nil || r.Header.Get("Authorization") != "Bearer "+saved {
			t.Error("credential was not saved before transmission")
		}
		var body struct {
			Name string `json:"name"`
		}
		if json.NewDecoder(r.Body).Decode(&body) != nil || body.Name != "测试作者" {
			t.Error("wrong author")
		}
		if calls == 1 {
			first = saved
			// Simulate a committed registration whose reply is lost.
			conn, _, err := w.(http.Hijacker).Hijack()
			if err != nil {
				t.Error(err)
				return
			}
			conn.Close()
			return
		}
		if saved != first {
			t.Error("retry replaced credential")
		}
		_, _ = io.WriteString(w, `{"name":"测试作者"}`)
	}))
	defer server.Close()
	var out bytes.Buffer
	if err := registerPublisherWithOutput(server.Client(), server.URL, "测试作者", path, &out); err == nil || !strings.Contains(err.Error(), "retained") {
		t.Fatal("lost response should retain credential and explain retry")
	}
	if err := registerPublisherWithOutput(server.Client(), server.URL, "测试作者", path, &out); err != nil {
		t.Fatal(err)
	}
	if calls != 2 || !strings.Contains(out.String(), "测试作者") || strings.Contains(out.String(), first) {
		t.Fatal("incorrect retry confirmation or exposed credential")
	}
}

func TestRegistrationConflictsFailuresAndConfirmationsNeverEchoSecrets(t *testing.T) {
	for _, status := range []int{200, 201, 400, 401, 403, 409, 429, 500} {
		t.Run(fmt.Sprint(status), func(t *testing.T) {
			path := filepath.Join(t.TempDir(), "publisher.token")
			if err := os.WriteFile(path, []byte(syntheticToken), 0600); err != nil {
				t.Fatal(err)
			}
			calls := 0
			server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				calls++
				w.WriteHeader(status)
				// Test both raw and encoded credential reflection and terminal controls.
				_ = json.NewEncoder(w).Encode(map[string]string{"name": "Author", "detail": syntheticToken + "\x1b[2J" + base64.StdEncoding.EncodeToString([]byte(syntheticToken))})
			}))
			defer server.Close()
			var out bytes.Buffer
			err := registerPublisherWithOutput(server.Client(), server.URL, "Author", path, &out)
			if status == 200 || status == 201 {
				if err != nil {
					t.Fatal(err)
				}
			} else if err == nil || !strings.Contains(err.Error(), fmt.Sprint(status)) || !strings.Contains(err.Error(), "retained") {
				t.Fatal("missing safe HTTP failure and retention guidance")
			}
			text := out.String()
			if err != nil {
				text += err.Error()
			}
			if strings.Contains(text, syntheticToken) || strings.Contains(text, base64.StdEncoding.EncodeToString([]byte(syntheticToken))) || strings.Contains(text, "\x1b") {
				t.Fatal("server reflection leaked to output")
			}
			if saved, err := readToken(path); err != nil || saved != syntheticToken || calls != 1 {
				t.Fatal("failure replaced token or retried automatically")
			}
		})
	}
}

func TestRegistrationInvalidConfirmationsRetainToken(t *testing.T) {
	for _, body := range []string{`{`, `{"name":"Other"}`, `{"name":"Author"} trailing`, strings.Repeat("x", 8193)} {
		server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			w.WriteHeader(201)
			_, _ = io.WriteString(w, body)
		}))
		path := filepath.Join(t.TempDir(), "publisher.token")
		err := registerPublisherWithOutput(server.Client(), server.URL, "Author", path, io.Discard)
		server.Close()
		if err == nil || !strings.Contains(err.Error(), "retained") {
			t.Fatal("bad confirmation accepted")
		}
		if _, err := readToken(path); err != nil {
			t.Fatal("token missing after bad confirmation")
		}
	}
}

func TestRegistrationValidationSendsNothing(t *testing.T) {
	client := &http.Client{Transport: testTransport(func(*http.Request) (*http.Response, error) {
		t.Error("unexpected network request")
		return nil, errors.New("must not send")
	})}
	for _, author := range []string{"", "Anonymous (open)", "anonymous (OPEN)", " Author", "A\nB", "A\u200bB", strings.Repeat("测", 14)} {
		path := filepath.Join(t.TempDir(), "publisher.token")
		if err := registerPublisherWithOutput(client, "http://127.0.0.1", author, path, io.Discard); err == nil {
			t.Fatal("invalid author accepted")
		}
		if _, err := os.Stat(path); !os.IsNotExist(err) {
			t.Fatal("invalid author created token")
		}
	}
	path := filepath.Join(t.TempDir(), "missing", "publisher.token")
	if err := registerPublisherWithOutput(client, "http://127.0.0.1", "Author", path, io.Discard); err == nil {
		t.Fatal("unwritable path accepted")
	}
}

func TestRegistrationCLIValidationAndUTF8Help(t *testing.T) {
	dir := t.TempDir()
	for _, args := range [][]string{
		{"-register"},
		{"-register", "-author", "Author", "-token-file", "unused", "-open"},
		{"-register", "-author", "Author", "-token-file", "unused", "-next-version"},
		{"-register", "-author", "Author", "-token-file", "unused", "-binary", "unused"},
		{"-author", "Author", "-token-file", "unused"},
		{"-register", "-author", "Author", "-token-file", "unused", "-server", "http://example.invalid"},
	} {
		if err := runArgsWithOutput(args, dir, io.Discard); err == nil {
			t.Fatal("invalid arguments accepted")
		}
	}
	var out bytes.Buffer
	if err := runArgsWithOutput([]string{"-help"}, dir, &out); err != nil {
		t.Fatal(err)
	}
	if !utf8.Valid(out.Bytes()) || !strings.Contains(out.String(), "1-40 UTF-8 bytes") || !strings.Contains(out.String(), "-register") || !strings.Contains(out.String(), "retry") || strings.Contains(out.String(), "printable ASCII") {
		t.Fatal("help is incomplete or inaccurate")
	}
}
