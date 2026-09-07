package repository

import (
	"bytes"
	"context"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"runtime"
	"sync"
	"testing"
	"time"
)

func TestTwentyDownloadsShareGlobalBudget(t *testing.T) {
	c, s := fixture(t)
	c.BytesPerSecond = 320000
	data := bytes.Repeat([]byte("payload!"), 1024)
	p, _, err := s.Publish(meta(), "Alice", data)
	if err != nil {
		t.Fatal(err)
	}
	handler := NewServer(c, s)
	server := httptest.NewServer(handler)
	defer server.Close()
	start := time.Now()
	var wg sync.WaitGroup
	for i := 0; i < 20; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			resp, e := server.Client().Get(server.URL + "/c1/v2/" + p.Archive)
			if e != nil {
				t.Error(e)
				return
			}
			defer resp.Body.Close()
			body, e := io.ReadAll(resp.Body)
			if e != nil || resp.StatusCode != 200 || !bytes.Equal(body, data) {
				t.Errorf("download failed: %d %v", resp.StatusCode, e)
			}
		}()
	}
	done := make(chan struct{})
	go func() { wg.Wait(); close(done) }()
	maxActive, maxWaiting := 0, 0
	ticker := time.NewTicker(time.Millisecond)
	defer ticker.Stop()
loop:
	for {
		select {
		case <-done:
			break loop
		case <-ticker.C:
			a, q := handler.gate.Stats()
			if a > maxActive {
				maxActive = a
			}
			if q > maxWaiting {
				maxWaiting = q
			}
		}
	}
	if maxActive > 2 || maxActive == 0 || maxWaiting == 0 || maxWaiting > 20 {
		t.Fatalf("active=%d waiting=%d", maxActive, maxWaiting)
	}
	minimum := time.Duration(int64(len(data)*20) * int64(time.Second) / c.BytesPerSecond)
	if elapsed := time.Since(start); elapsed < minimum*9/10 {
		t.Fatalf("global rate bypass: elapsed %v expected at least %v", elapsed, minimum)
	}
	if a, q := handler.gate.Stats(); a != 0 || q != 0 {
		t.Fatal("download slots leaked", a, q)
	}
}
func TestQueueFullReturnsRetryAfter(t *testing.T) {
	c, s := fixture(t)
	c.Downloads = 1
	c.Queue = 0
	p, _, err := s.Publish(meta(), "Alice", []byte("test"))
	if err != nil {
		t.Fatal(err)
	}
	handler := NewServer(c, s)
	release, err := handler.gate.Acquire(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	defer release()
	w := httptest.NewRecorder()
	handler.ServeHTTP(w, httptest.NewRequest("GET", "/c1/v2/"+p.Archive, nil))
	if w.Code != http.StatusServiceUnavailable || w.Header().Get("Retry-After") != "5" {
		t.Fatal(w.Code, w.Header())
	}
	w = httptest.NewRecorder()
	handler.ServeHTTP(w, httptest.NewRequest("GET", "/c1/v2/index.v1", nil))
	if w.Code != 200 {
		t.Fatal("downloads blocked metadata")
	}
}
func TestCoreDownloadsUseSameGate(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("production core channels use Linux symlinks")
	}
	c, s := fixture(t)
	c.CoreRoot = t.TempDir()
	c.Downloads = 1
	c.Queue = 0
	if err := os.MkdirAll(filepath.Join(c.CoreRoot, "releases/1/artifacts"), 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(filepath.Join(c.CoreRoot, "channels"), 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink("../releases/1", filepath.Join(c.CoreRoot, "channels/stable")); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(c.CoreRoot, "releases/1/artifacts/c1pkg"), []byte("signed component"), 0644); err != nil {
		t.Fatal(err)
	}
	handler := NewServer(c, s)
	release, err := handler.gate.Acquire(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	w := httptest.NewRecorder()
	handler.ServeHTTP(w, httptest.NewRequest("GET", "/c1/core/v1/stable/artifacts/c1pkg", nil))
	if w.Code != 503 {
		t.Fatal(w.Code)
	}
	release()
	w = httptest.NewRecorder()
	handler.ServeHTTP(w, httptest.NewRequest("GET", "/c1/core/v1/stable/artifacts/c1pkg", nil))
	if w.Code != 200 {
		t.Fatal(w.Code)
	}
	w = httptest.NewRecorder()
	handler.ServeHTTP(w, httptest.NewRequest("GET", "/c1/core/v1/trust/core.pem", nil))
	if w.Code != 404 {
		t.Fatal("core trust directory exposed")
	}
}
func TestStorageBudgetAndPinnedKey(t *testing.T) {
	c, s := fixture(t)
	s.maxBytes = 3
	if _, _, err := s.Publish(meta(), "Alice", []byte("four")); err == nil {
		t.Fatal("storage budget ignored")
	}
	if s.Catalog().Sequence != 40 {
		t.Fatal("failed publish advanced index")
	}
	s.Close()
	if err := os.WriteFile(c.PublicKey, make([]byte, 32), 0600); err != nil {
		t.Fatal(err)
	}
	if other, err := Open(c); err == nil {
		other.Close()
		t.Fatal("mismatched signing key accepted")
	}
}
