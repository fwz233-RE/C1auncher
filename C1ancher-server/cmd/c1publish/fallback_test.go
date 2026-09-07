package main

import (
	"io"
	"net/http"
	"strings"
	"testing"
)

type testTransport func(*http.Request) (*http.Response, error)

func (f testTransport) RoundTrip(r *http.Request) (*http.Response, error) { return f(r) }
func TestOfficialFallback(t *testing.T) {
	for _, code := range []int{403, 404, 308, 502, 503} {
		calls := 0
		transport := &fallbackTransport{base: testTransport(func(r *http.Request) (*http.Response, error) {
			calls++
			b, _ := io.ReadAll(r.Body)
			if string(b) != "payload" || r.Header.Get("Authorization") != "Bearer test-only" {
				t.Fatal("lost publication")
			}
			status := code
			if calls == 2 {
				if r.URL.Host != "123.56.214.77" || r.URL.Scheme != "http" || r.URL.Path != "/api/v1/publish" {
					t.Fatal("wrong fallback")
				}
				status = 201
			}
			return &http.Response{StatusCode: status, Body: io.NopCloser(strings.NewReader("ok")), Header: make(http.Header)}, nil
		})}
		r, _ := http.NewRequest("POST", "http://www.fwz233.com/api/v1/publish", strings.NewReader("payload"))
		r.Header.Set("Authorization", "Bearer test-only")
		response, err := transport.RoundTrip(r)
		if err != nil || response.StatusCode != 201 || calls != 2 {
			t.Fatalf("fallback failed for %d", code)
		}
		response.Body.Close()
	}
}
func TestNeverFallbackOtherOriginsOrConflicts(t *testing.T) {
	for _, origin := range []string{"http://www.fwz233.com.evil.test", "https://www.fwz233.com", "http://127.0.0.1", "http://www.fwz233.com"} {
		calls := 0
		transport := &fallbackTransport{base: testTransport(func(r *http.Request) (*http.Response, error) {
			calls++
			return &http.Response{StatusCode: 409, Body: io.NopCloser(strings.NewReader("conflict"))}, nil
		})}
		r, _ := http.NewRequest("GET", origin+"/api/v1/catalog", nil)
		response, _ := transport.RoundTrip(r)
		response.Body.Close()
		if calls != 1 {
			t.Fatal("unexpected fallback")
		}
	}
}
