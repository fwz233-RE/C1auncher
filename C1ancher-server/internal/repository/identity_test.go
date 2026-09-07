package repository

import (
	"archive/tar"
	"bytes"
	"crypto/ed25519"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"sync"
	"testing"
	"time"
)

// All credentials, keys, ELF payloads and repositories in these tests are
// synthetic. The deterministic tokens are test data, not production secrets.
func identityToken(seed string) string {
	digest := sha256.Sum256([]byte("identity-test-only:" + seed))
	return base64.RawURLEncoding.EncodeToString(digest[:])
}

func identityFixture(t *testing.T) (Config, *Store, *Server) {
	t.Helper()
	c, s := fixture(t)
	c.SelfRegistration = true
	c.PublishMode = "open"
	return c, s, NewServer(c, s)
}

func identityRegisterRaw(h http.Handler, method, body string, authorization []string) *httptest.ResponseRecorder {
	r := httptest.NewRequest(method, "/api/v1/publishers/register", strings.NewReader(body))
	r.Header.Set("Content-Type", "application/json")
	for _, value := range authorization {
		r.Header.Add("Authorization", value)
	}
	w := httptest.NewRecorder()
	h.ServeHTTP(w, r)
	return w
}

func identityRegister(t *testing.T, h http.Handler, name, token string, want int) *httptest.ResponseRecorder {
	t.Helper()
	body, err := json.Marshal(map[string]string{"name": name})
	if err != nil {
		t.Fatal(err)
	}
	w := identityRegisterRaw(h, http.MethodPost, string(body), []string{"Bearer " + token})
	if w.Code != want {
		t.Fatalf("register %q: HTTP %d, want %d: %s", name, w.Code, want, w.Body.String())
	}
	if want == 200 || want == 201 {
		var got map[string]any
		if err := json.Unmarshal(w.Body.Bytes(), &got); err != nil || len(got) != 1 || got["name"] != name {
			t.Fatalf("registration must return only the author name: %s (%v)", w.Body.String(), err)
		}
		if w.Header().Get("Content-Type") != "application/json" || w.Header().Get("Cache-Control") != "no-store" {
			t.Fatalf("unexpected registration response headers: %v", w.Header())
		}
	}
	return w
}

func identityPublish(t *testing.T, h http.Handler, m Metadata, token string, body []byte, want int) {
	t.Helper()
	w := publishRequest(t, h, m, token, body)
	if w.Code != want {
		t.Fatalf("publish %s: HTTP %d, want %d: %s", m.ID, w.Code, want, w.Body.String())
	}
}

func identityMetadata(id string) Metadata {
	m := meta()
	m.ID = id
	return m
}

func identitySnapshot(t *testing.T, c Config) snapshot {
	t.Helper()
	b, err := os.ReadFile(filepath.Join(c.Root, "current.json"))
	if err != nil {
		t.Fatal(err)
	}
	var v snapshot
	if err := json.Unmarshal(b, &v); err != nil {
		t.Fatal(err)
	}
	return v
}

func identityClaim(t *testing.T, s *Store, id, name string) {
	t.Helper()
	s.mu.RLock()
	defer s.mu.RUnlock()
	count := 0
	if r := s.current.Registry; r != nil {
		for _, claim := range r.Claims {
			if strings.EqualFold(claim.ID, id) {
				count++
				if claim.ID != id || claim.Name != name {
					t.Fatalf("unexpected owner: %+v, want %s/%s", claim, id, name)
				}
			}
		}
	}
	if count != 1 {
		t.Fatalf("want one durable claim for %s/%s, got %d", id, name, count)
	}
}

func identityReopen(t *testing.T, c Config, s *Store) *Store {
	t.Helper()
	if err := s.Close(); err != nil {
		t.Fatal(err)
	}
	reopened, err := Open(c)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { reopened.Close() })
	return reopened
}

func TestIdentityRegistrationOptInAndMethods(t *testing.T) {
	c, s := fixture(t)
	token := identityToken("opt-in")
	before := identitySnapshot(t, c)
	identityRegister(t, NewServer(c, s), "Carol", token, 403)
	if !reflect.DeepEqual(before, identitySnapshot(t, c)) {
		t.Fatal("disabled registration changed durable state")
	}
	c.SelfRegistration = true
	h := NewServer(c, s)
	for _, method := range []string{"GET", "HEAD", "PUT", "DELETE", "OPTIONS"} {
		w := identityRegisterRaw(h, method, `{"name":"Carol"}`, []string{"Bearer " + token})
		if w.Code != 405 || w.Header().Get("Allow") != "POST" {
			t.Fatalf("%s registration: HTTP %d, Allow %q", method, w.Code, w.Header().Get("Allow"))
		}
	}
	identityRegister(t, h, "Carol", token, 201)
	if p, ok := s.Authenticate(c, token); !ok || p.Name != "Carol" {
		t.Fatal("registered token did not authenticate", p, ok)
	}
	if !bytes.Equal(index(before.Catalog, 2), index(s.Catalog(), 2)) || !bytes.Equal(before.Index2, s.Index(2, false)) {
		t.Fatal("registration changed package catalog or sequence")
	}
}

func TestIdentityRegistrationIdempotenceConflictsAndPrivacy(t *testing.T) {
	c, s, h := identityFixture(t)
	token := identityToken("carol")
	identityRegister(t, h, "Carol", token, 201)
	before, err := os.ReadFile(filepath.Join(c.Root, "current.json"))
	if err != nil {
		t.Fatal(err)
	}
	identityRegister(t, h, "Carol", token, 200)
	identityRegister(t, h, "Carol", identityToken("other"), 409)
	identityRegister(t, h, "carol", identityToken("case"), 409)
	identityRegister(t, h, "carol", token, 409)
	identityRegister(t, h, "Other Author", token, 409)
	identityRegister(t, h, "Alice", identityToken("alice-impostor"), 409)
	identityRegister(t, h, "bOB", identityToken("bob-impostor"), 409)
	identityRegister(t, h, "Other Author", strings.Repeat("a", 40), 409)
	// Existing static tokens retain recovery compatibility without being copied
	// into the dynamic registry, which would bypass later config revocation.
	identityRegister(t, h, "Alice", strings.Repeat("a", 40), 200)
	identityRegister(t, h, "Bob", strings.Repeat("b", 40), 200)
	after, err := os.ReadFile(filepath.Join(c.Root, "current.json"))
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(before, after) {
		t.Fatal("idempotent or conflicting registrations rewrote durable state")
	}
	if bytes.Contains(after, []byte(token)) || !bytes.Contains(after, []byte(TokenHash(token))) {
		t.Fatal("registry must store only token digests")
	}
	v := identitySnapshot(t, c)
	if v.Registry == nil || len(v.Registry.Publishers) != 1 || len(v.Registry.Claims) != 0 {
		t.Fatalf("unexpected registry: %+v", v.Registry)
	}
	if !ed25519.Verify(s.public, registryMessage(v), v.RegistrySignature) {
		t.Fatal("identity registry is not signed")
	}
	for _, route := range []string{"/api/v1/catalog", "/healthz", "/c1/v1/index.v1", "/c1/v2/index.v1", "/current.json", "/registry", "/api/v1/publishers"} {
		w := httptest.NewRecorder()
		h.ServeHTTP(w, httptest.NewRequest("GET", route, nil))
		for _, private := range []string{token, TokenHash(token), "tokenSHA256", "registrySignature"} {
			if strings.Contains(w.Body.String(), private) {
				t.Fatalf("private identity material exposed by %s", route)
			}
		}
	}
}

func TestIdentityRegistrationCanonicalTokens(t *testing.T) {
	canonical := identityToken("canonical")
	alphabet := "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
	last := strings.IndexByte(alphabet, canonical[len(canonical)-1])
	noncanonical := canonical[:len(canonical)-1] + string(alphabet[last+1])
	cases := []struct {
		name string
		auth []string
	}{
		{"missing", nil},
		{"empty", []string{"Bearer "}},
		{"basic", []string{"Basic " + canonical}},
		{"lowercase-scheme", []string{"bearer " + canonical}},
		{"duplicate", []string{"Bearer " + canonical, "Bearer " + canonical}},
		{"comma-joined", []string{"Bearer " + canonical + ", Bearer " + canonical}},
		{"short", []string{"Bearer " + base64.RawURLEncoding.EncodeToString(make([]byte, 31))}},
		{"long", []string{"Bearer " + base64.RawURLEncoding.EncodeToString(make([]byte, 33))}},
		{"padded", []string{"Bearer " + canonical + "="}},
		{"nonzero-pad-bits", []string{"Bearer " + noncanonical}},
		{"newline", []string{"Bearer " + canonical[:10] + "\n" + canonical[10:]}},
		{"trailing-space", []string{"Bearer " + canonical + " "}},
		{"extra-space", []string{"Bearer  " + canonical}},
		{"standard-base64", []string{"Bearer " + base64.RawStdEncoding.EncodeToString(bytes.Repeat([]byte{255}, 32))}},
		{"arbitrary-old-format", []string{"Bearer " + strings.Repeat("z", 40)}},
		{"oversized", []string{"Bearer " + strings.Repeat("a", 257)}},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			c, s, h := identityFixture(t)
			w := identityRegisterRaw(h, "POST", `{"name":"Carol"}`, tc.auth)
			if w.Code != 401 {
				t.Fatalf("HTTP %d, want 401: %s", w.Code, w.Body.String())
			}
			if s.current.Registry != nil || identitySnapshot(t, c).Registry != nil {
				t.Fatal("invalid credential created identity state")
			}
			identityRegister(t, h, "Carol", canonical, 201)
		})
	}
}

func TestIdentityRegistrationNamesAndJSON(t *testing.T) {
	badNames := []string{"", " Alice", "Alice ", "A\nB", "A\tB", "A\x00B", "A\u200bB", "A\u202eB", "A\ufffdB", strings.Repeat("x", 41), strings.Repeat("界", 14), OpenPublisherName, strings.ToUpper(OpenPublisherName)}
	for i, name := range badNames {
		t.Run(fmt.Sprintf("invalid-name-%d", i), func(t *testing.T) {
			_, s, h := identityFixture(t)
			identityRegister(t, h, name, identityToken("name"), 400)
			if s.current.Registry != nil {
				t.Fatal("invalid author created registry")
			}
		})
	}
	for _, name := range []string{"Carol", "作者", strings.Repeat("x", 40), strings.Repeat("界", 13)} {
		t.Run("valid-"+name, func(t *testing.T) {
			_, _, h := identityFixture(t)
			identityRegister(t, h, name, identityToken(name), 201)
		})
	}
	for i, body := range []string{"", `{}`, `null`, `[]`, `{"name":1}`, `{"name":"Carol","author":"Alice"}`, `{"name":"Carol"} {}`, `{"name":"Carol"} garbage`, `{"name":"Carol"`, `{"name":"` + strings.Repeat("x", 1024) + `"}`, "{\"name\":\"\xff\"}"} {
		t.Run(fmt.Sprintf("json-%d", i), func(t *testing.T) {
			_, s, h := identityFixture(t)
			w := identityRegisterRaw(h, "POST", body, []string{"Bearer " + identityToken("json")})
			if w.Code != 400 || s.current.Registry != nil {
				t.Fatalf("invalid JSON: HTTP %d, registry %+v", w.Code, s.current.Registry)
			}
		})
	}
}

func TestIdentityFirstPublishDurableOwnershipAndRestart(t *testing.T) {
	c, s, h := identityFixture(t)
	carol, dave := identityToken("carol"), identityToken("dave")
	identityRegister(t, h, "Carol", carol, 201)
	identityRegister(t, h, "Dave", dave, 201)
	// Identities must survive restart even before they have published anything.
	s = identityReopen(t, c, s)
	h = NewServer(c, s)
	identityRegister(t, h, "Carol", carol, 200)
	body := goodPayload(t)
	m := identityMetadata("carol-app")
	identityPublish(t, h, m, carol, body, 201)
	identityClaim(t, s, m.ID, "Carol")
	seq := s.Catalog().Sequence
	identityPublish(t, h, m, carol, body, 201)
	if s.Catalog().Sequence != seq {
		t.Fatal("same owner retry advanced sequence")
	}
	m.Version = "0.2.0"
	for _, token := range []string{dave, strings.Repeat("a", 40), strings.Repeat("b", 40)} {
		identityPublish(t, h, m, token, body, 403)
	}
	openPublish(t, h, m, body, 403)
	variant := m
	variant.ID = strings.ToUpper(m.ID)
	identityPublish(t, h, variant, carol, body, 403)
	identityPublish(t, h, variant, dave, body, 403)
	openPublish(t, h, variant, body, 403)
	identityPublish(t, h, m, carol, body, 201)
	for _, version := range []int{1, 2} {
		if !ed25519.Verify(s.public, s.Index(version, false), s.Index(version, true)) {
			t.Fatalf("index v%d signature invalid", version)
		}
	}
	if !bytes.Contains(s.Index(2, false), []byte("\tCarol\n")) || bytes.Contains(s.Index(1, false), []byte("Carol")) {
		t.Fatal("signed author is missing or changed legacy schema")
	}
	before := identitySnapshot(t, c)
	s = identityReopen(t, c, s)
	h = NewServer(c, s)
	if !reflect.DeepEqual(before, identitySnapshot(t, c)) {
		t.Fatal("restart changed signed identity/catalog snapshot")
	}
	identityClaim(t, s, m.ID, "Carol")
	identityRegister(t, h, "Carol", carol, 200)
	m.Version = "0.3.0"
	identityPublish(t, h, m, dave, body, 403)
	openPublish(t, h, m, body, 403)
	identityPublish(t, h, m, carol, body, 201)
	identityClaim(t, s, m.ID, "Carol")
	v := identitySnapshot(t, c)
	if len(v.Registry.Claims) != 1 || v.Catalog.Packages[0].Author != "Carol" || !ed25519.Verify(s.public, registryMessage(v), v.RegistrySignature) {
		t.Fatal("ownership was duplicated, lost, or not committed with signed catalog")
	}
}

func TestIdentityStaticPublishersCanClaimOnlyWhenEnabled(t *testing.T) {
	for _, enabled := range []bool{false, true} {
		t.Run(fmt.Sprint(enabled), func(t *testing.T) {
			c, s := fixture(t)
			c.SelfRegistration = enabled
			h := NewServer(c, s)
			body := goodPayload(t)
			for _, tc := range []struct{ name, token, assigned, fresh string }{
				{"Alice", strings.Repeat("a", 40), "hello", "alice-new"},
				{"Bob", strings.Repeat("b", 40), "games", "bob-new"},
			} {
				identityPublish(t, h, identityMetadata(tc.assigned), tc.token, body, 201)
				want := 403
				if enabled {
					want = 201
				}
				identityPublish(t, h, identityMetadata(tc.fresh), tc.token, body, want)
				if enabled {
					identityClaim(t, s, tc.fresh, tc.name)
				}
			}
			identityPublish(t, h, identityMetadata("hello"), strings.Repeat("b", 40), body, 403)
			identityPublish(t, h, identityMetadata("games"), strings.Repeat("a", 40), body, 403)
			if !enabled {
				return
			}
			s = identityReopen(t, c, s)
			h = NewServer(c, s)
			for _, tc := range []struct{ name, token, id string }{{"Alice", strings.Repeat("a", 40), "alice-new"}, {"Bob", strings.Repeat("b", 40), "bob-new"}} {
				m := identityMetadata(tc.id)
				m.Version = "0.2.0"
				identityPublish(t, h, m, tc.token, body, 201)
				identityClaim(t, s, tc.id, tc.name)
			}
			if len(identitySnapshot(t, c).Registry.Publishers) != 0 {
				t.Fatal("static credentials were copied into registry")
			}
		})
	}
}

func TestIdentityHistoricalAndAnonymousApplicationsCannotBeClaimed(t *testing.T) {
	c, s, h := identityFixture(t)
	body := goodPayload(t)
	// A pre-feature protected publication has no dynamic claim.
	old := identityMetadata("retired-app")
	archive, err := BuildPackage(old, bytes.NewReader(body))
	if err != nil {
		t.Fatal(err)
	}
	if _, _, err := s.Publish(old, "Retired Author", archive); err != nil {
		t.Fatal(err)
	}
	anonymous := identityMetadata("anonymous-app")
	openPublish(t, h, anonymous, body, 201)
	identityRegister(t, h, "Retired Author", identityToken("retired"), 409)
	identityRegister(t, h, "retired author", identityToken("retired-case"), 409)
	carol := identityToken("carol")
	identityRegister(t, h, "Carol", carol, 201)
	for _, id := range []string{old.ID, strings.ToUpper(old.ID), anonymous.ID, strings.ToUpper(anonymous.ID), "hello", "HELLO", "games"} {
		m := identityMetadata(id)
		m.Version = "0.2.0"
		identityPublish(t, h, m, carol, body, 403)
		if id == old.ID || id == anonymous.ID {
			identityPublish(t, h, m, strings.Repeat("a", 40), body, 403)
		}
	}
	old.Version = "0.2.0"
	openPublish(t, h, old, body, 403)
	anonymous.Version = "0.2.0"
	openPublish(t, h, anonymous, body, 201)
	if len(identitySnapshot(t, c).Registry.Claims) != 0 {
		t.Fatal("existing applications acquired claims")
	}
	s = identityReopen(t, c, s)
	h = NewServer(c, s)
	identityRegister(t, h, "Retired Author", identityToken("retired"), 409)
	identityPublish(t, h, old, carol, body, 403)
	identityPublish(t, h, anonymous, carol, body, 403)
}

func TestIdentityImportedApplicationRemainsProtectedWithoutConfig(t *testing.T) {
	c, s := fixture(t)
	m := meta()
	archive, err := BuildPackage(m, bytes.NewReader(goodPayload(t)))
	if err != nil {
		t.Fatal(err)
	}
	dir := t.TempDir()
	relative := "packages/hello/0.1.0.tar.gz"
	if err := os.MkdirAll(filepath.Join(dir, "packages", "hello"), 0700); err != nil {
		t.Fatal(err)
	}
	digest := sha256.Sum256(archive)
	catalog := Catalog{Sequence: 100, Packages: []Package{{Metadata: m, Archive: relative, SHA256: hex.EncodeToString(digest[:]), Size: int64(len(archive))}}}
	idx := index(catalog, 1)
	for file, data := range map[string][]byte{relative: archive, "index.v1": idx, "index.v1.sig": ed25519.Sign(s.key, idx)} {
		if err := os.WriteFile(filepath.Join(dir, filepath.FromSlash(file)), data, 0600); err != nil {
			t.Fatal(err)
		}
	}
	if err := ImportLegacy(s, c, dir); err != nil {
		t.Fatal(err)
	}
	c.SelfRegistration = true
	c.PublishMode = "open"
	c.Publishers = nil // Revoking the importer must not expose its IDs or author.
	s = identityReopen(t, c, s)
	h := NewServer(c, s)
	identityRegister(t, h, "Alice", identityToken("alice"), 409)
	identityRegister(t, h, "ALICE", identityToken("alice-case"), 409)
	carol := identityToken("carol")
	identityRegister(t, h, "Carol", carol, 201)
	m.Version = "0.2.0"
	identityPublish(t, h, m, carol, goodPayload(t), 403)
	openPublish(t, h, m, goodPayload(t), 403)
	if got := s.Catalog(); got.Sequence != 101 || got.Packages[0].Author != "Alice" || len(identitySnapshot(t, c).Registry.Claims) != 0 {
		t.Fatal("imported ownership changed", got)
	}
}

func TestIdentityRejectedUploadsNeverClaim(t *testing.T) {
	for _, kind := range []string{"gzip", "traversal", "symlink", "elf", "entry", "version", "reserved", "metadata-author", "oversized", "busy", "storage"} {
		t.Run(kind, func(t *testing.T) {
			c, s, h := identityFixture(t)
			carol, dave := identityToken("carol"), identityToken("dave")
			identityRegister(t, h, "Carol", carol, 201)
			identityRegister(t, h, "Dave", dave, 201)
			before := identitySnapshot(t, c)
			m := identityMetadata("unclaimed")
			body := goodPayload(t)
			want := 400
			switch kind {
			case "gzip":
				body = []byte("not gzip")
			case "traversal":
				body = payload(t, []*tar.Header{{Name: "../hello", Typeflag: tar.TypeReg}}, nil)
			case "symlink":
				body = payload(t, []*tar.Header{{Name: "hello", Typeflag: tar.TypeSymlink, Linkname: "/etc/shadow"}}, nil)
			case "elf":
				body = payload(t, []*tar.Header{{Name: "hello", Typeflag: tar.TypeReg, Size: 3}}, [][]byte{[]byte("bad")})
			case "entry":
				m.Entry = "missing"
			case "version":
				m.Version = "1.01.0"
			case "reserved":
				m.ID = "C1ancher"
			case "oversized":
				want = 413
			case "busy":
				want = 503
				h.uploads <- struct{}{}
			case "storage":
				want = 500
				s.maxBytes = 1
			}
			data, _ := json.Marshal(m)
			if kind == "metadata-author" {
				data = []byte(`{"id":"unclaimed","version":"0.1.0","name":"Hello","entry":"hello","author":"Alice"}`)
			}
			r := httptest.NewRequest("POST", "/api/v1/publish", bytes.NewReader(body))
			r.Header.Set("Authorization", "Bearer "+carol)
			r.Header.Set("X-C1-Metadata", base64.RawURLEncoding.EncodeToString(data))
			if kind == "oversized" {
				r.ContentLength = MaxArchive + 1
			}
			w := httptest.NewRecorder()
			h.ServeHTTP(w, r)
			if kind == "busy" {
				<-h.uploads
			}
			s.maxBytes = 2 << 30
			if w.Code != want {
				t.Fatalf("HTTP %d, want %d: %s", w.Code, want, w.Body.String())
			}
			if !reflect.DeepEqual(before, identitySnapshot(t, c)) || !bytes.Equal(index(before.Catalog, 2), index(s.Catalog(), 2)) || len(s.current.Registry.Claims) != 0 {
				t.Fatal("rejected upload changed identity or catalog state")
			}
			// Another author can still claim the ordinary ID after a failure.
			identityPublish(t, h, identityMetadata("unclaimed"), dave, goodPayload(t), 201)
			identityClaim(t, s, "unclaimed", "Dave")
		})
	}
}

// The barrier is reached only when HTTP authorization has succeeded and the
// payload is being read. Separate handlers share one store to exercise the
// commit-time ownership check rather than the per-handler upload busy response.
type identityBarrierReader struct {
	io.Reader
	once    sync.Once
	ready   chan<- struct{}
	release <-chan struct{}
}

func (r *identityBarrierReader) Read(p []byte) (int, error) {
	r.once.Do(func() { r.ready <- struct{}{}; <-r.release })
	return r.Reader.Read(p)
}

func TestIdentityConcurrentFirstPublishExactlyOneOwner(t *testing.T) {
	for _, caseVariant := range []bool{false, true} {
		t.Run(fmt.Sprintf("case-variant-%t", caseVariant), func(t *testing.T) {
			c, s, h := identityFixture(t)
			tokens := []string{identityToken("carol"), identityToken("dave")}
			names := []string{"Carol", "Dave"}
			for i := range names {
				identityRegister(t, h, names[i], tokens[i], 201)
			}
			body := goodPayload(t)
			ready, release := make(chan struct{}, 2), make(chan struct{})
			responses := make(chan struct {
				i    int
				code int
				body string
			}, 2)
			for i := range names {
				m := identityMetadata("contested")
				if caseVariant && i == 1 {
					m.ID = "CONTESTED"
				}
				data, _ := json.Marshal(m)
				r := httptest.NewRequest("POST", "/api/v1/publish", &identityBarrierReader{Reader: bytes.NewReader(body), ready: ready, release: release})
				r.Header.Set("Authorization", "Bearer "+tokens[i])
				r.Header.Set("X-C1-Metadata", base64.RawURLEncoding.EncodeToString(data))
				handler := NewServer(c, s)
				go func(i int) {
					w := httptest.NewRecorder()
					handler.ServeHTTP(w, r)
					responses <- struct {
						i    int
						code int
						body string
					}{i, w.Code, w.Body.String()}
				}(i)
			}
			for range 2 {
				select {
				case <-ready:
				case <-time.After(5 * time.Second):
					close(release)
					t.Fatal("both publishers did not reach archive validation")
				}
			}
			close(release)
			winner, successes, denied := -1, 0, 0
			for range 2 {
				response := <-responses
				switch response.code {
				case 201:
					winner = response.i
					successes++
				case 403:
					denied++
				default:
					t.Errorf("unexpected race response: HTTP %d: %s", response.code, response.body)
				}
			}
			if successes != 1 || denied != 1 {
				t.Fatalf("want exactly one committed owner and one forbidden response: successes %d, denied %d", successes, denied)
			}
			catalog := s.Catalog()
			if catalog.Sequence != 41 || len(catalog.Packages) != 1 || catalog.Packages[0].Author != names[winner] {
				t.Fatal("concurrent publication changed more than one catalog entry", catalog)
			}
			id := catalog.Packages[0].ID
			identityClaim(t, s, id, names[winner])
			s = identityReopen(t, c, s)
			h = NewServer(c, s)
			m := identityMetadata(id)
			m.Version = "0.2.0"
			identityPublish(t, h, m, tokens[1-winner], body, 403)
			openPublish(t, h, m, body, 403)
			identityPublish(t, h, m, tokens[winner], body, 201)
		})
	}
}

func TestIdentityConcurrentRegistrationIsIdempotentAndExclusive(t *testing.T) {
	for _, conflict := range []string{"same-pair", "same-name", "same-token", "case-name"} {
		t.Run(conflict, func(t *testing.T) {
			c, s, h := identityFixture(t)
			start := make(chan struct{})
			codes := make(chan int, 8)
			for i := 0; i < 8; i++ {
				name, token := "Carol", identityToken("carol")
				switch conflict {
				case "same-name":
					token = identityToken(fmt.Sprint(i))
				case "same-token":
					name = fmt.Sprintf("Author %d", i)
				case "case-name":
					token = identityToken(fmt.Sprint(i))
					if i%2 != 0 {
						name = "CAROL"
					}
				}
				body, _ := json.Marshal(map[string]string{"name": name})
				go func() {
					<-start
					codes <- identityRegisterRaw(h, "POST", string(body), []string{"Bearer " + token}).Code
				}()
			}
			close(start)
			created := 0
			for range 8 {
				code := <-codes
				if code == 201 {
					created++
					continue
				}
				want := 409
				if conflict == "same-pair" {
					want = 200
				}
				if code != want {
					t.Errorf("HTTP %d, want %d", code, want)
				}
			}
			if created != 1 || len(identitySnapshot(t, c).Registry.Publishers) != 1 || s.Catalog().Sequence != 40 {
				t.Fatalf("concurrent registrations produced %d created identities", created)
			}
		})
	}
}

func TestIdentityDisableAndStaticRevocation(t *testing.T) {
	c, s, h := identityFixture(t)
	carol, alice := identityToken("carol"), strings.Repeat("a", 40)
	identityRegister(t, h, "Carol", carol, 201)
	identityRegister(t, h, "Alice", alice, 200)
	body := goodPayload(t)
	identityPublish(t, h, identityMetadata("carol-owned"), carol, body, 201)
	identityPublish(t, h, identityMetadata("alice-owned"), alice, body, 201)
	c.SelfRegistration = false
	s = identityReopen(t, c, s)
	h = NewServer(c, s)
	identityRegister(t, h, "Carol", carol, 403)
	m := identityMetadata("carol-owned")
	m.Version = "0.2.0"
	identityPublish(t, h, m, carol, body, 401)
	openPublish(t, h, m, body, 403)
	m.ID = "alice-owned"
	identityPublish(t, h, m, alice, body, 201)
	identityPublish(t, h, identityMetadata("alice-disabled-new"), alice, body, 403)
	// Rotation is read only from config, even after static registration recovery.
	rotated := identityToken("alice-rotated")
	c.SelfRegistration = true
	c.Publishers = append([]Publisher(nil), c.Publishers...)
	c.Publishers[0].TokenSHA256 = TokenHash(rotated)
	s = identityReopen(t, c, s)
	h = NewServer(c, s)
	m.Version = "0.3.0"
	identityPublish(t, h, m, alice, body, 401)
	identityPublish(t, h, m, rotated, body, 201)
	identityPublish(t, h, identityMetadata("carol-owned"), carol, body, 201)
	// Removing Alice leaves ownership reserved but removes her authentication.
	c.Publishers = c.Publishers[1:]
	s = identityReopen(t, c, s)
	h = NewServer(c, s)
	m.Version = "0.4.0"
	identityPublish(t, h, m, rotated, body, 401)
	identityPublish(t, h, m, carol, body, 403)
	openPublish(t, h, m, body, 403)
	identityRegister(t, h, "Alice", identityToken("replacement-alice"), 409)
	identityRegister(t, h, "ALICE", identityToken("replacement-alice-case"), 409)
	identityClaim(t, s, "alice-owned", "Alice")
}

func TestIdentityStartupRejectsConfigConflicts(t *testing.T) {
	for _, kind := range []string{"name", "case-name", "token", "claim-owner", "claim-case"} {
		for _, enabled := range []bool{true, false} {
			t.Run(fmt.Sprintf("%s/enabled-%t", kind, enabled), func(t *testing.T) {
				c, s, h := identityFixture(t)
				token := identityToken("carol")
				identityRegister(t, h, "Carol", token, 201)
				identityPublish(t, h, identityMetadata("owned"), token, goodPayload(t), 201)
				p := Publisher{Name: "Eve", TokenSHA256: TokenHash(identityToken("eve"))}
				switch kind {
				case "name":
					p.Name = "Carol"
				case "case-name":
					p.Name = "CAROL"
				case "token":
					p.TokenSHA256 = TokenHash(token)
				case "claim-owner":
					p.Packages = []string{"owned"}
				case "claim-case":
					p.Packages = []string{"OWNED"}
				}
				c.Publishers = append(c.Publishers, p)
				c.SelfRegistration = enabled
				if err := s.Close(); err != nil {
					t.Fatal(err)
				}
				if reopened, err := Open(c); err == nil {
					reopened.Close()
					t.Fatal("conflicting config accepted at startup")
				}
			})
		}
	}
}

func TestIdentityRegistryTamperAndSemanticValidation(t *testing.T) {
	cases := []struct {
		name   string
		resign bool
		mutate func(*snapshot)
	}{
		{"publisher-name", false, func(v *snapshot) { v.Registry.Publishers[0].Name = "Eve" }},
		{"publisher-token", false, func(v *snapshot) { v.Registry.Publishers[0].TokenSHA256 = TokenHash(identityToken("eve")) }},
		{"claim-owner", false, func(v *snapshot) { v.Registry.Claims[0].Name = "Eve" }},
		{"missing-signature", false, func(v *snapshot) { v.RegistrySignature = nil }},
		{"orphan-signature", false, func(v *snapshot) { v.Registry = nil }},
		{"unknown-version", true, func(v *snapshot) { v.Registry.Version = 2 }},
		{"invalid-name", true, func(v *snapshot) { v.Registry.Publishers[0].Name = " Bad" }},
		{"reserved-author", true, func(v *snapshot) { v.Registry.Publishers[0].Name = OpenPublisherName }},
		{"invalid-digest", true, func(v *snapshot) { v.Registry.Publishers[0].TokenSHA256 = "not-a-digest" }},
		{"uppercase-digest", true, func(v *snapshot) {
			v.Registry.Publishers[0].TokenSHA256 = strings.ToUpper(v.Registry.Publishers[0].TokenSHA256)
		}},
		{"duplicate-name", true, func(v *snapshot) {
			v.Registry.Publishers = append(v.Registry.Publishers, registeredPublisher{Name: "CAROL", TokenSHA256: TokenHash(identityToken("other"))})
		}},
		{"duplicate-token", true, func(v *snapshot) {
			p := v.Registry.Publishers[0]
			p.Name = "Eve"
			v.Registry.Publishers = append(v.Registry.Publishers, p)
		}},
		{"duplicate-claim", true, func(v *snapshot) { v.Registry.Claims = append(v.Registry.Claims, v.Registry.Claims[0]) }},
		{"missing-package", true, func(v *snapshot) { v.Registry.Claims[0].ID = "absent" }},
		{"mismatched-owner", true, func(v *snapshot) { v.Registry.Claims[0].Name = "Eve" }},
		{"missing-claim", true, func(v *snapshot) { v.Registry.Claims = nil }},
		{"reserved-id", true, func(v *snapshot) { v.Registry.Claims[0].ID = "c1pkg" }},
		{"invalid-id", true, func(v *snapshot) { v.Registry.Claims[0].ID = "../escape" }},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			c, s, h := identityFixture(t)
			token := identityToken("carol")
			identityRegister(t, h, "Carol", token, 201)
			identityPublish(t, h, identityMetadata("owned"), token, goodPayload(t), 201)
			v := identitySnapshot(t, c)
			tc.mutate(&v)
			if tc.resign {
				s.signRegistry(&v)
			}
			b, err := json.Marshal(v)
			if err != nil {
				t.Fatal(err)
			}
			if err := s.Close(); err != nil {
				t.Fatal(err)
			}
			if err := os.WriteFile(filepath.Join(c.Root, "current.json"), b, 0600); err != nil {
				t.Fatal(err)
			}
			c.SelfRegistration = false // Validation is mandatory, not feature-gated.
			if reopened, err := Open(c); err == nil {
				reopened.Close()
				t.Fatal("tampered or semantically invalid identity registry accepted")
			}
		})
	}
}

func TestIdentityRegistrySignatureBoundToCatalog(t *testing.T) {
	c, s, h := identityFixture(t)
	token := identityToken("carol")
	identityRegister(t, h, "Carol", token, 201)
	m := identityMetadata("owned")
	identityPublish(t, h, m, token, goodPayload(t), 201)
	old := identitySnapshot(t, c)
	m.Version = "0.2.0"
	identityPublish(t, h, m, token, goodPayload(t), 201)
	current := identitySnapshot(t, c)
	current.Registry, current.RegistrySignature = old.Registry, old.RegistrySignature
	if !ed25519.Verify(s.public, current.Index2, current.Signature2) {
		t.Fatal("test requires a valid newer catalog signature")
	}
	b, err := json.Marshal(current)
	if err != nil {
		t.Fatal(err)
	}
	if err := s.Close(); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(c.Root, "current.json"), b, 0600); err != nil {
		t.Fatal(err)
	}
	if reopened, err := Open(c); err == nil {
		reopened.Close()
		t.Fatal("old registry signature replayed against a different catalog")
	}
}

func TestIdentityRegistrationUnavailableAndPersistenceFailure(t *testing.T) {
	for _, kind := range []string{"read-only", "write-failed", "persist-fails"} {
		t.Run(kind, func(t *testing.T) {
			c, s, h := identityFixture(t)
			before := identitySnapshot(t, c)
			switch kind {
			case "read-only":
				c.SigningKey = ""
				s = identityReopen(t, c, s)
				h = NewServer(c, s)
			case "write-failed":
				s.writeFailed = true
			case "persist-fails":
				s.root = filepath.Join(c.Root, "nonexistent-parent")
			}
			token := identityToken("carol")
			identityRegister(t, h, "Carol", token, 503)
			if _, ok := s.Authenticate(c, token); ok || s.current.Registry != nil || !reflect.DeepEqual(before, identitySnapshot(t, c)) {
				t.Fatal("failed registration became visible or durable")
			}
			if kind == "persist-fails" {
				s.root = c.Root
				identityRegister(t, h, "Carol", token, 503)
				s = identityReopen(t, c, s)
				identityRegister(t, NewServer(c, s), "Carol", token, 201)
			}
		})
	}
}

func TestIdentityRegistrationRateLimit(t *testing.T) {
	_, _, h := identityFixture(t)
	token := identityToken("carol")
	identityRegister(t, h, "Carol", token, 201)
	for i := 1; i < registrationBurst; i++ {
		identityRegister(t, h, "Carol", token, 200)
	}
	w := identityRegister(t, h, "Carol", token, 429)
	if w.Header().Get("Retry-After") == "" {
		t.Fatal("rate limit omitted retry advice")
	}
	// Advance only the bucket's test clock; no sleep or external requests.
	h.registrationMu.Lock()
	h.registrationTime = time.Now().Add(-registrationInterval)
	h.registrationMu.Unlock()
	identityRegister(t, h, "Carol", token, 200)
	identityRegister(t, h, "Carol", token, 429)
}
