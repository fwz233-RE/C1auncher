package main

import (
	"bytes"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func publisherFixture(t *testing.T) (directory, token, binary string) {
	t.Helper()
	directory = t.TempDir()
	token, binary = filepath.Join(directory, "publisher.token"), filepath.Join(directory, "app")
	if err := os.WriteFile(token, []byte(syntheticToken), 0600); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(binary, deviceELF(), 0600); err != nil {
		t.Fatal(err)
	}
	return
}

func TestAuthenticatedPublishNewIDsUpdatesAndIdenticalRetry(t *testing.T) {
	dir, token, binary := publisherFixture(t)
	var requests [][]byte
	var versions []string
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost || r.URL.Path != "/api/v1/publish" || r.Header.Get("Authorization") != "Bearer "+syntheticToken || r.Header.Get("X-C1-Publish-Mode") != "" {
			t.Error("wrong authenticated publication request")
		}
		metadata, err := base64.RawURLEncoding.DecodeString(r.Header.Get("X-C1-Metadata"))
		var m struct{ ID, Version, Name string }
		if err != nil || json.Unmarshal(metadata, &m) != nil || m.ID != "own-new-id" || m.Name != "中文应用" {
			t.Error("invalid metadata")
		}
		versions = append(versions, m.Version)
		body, err := io.ReadAll(r.Body)
		if err != nil {
			t.Error(err)
		}
		requests = append(requests, body)
		if len(requests) == 1 {
			// A committed upload with a lost reply must be retried unchanged.
			conn, _, err := w.(http.Hijacker).Hijack()
			if err != nil {
				t.Error(err)
				return
			}
			conn.Close()
			return
		}
		w.WriteHeader(201)
		_, _ = io.WriteString(w, `{"sequence":42}`)
	}))
	defer server.Close()
	for i, version := range []string{"0.1.0", "0.1.0", "0.1.1"} {
		args := []string{"-server", server.URL, "-token-file", token, "-id", "own-new-id", "-name", "中文应用", "-version", version, "-binary", binary}
		err := runArgsWithOutput(args, dir, io.Discard)
		if i == 0 {
			if err == nil || !strings.Contains(err.Error(), "retry the same version and payload") {
				t.Fatal("missing lost-upload retry guidance")
			}
		} else if err != nil {
			t.Fatal(err)
		}
	}
	if len(requests) != 3 || !bytes.Equal(requests[0], requests[1]) || strings.Join(versions, ",") != "0.1.0,0.1.0,0.1.1" {
		t.Fatal("retry changed payload or update lost version")
	}
}

func TestPublishResponseNeverEchoesToken(t *testing.T) {
	for _, status := range []int{201, 401, 403, 409, 429, 503} {
		t.Run(fmt.Sprint(status), func(t *testing.T) {
			dir, token, binary := publisherFixture(t)
			calls := 0
			server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				calls++
				w.WriteHeader(status)
				_, _ = io.WriteString(w, syntheticToken+"\x1b[2J"+base64.StdEncoding.EncodeToString([]byte(syntheticToken)))
			}))
			defer server.Close()
			var out bytes.Buffer
			err := runArgsWithOutput([]string{"-server", server.URL, "-token-file", token, "-id", "own-app", "-name", "App", "-version", "0.1.0", "-binary", binary}, dir, &out)
			if status == 201 {
				if err != nil || !strings.Contains(out.String(), "Published own-app version 0.1.0") {
					t.Fatal("missing safe upload confirmation")
				}
			} else if err == nil || !strings.Contains(err.Error(), fmt.Sprintf("HTTP %d", status)) {
				t.Fatal("missing HTTP status")
			}
			text := out.String()
			if err != nil {
				text += err.Error()
			}
			if strings.Contains(text, syntheticToken) || strings.Contains(text, "\x1b") || strings.Contains(text, base64.StdEncoding.EncodeToString([]byte(syntheticToken))) {
				t.Fatal("reflected response leaked")
			}
			if calls != 1 {
				t.Fatal("unexpected automatic retry")
			}
		})
	}
}

func TestPublishRejectsTokenInPayloadIncludingHardlink(t *testing.T) {
	for _, hardlink := range []bool{false, true} {
		t.Run(fmt.Sprint(hardlink), func(t *testing.T) {
			dir, token, binary := publisherFixture(t)
			payload := dir
			if hardlink {
				payload = t.TempDir()
				if err := os.Link(token, filepath.Join(payload, "private-copy")); err != nil {
					t.Skip("hardlinks unavailable")
				}
				if err := os.WriteFile(filepath.Join(payload, "app"), deviceELF(), 0600); err != nil {
					t.Fatal(err)
				}
			}
			calls := 0
			server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { calls++; w.WriteHeader(201) }))
			defer server.Close()
			args := []string{"-server", server.URL, "-token-file", token, "-id", "own-app", "-name", "App", "-version", "0.1.0", "-payload", payload, "-entry", filepath.Base(binary)}
			err := runArgsWithOutput(args, dir, io.Discard)
			if err == nil || !strings.Contains(err.Error(), "payload contains your publisher token") || calls != 0 {
				t.Fatal("token file could be uploaded")
			}
		})
	}
}

func TestPublisherTruncatedResponsesRetainToken(t *testing.T) {
	for _, registration := range []bool{true, false} {
		t.Run(fmt.Sprint(registration), func(t *testing.T) {
			dir, token, binary := publisherFixture(t)
			server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				w.Header().Set("Content-Length", "999")
				w.WriteHeader(http.StatusCreated)
				_, _ = io.WriteString(w, syntheticToken)
			}))
			defer server.Close()
			args := []string{"-server", server.URL, "-token-file", token}
			if registration {
				args = append(args, "-register", "-author", "Author")
			} else {
				args = append(args, "-id", "own-app", "-name", "App", "-version", "0.1.0", "-binary", binary)
			}
			var output bytes.Buffer
			err := runArgsWithOutput(args, dir, &output)
			if err == nil || !strings.Contains(err.Error(), "retry") || strings.Contains(err.Error()+output.String(), syntheticToken) {
				t.Fatal("truncated reply did not return a credential-safe retry error")
			}
			if saved, err := readToken(token); err != nil || saved != syntheticToken {
				t.Fatal("truncated reply lost the existing token")
			}
		})
	}
}

func TestRegistrationRedirectNeverSendsCredentialsToTarget(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "publisher.token")
	targetCalls := 0
	target := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { targetCalls++; w.WriteHeader(201) }))
	defer target.Close()
	source := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		http.Redirect(w, r, target.URL, http.StatusTemporaryRedirect)
	}))
	defer source.Close()
	err := runArgsWithOutput([]string{"-server", source.URL, "-register", "-author", "Author", "-token-file", path}, dir, io.Discard)
	if err == nil || targetCalls != 0 {
		t.Fatal("redirect followed")
	}
	if _, err := readToken(path); err != nil {
		t.Fatal("redirect failure lost token")
	}
}
