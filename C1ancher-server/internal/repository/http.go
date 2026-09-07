package repository

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"sync"
	"time"
)

type Server struct {
	store              *Store
	config             Config
	gate               *Gate
	bandwidth          bandwidth
	uploads            chan struct{}
	registrationMu     sync.Mutex
	registrationTime   time.Time
	registrationTokens int
}

func NewServer(c Config, s *Store) *Server {
	return &Server{store: s, config: c, gate: NewGate(c.Downloads, c.Queue), bandwidth: bandwidth{rate: c.BytesPerSecond}, uploads: make(chan struct{}, 1)}
}
func busy(w http.ResponseWriter) {
	w.Header().Set("Retry-After", "5")
	http.Error(w, "Repository busy; waiting clients should retry in 5 seconds.", http.StatusServiceUnavailable)
}
func jsonResponse(w http.ResponseWriter, code int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(code)
	_ = json.NewEncoder(w).Encode(v)
}

var objectPath = regexp.MustCompile(`^objects/[0-9a-f]{64}\.tar\.gz$`)

func (s *Server) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("X-Content-Type-Options", "nosniff")
	w.Header().Set("Cache-Control", "no-store")
	if r.URL.Path == "/api/v1/publishers/register" {
		s.registerPublisher(w, r)
		return
	}
	if r.URL.Path == "/api/v1/publish" {
		s.publish(w, r)
		return
	}
	if r.Method != http.MethodGet && r.Method != http.MethodHead {
		w.Header().Set("Allow", "GET, HEAD")
		http.Error(w, "method not allowed", 405)
		return
	}
	switch r.URL.Path {
	case "/healthz":
		jsonResponse(w, 200, map[string]any{"ok": true, "sequence": s.store.Catalog().Sequence})
		return
	case "/api/v1/catalog":
		jsonResponse(w, 200, s.store.Catalog())
		return
	case "/api/v1/download-status":
		a, q := s.gate.Stats()
		jsonResponse(w, 200, map[string]any{"active": a, "waiting": q, "limit": s.config.Downloads})
		return
	}
	if strings.HasPrefix(r.URL.Path, "/c1/core/v1/") {
		s.serveCore(w, r)
		return
	}
	version := 2
	prefix := "/c1/v2/"
	if strings.HasPrefix(r.URL.Path, "/c1/v1/") {
		version = 1
		prefix = "/c1/v1/"
	}
	if !strings.HasPrefix(r.URL.Path, prefix) {
		http.NotFound(w, r)
		return
	}
	relative := strings.TrimPrefix(r.URL.Path, prefix)
	if relative == "index.v1" || relative == "index.v1.sig" {
		b := s.store.Index(version, relative == "index.v1.sig")
		h := sha256.Sum256(b)
		w.Header().Set("ETag", "\""+hex.EncodeToString(h[:])+"\"")
		http.ServeContent(w, r, relative, time.Time{}, bytes.NewReader(b))
		return
	}
	if !objectPath.MatchString(relative) {
		http.NotFound(w, r)
		return
	}
	file, err := os.Open(filepath.Join(s.config.Root, filepath.FromSlash(relative)))
	if err != nil {
		http.NotFound(w, r)
		return
	}
	defer file.Close()
	info, err := file.Stat()
	if err != nil || !info.Mode().IsRegular() || info.Size() > MaxArchive {
		http.NotFound(w, r)
		return
	}
	w.Header().Set("Cache-Control", "public, max-age=31536000, immutable")
	w.Header().Set("ETag", "\""+strings.TrimSuffix(strings.TrimPrefix(relative, "objects/"), ".tar.gz")+"\"")
	s.serveDownload(w, r, relative, file)
}

func (s *Server) serveDownload(w http.ResponseWriter, r *http.Request, relative string, file *os.File) {
	if r.Method == http.MethodHead {
		http.ServeContent(w, r, relative, time.Time{}, file)
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), 120*time.Second)
	defer cancel()
	release, err := s.gate.Acquire(ctx)
	if err != nil {
		w.Header().Set("Cache-Control", "no-store")
		busy(w)
		return
	}
	defer release()
	// ServeContent provides range/resume support; every body write still passes
	// through the one shared bandwidth budget, including multipart ranges.
	http.ServeContent(&limitedWriter{ResponseWriter: w, ctx: r.Context(), budget: &s.bandwidth}, r, relative, time.Time{}, file)
}

var corePath = regexp.MustCompile(`^(stable|canary)/(manifest\.v1(\.sig)?|artifacts/(C1ancher|C1ancher-launcher|c1pkg|c1updater))$`)

// Core releases are signed offline and published by the separate root-owned
// core activation tool. This service has read-only access and no core key.
func (s *Server) serveCore(w http.ResponseWriter, r *http.Request) {
	relative := strings.TrimPrefix(r.URL.Path, "/c1/core/v1/")
	if s.config.CoreRoot == "" || !corePath.MatchString(relative) {
		http.NotFound(w, r)
		return
	}
	file, err := os.OpenInRoot(s.config.CoreRoot, "channels/"+relative)
	if err != nil {
		http.NotFound(w, r)
		return
	}
	defer file.Close()
	info, err := file.Stat()
	if err != nil || !info.Mode().IsRegular() || info.Size() > MaxArchive {
		http.NotFound(w, r)
		return
	}
	if strings.Contains(relative, "/manifest.") {
		if info.Size() > 64<<10 {
			http.NotFound(w, r)
			return
		}
		http.ServeContent(w, r, relative, time.Time{}, file)
		return
	}
	s.serveDownload(w, r, relative, file)
}

type limitedWriter struct {
	http.ResponseWriter
	ctx    context.Context
	budget *bandwidth
}

func (w *limitedWriter) Write(b []byte) (int, error) {
	total := 0
	for len(b) > 0 {
		n := len(b)
		if n > 16<<10 {
			n = 16 << 10
		}
		if err := w.budget.wait(w.ctx, n); err != nil {
			return total, err
		}
		written, err := w.ResponseWriter.Write(b[:n])
		total += written
		if err != nil {
			return total, err
		}
		if written != n {
			return total, io.ErrShortWrite
		}
		b = b[n:]
	}
	return total, nil
}
func (s *Server) publish(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		w.Header().Set("Allow", "POST")
		http.Error(w, "method not allowed", 405)
		return
	}
	open := r.Header.Get("X-C1-Publish-Mode") == "open"
	var publisher Publisher
	if open {
		if s.config.PublishMode != "open" {
			http.Error(w, "open publishing is disabled; server requires a publisher token", 403)
			return
		}
		if _, present := r.Header["Authorization"]; present {
			http.Error(w, "open publishing must not include Authorization", 400)
			return
		}
	} else {
		if len(r.Header.Values("Authorization")) != 1 {
			http.Error(w, "unauthorized", 401)
			return
		}
		if mode := r.Header.Get("X-C1-Publish-Mode"); mode != "" && mode != "token" {
			http.Error(w, "invalid publish mode", 400)
			return
		}
		token := strings.TrimPrefix(r.Header.Get("Authorization"), "Bearer ")
		if !strings.HasPrefix(r.Header.Get("Authorization"), "Bearer ") {
			http.Error(w, "unauthorized", 401)
			return
		}
		var ok bool
		publisher, ok = s.store.Authenticate(s.config, token)
		if !ok {
			http.Error(w, "unauthorized", 401)
			return
		}
	}
	if len(s.store.key) == 0 {
		http.Error(w, "publishing disabled; repository is read-only", 503)
		return
	}
	header := r.Header.Get("X-C1-Metadata")
	if len(header) > 2048 {
		http.Error(w, "metadata too large", 400)
		return
	}
	b, err := base64.RawURLEncoding.DecodeString(header)
	if err != nil {
		http.Error(w, "invalid metadata", 400)
		return
	}
	var m Metadata
	decoder := json.NewDecoder(bytes.NewReader(b))
	decoder.DisallowUnknownFields()
	if decoder.Decode(&m) != nil || decoder.Decode(new(any)) != io.EOF || m.Validate() != nil {
		http.Error(w, "invalid metadata", 400)
		return
	}
	if err := s.store.AuthorizePublish(s.config, publisher, m.ID, open); err != nil {
		http.Error(w, "this publisher does not own this application", 403)
		return
	}
	// Do not accumulate big uploads or decompression work in memory on a 2 GB host.
	select {
	case s.uploads <- struct{}{}:
		defer func() { <-s.uploads }()
	default:
		busy(w)
		return
	}
	if r.ContentLength > MaxArchive {
		http.Error(w, "upload exceeds 32 MiB", 413)
		return
	}
	r.Body = http.MaxBytesReader(w, r.Body, MaxArchive)
	defer r.Body.Close()
	archive, err := BuildPackage(m, r.Body)
	if err != nil {
		http.Error(w, err.Error(), 400)
		return
	}
	p, seq, err := s.store.PublishAuthorized(s.config, publisher, m, archive, open)
	if errors.Is(err, ErrProtected) {
		http.Error(w, err.Error(), 403)
		return
	}
	if errors.Is(err, ErrConflict) {
		http.Error(w, err.Error(), 409)
		return
	}
	if err != nil {
		http.Error(w, "publish failed; retry the same version and payload", 500)
		return
	}
	jsonResponse(w, 201, map[string]any{"package": p, "sequence": seq})
}
