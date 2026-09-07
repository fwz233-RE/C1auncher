package main

import (
	"bytes"
	"c1repo/internal/repository"
	"crypto/rand"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"runtime"
	"strings"
)

func validateAuthor(name string) error {
	// Reuse the device label rules (UTF-8, 40 bytes, no invisible controls).
	m := repository.Metadata{ID: "author-check", Version: "0.1.0", Name: name, Entry: "app"}
	if m.Validate() != nil || strings.EqualFold(name, repository.OpenPublisherName) {
		return errors.New("author must be a valid 1-40 byte display name; Anonymous (open) is reserved")
	}
	return nil
}

func readToken(path string) (string, error) {
	info, err := os.Lstat(path)
	if err != nil || !info.Mode().IsRegular() || info.Size() > 1024 {
		return "", errors.New("cannot read token file: expected a small regular file, not a link")
	}
	f, err := os.Open(path)
	if err != nil {
		return "", errors.New("cannot read token file")
	}
	defer f.Close()
	opened, err := f.Stat()
	if err != nil || !opened.Mode().IsRegular() || !os.SameFile(info, opened) {
		return "", errors.New("token file changed while opening; no credentials were sent")
	}
	b, err := io.ReadAll(io.LimitReader(f, 1025))
	if err != nil || len(b) > 1024 {
		return "", errors.New("cannot read token file")
	}
	token := strings.TrimSpace(string(b))
	if len(token) < 32 || len(token) > 256 {
		return "", errors.New("invalid publisher token")
	}
	for _, c := range token {
		if c < 33 || c > 126 {
			return "", errors.New("invalid publisher token")
		}
	}
	return token, nil
}

// Persist a cryptographically random token before registering it. Never replace
// an existing credential: retries reuse it, including after a lost response.
func registrationToken(path string) (string, error) {
	if _, err := os.Lstat(path); !os.IsNotExist(err) {
		return reuseRegistrationToken(path)
	}
	// Generate before opening so an entropy failure cannot leave an empty file.
	var raw [32]byte
	if _, err := rand.Read(raw[:]); err != nil {
		return "", errors.New("secure token generation failed")
	}
	f, err := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0600)
	if os.IsExist(err) {
		return reuseRegistrationToken(path)
	}
	if err != nil {
		return "", errors.New("cannot create token file; choose a writable private directory")
	}
	complete := false
	defer func() {
		f.Close()
		if !complete {
			// No request can have been sent. Only remove our incomplete creation.
			os.Remove(path)
		}
	}()
	token := base64.RawURLEncoding.EncodeToString(raw[:])
	if _, err = f.WriteString(token + "\n"); err != nil {
		return "", errors.New("cannot save token; no registration was sent")
	}
	complete = true
	if err = f.Sync(); err != nil {
		return "", errors.New("cannot sync token; no registration was sent")
	}
	if err = f.Close(); err != nil {
		return "", errors.New("cannot close token; no registration was sent")
	}
	if err = syncTokenDirectory(path); err != nil {
		return "", err
	}
	return token, nil
}

// A previous attempt may have failed during Sync after writing a complete
// token. Flush it again before any retry instead of silently using that file.
func reuseRegistrationToken(path string) (string, error) {
	token, err := readToken(path)
	if err != nil {
		return "", err
	}
	f, err := os.OpenFile(path, os.O_RDWR, 0)
	if err != nil {
		return "", errors.New("cannot sync existing token; registration retries require a writable private token file")
	}
	defer f.Close()
	if err = f.Sync(); err != nil {
		return "", errors.New("cannot sync token; no registration was sent")
	}
	if err = f.Close(); err != nil {
		return "", errors.New("cannot close token; no registration was sent")
	}
	if err = syncTokenDirectory(path); err != nil {
		return "", err
	}
	return token, nil
}

func syncTokenDirectory(path string) error {
	// File.Sync uses FlushFileBuffers on Windows; Go cannot portably flush
	// Windows directory handles. Windows permissions inherit the parent ACL,
	// so the wizard stores tokens in the current user's configuration folder.
	if runtime.GOOS == "windows" {
		return nil
	}
	dir, err := os.Open(filepath.Dir(path))
	if err != nil {
		return errors.New("cannot open token directory for sync; no registration was sent")
	}
	defer dir.Close()
	if err = dir.Sync(); err != nil {
		return errors.New("cannot sync token directory; no registration was sent")
	}
	return nil
}

func registerPublisher(client *http.Client, origin, author, tokenPath string) error {
	return registerPublisherWithOutput(client, origin, author, tokenPath, os.Stdout)
}

func registerPublisherWithOutput(client *http.Client, origin, author, tokenPath string, output io.Writer) error {
	if err := validateAuthor(author); err != nil {
		return err
	}
	secret, err := registrationToken(tokenPath)
	if err != nil {
		return err
	}
	body, _ := json.Marshal(struct {
		Name string `json:"name"`
	}{author})
	req, err := http.NewRequest(http.MethodPost, origin+"/api/v1/publishers/register", bytes.NewReader(body))
	if err != nil {
		return err
	}
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("Authorization", "Bearer "+secret)
	resp, err := client.Do(req)
	if err != nil {
		return errors.New("registration response unavailable; token file retained: retry the same author and file, never generate a replacement")
	}
	defer resp.Body.Close()
	data, err := io.ReadAll(io.LimitReader(resp.Body, 8193))
	if err != nil || len(data) > 8192 {
		return errors.New("invalid registration response; token file retained for retry")
	}
	if resp.StatusCode != http.StatusCreated && resp.StatusCode != http.StatusOK {
		return publisherHTTPError("registration", resp.StatusCode)
	}
	var result struct {
		Name string `json:"name"`
	}
	if json.Unmarshal(data, &result) != nil || result.Name != author {
		return errors.New("invalid registration confirmation; token file retained for retry")
	}
	fmt.Fprintf(output, "Registered author: %s\nToken saved in: %s\nKeep and back up this file privately. Publish with -token-file; new unassigned application IDs are automatically yours after successful publication.\n", result.Name, tokenPath)
	return nil
}

// Remote bodies may echo a token in raw, encoded, or escaped form. Report only
// locally defined messages instead of attempting incomplete string redaction.
func publisherHTTPError(operation string, status int) error {
	message := "request rejected"
	switch status {
	case http.StatusBadRequest:
		message = "invalid request; check the author name or application metadata and payload"
	case http.StatusNotFound, http.StatusMethodNotAllowed:
		message = "endpoint unavailable; check the server origin and whether this server supports self-service publishing"
	case http.StatusRequestEntityTooLarge:
		message = "request exceeds the server size limit; reduce the payload before retrying"
	case http.StatusUnauthorized:
		message = "token rejected; use the original registered token file"
	case http.StatusForbidden:
		message = "publishing or registration is disabled, or this token is not allowed to perform the operation"
	case http.StatusConflict:
		if operation == "registration" {
			message = "author name or token is already registered differently; keep this file; use the original author for a retry, or another author if the name is taken"
		} else {
			message = "application ID belongs to another publisher, or this version conflicts; update your own ID with its original token and a higher version; a lost response can be retried with the identical version and payload"
		}
	case http.StatusTooManyRequests:
		message = "rate limited; wait before retrying with the same token file"
	default:
		if status >= 500 {
			message = "server unavailable; retry the same request with the same token file"
		}
	}
	if operation == "registration" {
		message += "; token file retained"
	}
	return fmt.Errorf("%s HTTP %d: %s", operation, status, message)
}
