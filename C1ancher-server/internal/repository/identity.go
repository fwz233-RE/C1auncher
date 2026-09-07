package repository

import (
	"bytes"
	"crypto/ed25519"
	"crypto/subtle"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"os"
	"strings"
	"time"
)

const (
	maxRegisteredPublishers = 4096
	maxSnapshotBytes        = 2 << 20
	registrationBurst       = 20
	registrationInterval    = 3 * time.Second
)

// The registry is private state, committed WITH the catalog, never served in an
// API. Static credentials remain exclusively in config (no revocation bypass).
type registeredPublisher struct {
	Name        string `json:"name"`
	TokenSHA256 string `json:"tokenSHA256"`
}
type applicationClaim struct {
	ID   string `json:"id"`
	Name string `json:"name"`
}
type identityRegistry struct {
	Version    int                   `json:"version"`
	Publishers []registeredPublisher `json:"publishers"`
	Claims     []applicationClaim    `json:"claims"`
}

var (
	ErrIdentityConflict = errors.New("publisher name or token already registered")
	ErrIdentityCapacity = errors.New("publisher registry full")
	ErrUnavailable      = errors.New("repository is read-only or persistence failed")
	ErrUnauthorized     = errors.New("unauthorized")
)

func validPublisherName(name string) bool {
	return label(name) && !strings.EqualFold(name, OpenPublisherName)
}

func readSnapshot(file string) ([]byte, error) {
	f, err := os.Open(file)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	b, err := io.ReadAll(io.LimitReader(f, maxSnapshotBytes+1))
	if err != nil {
		return nil, err
	}
	if len(b) > maxSnapshotBytes {
		return nil, errors.New("stored snapshot too large")
	}
	return b, nil
}

func registryMessage(v snapshot) []byte {
	// Domain separation plus the signed catalog binds ownership to this exact
	// publication. Device index formats and their signatures stay unchanged.
	b, _ := json.Marshal(struct {
		Domain   string            `json:"domain"`
		Registry *identityRegistry `json:"registry"`
		Index    []byte            `json:"index"`
	}{"c1repo-identities-v1", v.Registry, v.Index2})
	return b
}
func (s *Store) signRegistry(v *snapshot) {
	if v.Registry != nil {
		v.RegistrySignature = ed25519.Sign(s.key, registryMessage(*v))
	}
}
func cloneRegistry(v *identityRegistry) *identityRegistry {
	if v == nil {
		return &identityRegistry{Version: 1}
	}
	return &identityRegistry{Version: v.Version,
		Publishers: append([]registeredPublisher(nil), v.Publishers...),
		Claims:     append([]applicationClaim(nil), v.Claims...)}
}

// validateRegistry runs even when selfRegistration is disabled. Conflicting
// config edits or corrupt/unknown registry versions must stop startup.
func (s *Store) validateRegistry(c Config) error {
	v := s.current
	r := v.Registry
	bad := errors.New("invalid or conflicting stored identity registry")
	if r == nil {
		if len(v.RegistrySignature) != 0 {
			return bad
		}
		return nil // Pre-feature snapshots need no migration command.
	}
	if r.Version != 1 || len(r.Publishers) > maxRegisteredPublishers || len(r.Claims) > MaxPackages ||
		!ed25519.Verify(s.public, registryMessage(v), v.RegistrySignature) {
		return bad
	}
	for i, p := range r.Publishers {
		digest, err := hex.DecodeString(p.TokenSHA256)
		if !validPublisherName(p.Name) || err != nil || len(digest) != 32 || strings.ToLower(p.TokenSHA256) != p.TokenSHA256 {
			return bad
		}
		for _, other := range r.Publishers[:i] {
			if strings.EqualFold(p.Name, other.Name) || p.TokenSHA256 == other.TokenSHA256 {
				return bad
			}
		}
		for _, other := range c.Publishers {
			if strings.EqualFold(p.Name, other.Name) || p.TokenSHA256 == other.TokenSHA256 {
				return bad
			}
		}
	}
	for i, claim := range r.Claims {
		if !validID(claim.ID) || reserved[strings.ToLower(claim.ID)] || !validPublisherName(claim.Name) {
			return bad
		}
		for _, other := range r.Claims[:i] {
			if strings.EqualFold(claim.ID, other.ID) {
				return bad
			}
		}
		found := false
		for _, p := range v.Catalog.Packages {
			if p.ID == claim.ID && p.Author == claim.Name {
				found = true
			}
		}
		if !found {
			return bad
		}
		for _, p := range c.Publishers {
			for _, id := range p.Packages {
				if strings.EqualFold(id, claim.ID) && (id != claim.ID || p.Name != claim.Name) {
					return bad
				}
			}
		}
		for _, p := range r.Publishers {
			if strings.EqualFold(p.Name, claim.Name) && p.Name != claim.Name {
				return bad
			}
		}
	}
	for _, p := range v.Catalog.Packages {
		for _, user := range r.Publishers {
			if !strings.EqualFold(p.Author, user.Name) {
				continue
			}
			found := false
			for _, claim := range r.Claims {
				if claim.ID == p.ID && claim.Name == user.Name && p.Author == user.Name {
					found = true
				}
			}
			if !found {
				return bad
			}
		}
	}
	return nil
}

func (s *Store) authenticateLocked(c Config, token string) (Publisher, bool) {
	if p, ok := c.Authenticate(token); ok {
		return p, true
	}
	if !c.SelfRegistration || len(token) < 32 || len(token) > 256 || s.current.Registry == nil {
		return Publisher{}, false
	}
	digest := TokenHash(token)
	for _, p := range s.current.Registry.Publishers {
		if subtle.ConstantTimeCompare([]byte(digest), []byte(p.TokenSHA256)) == 1 {
			return Publisher{Name: p.Name}, true
		}
	}
	return Publisher{}, false
}
func (s *Store) Authenticate(c Config, token string) (Publisher, bool) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	return s.authenticateLocked(c, token)
}

// authorizeLocked is checked again after archive validation under the commit
// mutex: neither concurrent registration nor publication can steal an ID.
func (s *Store) authorizeLocked(c Config, p Publisher, id string, open bool) error {
	if r := s.current.Registry; r != nil {
		for _, claim := range r.Claims {
			if strings.EqualFold(claim.ID, id) {
				if open || claim.ID != id || claim.Name != p.Name {
					return ErrProtected
				}
				// Config may not assign this claim to another publisher.
				for _, owner := range c.Publishers {
					for _, assigned := range owner.Packages {
						if strings.EqualFold(assigned, id) && (assigned != id || owner.Name != p.Name) {
							return ErrProtected
						}
					}
				}
				return nil
			}
		}
	}
	for _, owner := range c.Publishers {
		for _, assigned := range owner.Packages {
			if strings.EqualFold(assigned, id) {
				if open || assigned != id || !p.Owns(id) {
					return ErrProtected
				}
				return nil // Existing, explicit static ownership is authoritative.
			}
		}
	}
	if open {
		return nil
	} // publish() still protects signed historical authors.
	if !c.SelfRegistration {
		return ErrProtected
	}
	for _, old := range s.current.Catalog.Packages {
		if strings.EqualFold(old.ID, id) {
			return ErrProtected
		}
	}
	return nil // Only an UNUSED ID can acquire an automatic claim.
}
func (s *Store) AuthorizePublish(c Config, p Publisher, id string, open bool) error {
	s.mu.RLock()
	defer s.mu.RUnlock()
	return s.authorizeLocked(c, p, id, open)
}

// register is serialized with publishing and uses precisely the same atomic
// commit. Its argument is already a digest; plaintext tokens never enter state.
func (s *Store) register(c Config, name, digest string) (bool, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if len(s.key) == 0 || s.writeFailed {
		return false, ErrUnavailable
	}
	for _, p := range c.Publishers {
		if p.TokenSHA256 == digest {
			if p.Name == name {
				return false, nil
			}
			return false, ErrIdentityConflict
		}
	}
	if r := s.current.Registry; r != nil {
		for _, p := range r.Publishers {
			if p.TokenSHA256 == digest {
				if p.Name == name {
					return false, nil
				}
				return false, ErrIdentityConflict
			}
		}
		for _, p := range r.Publishers {
			if strings.EqualFold(p.Name, name) {
				return false, ErrIdentityConflict
			}
		}
		for _, claim := range r.Claims {
			if strings.EqualFold(claim.Name, name) {
				return false, ErrIdentityConflict
			}
		}
	}
	for _, p := range c.Publishers {
		if strings.EqualFold(p.Name, name) {
			return false, ErrIdentityConflict
		}
	}
	for _, p := range s.current.Catalog.Packages {
		if strings.EqualFold(p.Author, name) {
			return false, ErrIdentityConflict
		}
	}
	r := cloneRegistry(s.current.Registry)
	if len(r.Publishers) >= maxRegisteredPublishers {
		return false, ErrIdentityCapacity
	}
	r.Publishers = append(r.Publishers, registeredPublisher{Name: name, TokenSHA256: digest})
	next := s.current
	next.Registry = r
	s.signRegistry(&next)
	if err := s.persist(next); err != nil {
		s.writeFailed = true // Rename might have committed before fsync failed.
		return false, ErrUnavailable
	}
	s.current = next
	return true, nil
}

// A global bucket bounds total work and memory, including behind a loopback
// reverse proxy. No forwarding headers or attacker-controlled IP map are used.
func (s *Server) allowRegistration(now time.Time) bool {
	s.registrationMu.Lock()
	defer s.registrationMu.Unlock()
	if s.registrationTime.IsZero() {
		s.registrationTime = now
		s.registrationTokens = registrationBurst
	}
	elapsed := now.Sub(s.registrationTime)
	if elapsed >= registrationInterval {
		n := int(elapsed / registrationInterval)
		s.registrationTokens += n
		if s.registrationTokens > registrationBurst {
			s.registrationTokens = registrationBurst
		}
		s.registrationTime = now
	}
	if s.registrationTokens == 0 {
		return false
	}
	s.registrationTokens--
	return true
}
func (s *Server) registerPublisher(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		w.Header().Set("Allow", "POST")
		http.Error(w, "method not allowed", 405)
		return
	}
	if !s.config.SelfRegistration {
		http.Error(w, "self registration is disabled", 403)
		return
	}
	s.store.mu.RLock()
	unavailable := len(s.store.key) == 0 || s.store.writeFailed
	s.store.mu.RUnlock()
	if unavailable {
		http.Error(w, ErrUnavailable.Error(), 503)
		return
	}
	if !s.allowRegistration(time.Now()) {
		w.Header().Set("Retry-After", "3")
		http.Error(w, "registration rate limit reached", 429)
		return
	}
	values := r.Header.Values("Authorization")
	if len(values) != 1 || !strings.HasPrefix(values[0], "Bearer ") {
		http.Error(w, "unauthorized", 401)
		return
	}
	token := strings.TrimPrefix(values[0], "Bearer ")
	// An existing static credential can recover its registration response even
	// if it predates the new canonical 32-byte base64url format.
	_, legacy := s.config.Authenticate(token)
	decoded, err := base64.RawURLEncoding.Strict().DecodeString(token)
	if !legacy && (err != nil || len(decoded) != 32 || base64.RawURLEncoding.EncodeToString(decoded) != token) {
		http.Error(w, "expected a 32-byte random base64url bearer token", 401)
		return
	}
	r.Body = http.MaxBytesReader(w, r.Body, 1024)
	defer r.Body.Close()
	b, err := io.ReadAll(r.Body)
	var body struct {
		Name string `json:"name"`
	}
	d := json.NewDecoder(bytes.NewReader(b))
	d.DisallowUnknownFields()
	if err != nil || d.Decode(&body) != nil || d.Decode(new(any)) != io.EOF || !validPublisherName(body.Name) {
		http.Error(w, "invalid publisher name or registration JSON", 400)
		return
	}
	created, err := s.store.register(s.config, body.Name, TokenHash(token))
	switch {
	case errors.Is(err, ErrIdentityConflict):
		http.Error(w, err.Error(), 409)
	case err != nil:
		http.Error(w, "registration unavailable; retry the same name and token", 503)
	default:
		code := 200
		if created {
			code = 201
		}
		jsonResponse(w, code, struct {
			Name string `json:"name"`
		}{body.Name})
	}
}
