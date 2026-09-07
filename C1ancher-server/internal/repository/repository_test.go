package repository

import (
	"archive/tar"
	"bytes"
	"compress/gzip"
	"context"
	"crypto/ed25519"
	"crypto/rand"
	"crypto/sha256"
	"crypto/x509"
	"encoding/base64"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"encoding/pem"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"
)

func fixture(t *testing.T) (Config, *Store) {
	t.Helper()
	root := t.TempDir()
	pub, key, e := ed25519.GenerateKey(rand.Reader)
	if e != nil {
		t.Fatal(e)
	}
	der, e := x509.MarshalPKCS8PrivateKey(key)
	if e != nil {
		t.Fatal(e)
	}
	keyFile := filepath.Join(root, "private.pem")
	pubFile := filepath.Join(root, "public.raw")
	if e = os.WriteFile(keyFile, pem.EncodeToMemory(&pem.Block{Type: "PRIVATE KEY", Bytes: der}), 0600); e != nil {
		t.Fatal(e)
	}
	if e = os.WriteFile(pubFile, pub, 0600); e != nil {
		t.Fatal(e)
	}
	c := Config{Root: filepath.Join(root, "repo"), PublicKey: pubFile, SigningKey: keyFile, Downloads: 2, Queue: 20, BytesPerSecond: 1 << 30, SequenceFloor: 40, Publishers: []Publisher{{Name: "Alice", TokenSHA256: TokenHash(strings.Repeat("a", 40)), Packages: []string{"hello", "book-reader"}}, {Name: "Bob", TokenSHA256: TokenHash(strings.Repeat("b", 40)), Packages: []string{"games"}}}}
	s, e := Open(c)
	if e != nil {
		t.Fatal(e)
	}
	t.Cleanup(func() { s.Close() })
	return c, s
}
func meta() Metadata { return Metadata{ID: "hello", Version: "0.1.0", Name: "Hello", Entry: "hello"} }
func elfFixture() []byte {
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
func payload(t *testing.T, headers []*tar.Header, data [][]byte) []byte {
	t.Helper()
	var b bytes.Buffer
	gz := gzip.NewWriter(&b)
	tw := tar.NewWriter(gz)
	for i, h := range headers {
		if e := tw.WriteHeader(h); e != nil {
			t.Fatal(e)
		}
		if i < len(data) && len(data[i]) > 0 {
			if _, e := tw.Write(data[i]); e != nil {
				t.Fatal(e)
			}
		}
	}
	if e := tw.Close(); e != nil {
		t.Fatal(e)
	}
	if e := gz.Close(); e != nil {
		t.Fatal(e)
	}
	return b.Bytes()
}
func goodPayload(t *testing.T) []byte {
	b := elfFixture()
	return payload(t, []*tar.Header{{Name: "hello", Typeflag: tar.TypeReg, Mode: 0777, Size: int64(len(b))}}, [][]byte{b})
}
func publishRequest(t *testing.T, h http.Handler, m Metadata, token string, b []byte) *httptest.ResponseRecorder {
	t.Helper()
	data, _ := json.Marshal(m)
	r := httptest.NewRequest("POST", "/api/v1/publish", bytes.NewReader(b))
	r.Header.Set("Authorization", "Bearer "+token)
	r.Header.Set("X-C1-Metadata", base64.RawURLEncoding.EncodeToString(data))
	w := httptest.NewRecorder()
	h.ServeHTTP(w, r)
	return w
}
func TestPublishAuthAndSignatures(t *testing.T) {
	c, s := fixture(t)
	h := NewServer(c, s)
	body := goodPayload(t)
	if w := publishRequest(t, h, meta(), "wrong", body); w.Code != 401 {
		t.Fatal(w.Code)
	}
	if w := publishRequest(t, h, meta(), strings.Repeat("b", 40), body); w.Code != 403 {
		t.Fatal(w.Code)
	}
	w := publishRequest(t, h, meta(), strings.Repeat("a", 40), body)
	if w.Code != 201 {
		t.Fatal(w.Code, w.Body.String())
	}
	catalog := s.Catalog()
	if catalog.Sequence != 41 || len(catalog.Packages) != 1 || catalog.Packages[0].Author != "Alice" {
		t.Fatal(catalog)
	}
	for _, v := range []int{1, 2} {
		if !ed25519.Verify(s.public, s.Index(v, false), s.Index(v, true)) {
			t.Fatal("bad signature")
		}
	}
	if !bytes.Contains(s.Index(2, false), []byte("\thello\tAlice\n")) {
		t.Fatal("missing signed author")
	}
	if bytes.Contains(s.Index(1, false), []byte("Alice")) {
		t.Fatal("broke legacy schema")
	}
	if w = publishRequest(t, h, meta(), strings.Repeat("a", 40), body); w.Code != 201 || s.Catalog().Sequence != 41 {
		t.Fatal("retry not idempotent", w.Code)
	}
	m := meta()
	m.Name = "Changed"
	if w = publishRequest(t, h, m, strings.Repeat("a", 40), body); w.Code != 409 {
		t.Fatal(w.Code)
	}
	m = meta()
	m.Version = "0.0.9"
	if w = publishRequest(t, h, m, strings.Repeat("a", 40), body); w.Code != 409 {
		t.Fatal(w.Code)
	}
}
func TestRestartAndCorruption(t *testing.T) {
	c, s := fixture(t)
	a, e := BuildPackage(meta(), bytes.NewReader(goodPayload(t)))
	if e != nil {
		t.Fatal(e)
	}
	p, _, e := s.Publish(meta(), "Alice", a)
	if e != nil {
		t.Fatal(e)
	}
	if other, e := Open(c); e == nil {
		other.Close()
		t.Fatal("concurrent store opened")
	}
	s.Close()
	reopened, e := Open(c)
	if e != nil {
		t.Fatal(e)
	}
	if reopened.Catalog().Sequence != 41 {
		t.Fatal("sequence lost")
	}
	reopened.Close()
	if e = os.WriteFile(filepath.Join(c.Root, filepath.FromSlash(p.Archive)), []byte("tampered"), 0600); e != nil {
		t.Fatal(e)
	}
	if other, e := Open(c); e == nil {
		other.Close()
		t.Fatal("corruption accepted")
	}
}
func TestArchivesAndELF(t *testing.T) {
	good := goodPayload(t)
	a, e := BuildPackage(meta(), bytes.NewReader(good))
	if e != nil {
		t.Fatal(e)
	}
	gz, e := gzip.NewReader(bytes.NewReader(a))
	if e != nil {
		t.Fatal(e)
	}
	tr := tar.NewReader(gz)
	h, e := tr.Next()
	if e != nil || h.Name != "manifest.v1" {
		t.Fatal(h, e)
	}
	b, _ := io.ReadAll(tr)
	if string(b) != "C1PKG-PACKAGE 1\nid\thello\nversion\t0.1.0\nentry\thello\n" {
		t.Fatal(string(b))
	}
	h, e = tr.Next()
	if e != nil || h.Mode != 0755 {
		t.Fatal(h, e)
	}
	for _, name := range []string{"../hello", "/hello", ".c1pkg-entry", "foo/../hello", "hello\\bad"} {
		upload := payload(t, []*tar.Header{{Name: name, Typeflag: tar.TypeReg, Size: 1}}, [][]byte{{1}})
		if _, e = BuildPackage(meta(), bytes.NewReader(upload)); e == nil {
			t.Fatal("accepted", name)
		}
	}
	for _, typ := range []byte{tar.TypeSymlink, tar.TypeLink, tar.TypeChar, tar.TypeFifo} {
		upload := payload(t, []*tar.Header{{Name: "hello", Typeflag: typ, Linkname: "/etc/shadow"}}, nil)
		if _, e = BuildPackage(meta(), bytes.NewReader(upload)); e == nil {
			t.Fatal("accepted special file")
		}
	}
	upload := payload(t, []*tar.Header{{Name: "hello", Typeflag: tar.TypeReg, Size: 1}, {Name: "hello", Typeflag: tar.TypeReg, Size: 1}}, [][]byte{{1}, {2}})
	if _, e = BuildPackage(meta(), bytes.NewReader(upload)); e == nil {
		t.Fatal("accepted duplicate")
	}
	if _, e = BuildPackage(meta(), bytes.NewReader(good[:len(good)-4])); e == nil {
		t.Fatal("accepted truncated gzip")
	}
	wrong := elfFixture()
	binary.LittleEndian.PutUint16(wrong[18:], 62)
	if ValidateELF(wrong) == nil {
		t.Fatal("accepted x86")
	}
	wrong = elfFixture()
	binary.LittleEndian.PutUint32(wrong[52:], 3)
	if ValidateELF(wrong) == nil {
		t.Fatal("accepted dynamic")
	}
}
func TestVersionAndReservedIDs(t *testing.T) {
	for _, pair := range [][2]string{{"0.1.12", "0.1.9"}, {"1.0.0", "0.99.0"}, {"1.0.0.1", "1.0.0"}} {
		v, e := CompareVersion(pair[0], pair[1])
		if e != nil || v != 1 {
			t.Fatal(pair, v, e)
		}
	}
	if v, e := CompareVersion("1.0", "1.0.0"); e != nil || v != 0 {
		t.Fatal(v, e)
	}
	for _, v := range []string{"1.01.0", "1.0beta", "1.0\n", "1.18446744073709551616"} {
		m := meta()
		m.Version = v
		if m.Validate() == nil {
			t.Fatal(v)
		}
	}
	for _, id := range []string{"c1pkg", "C1ancher", "C1UPDATER", "app_daemon"} {
		m := meta()
		m.ID = id
		if m.Validate() == nil {
			t.Fatal(id)
		}
	}
	if v, e := NextVersion("0.1.12"); e != nil || v != "0.1.13" {
		t.Fatal(v, e)
	}
}
func TestHTTPRangeAndRouteIsolation(t *testing.T) {
	c, s := fixture(t)
	a, e := BuildPackage(meta(), bytes.NewReader(goodPayload(t)))
	if e != nil {
		t.Fatal(e)
	}
	p, _, e := s.Publish(meta(), "Alice", a)
	if e != nil {
		t.Fatal(e)
	}
	h := NewServer(c, s)
	r := httptest.NewRequest("GET", "/c1/v2/"+p.Archive, nil)
	r.Header.Set("Range", "bytes=0-9")
	w := httptest.NewRecorder()
	h.ServeHTTP(w, r)
	if w.Code != 206 || !bytes.Equal(w.Body.Bytes(), a[:10]) {
		t.Fatal(w.Code, w.Body.String())
	}
	for _, url := range []string{"/current.json", "/c1/v2/../current.json", "/c1/v2/repository.lock", "/c1/v2/objects/not-a-hash.tar.gz"} {
		r = httptest.NewRequest("GET", url, nil)
		w = httptest.NewRecorder()
		h.ServeHTTP(w, r)
		if w.Code != 404 {
			t.Fatal(url, w.Code)
		}
	}
}
func TestGateBoundedCancellation(t *testing.T) {
	g := NewGate(1, 1)
	release, e := g.Acquire(context.Background())
	if e != nil {
		t.Fatal(e)
	}
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() {
		r, e := g.Acquire(ctx)
		if r != nil {
			r()
		}
		done <- e
	}()
	deadline := time.Now().Add(time.Second)
	for {
		_, q := g.Stats()
		if q == 1 {
			break
		}
		if time.Now().After(deadline) {
			t.Fatal("waiter missing")
		}
		time.Sleep(time.Millisecond)
	}
	if _, e = g.Acquire(context.Background()); e != ErrBusy {
		t.Fatal(e)
	}
	cancel()
	if <-done == nil {
		t.Fatal("cancel ignored")
	}
	release()
	a, q := g.Stats()
	if a != 0 || q != 0 {
		t.Fatal(a, q)
	}
	// Repeated grant/cancel races must not leak capacity.
	for i := 0; i < 100; i++ {
		release, e = g.Acquire(context.Background())
		if e != nil {
			t.Fatal(e)
		}
		ctx, cancel = context.WithCancel(context.Background())
		var wg sync.WaitGroup
		wg.Add(1)
		go func() {
			defer wg.Done()
			r, _ := g.Acquire(ctx)
			if r != nil {
				r()
			}
		}()
		cancel()
		release()
		wg.Wait()
	}
	a, q = g.Stats()
	if a != 0 || q != 0 {
		t.Fatal(a, q)
	}
}
func TestConcurrentPublishDoesNotDropOtherApplications(t *testing.T) {
	_, s := fixture(t)
	a, e := BuildPackage(meta(), bytes.NewReader(goodPayload(t)))
	if e != nil {
		t.Fatal(e)
	}
	var wg sync.WaitGroup
	for i := 0; i < 20; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			m := meta()
			m.ID = fmt.Sprintf("app-%d", i)
			if _, _, e := s.Publish(m, "Author", a); e != nil {
				t.Error(e)
			}
		}(i)
	}
	wg.Wait()
	if c := s.Catalog(); len(c.Packages) != 20 || c.Sequence != 60 {
		t.Fatal(c)
	}
}
func TestImportVerifiesTrustAndPreservesSequence(t *testing.T) {
	c, s := fixture(t)
	dir := t.TempDir()
	a, e := BuildPackage(meta(), bytes.NewReader(goodPayload(t)))
	if e != nil {
		t.Fatal(e)
	}
	hash := sha256.Sum256(a)
	archive := "packages/hello/0.1.0.tar.gz"
	if e = os.MkdirAll(filepath.Join(dir, "packages/hello"), 0700); e != nil {
		t.Fatal(e)
	}
	if e = os.WriteFile(filepath.Join(dir, archive), a, 0600); e != nil {
		t.Fatal(e)
	}
	catalog := Catalog{Sequence: 100, Packages: []Package{{Metadata: meta(), Archive: archive, SHA256: hex.EncodeToString(hash[:]), Size: int64(len(a))}}}
	b := index(catalog, 1)
	os.WriteFile(filepath.Join(dir, "index.v1"), b, 0600)
	os.WriteFile(filepath.Join(dir, "index.v1.sig"), ed25519.Sign(s.key, b), 0600)
	if e = ImportLegacy(s, c, dir); e != nil {
		t.Fatal(e)
	}
	if s.Catalog().Sequence != 101 {
		t.Fatal(s.Catalog())
	}
	os.WriteFile(filepath.Join(dir, "index.v1"), append(b, byte('!')), 0600)
	if e = ImportLegacy(s, c, dir); e == nil {
		t.Fatal("tampered import accepted")
	}
}
func TestReadOnlyMode(t *testing.T) {
	c, s := fixture(t)
	s.Close()
	c.SigningKey = ""
	readOnly, e := Open(c)
	if e != nil {
		t.Fatal(e)
	}
	defer readOnly.Close()
	w := publishRequest(t, NewServer(c, readOnly), meta(), strings.Repeat("a", 40), goodPayload(t))
	if w.Code != 503 {
		t.Fatal(w.Code)
	}
}
