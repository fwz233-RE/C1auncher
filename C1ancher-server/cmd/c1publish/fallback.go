package main

import "net/http"

// Fallback is restricted to the operator's fixed official origin. The same
// immutable publication can be retried after a lost response. Never follow a
// server-selected redirect with the developer's credentials.
type fallbackTransport struct{ base http.RoundTripper }

func (t *fallbackTransport) RoundTrip(r *http.Request) (*http.Response, error) {
	response, err := t.base.RoundTrip(r)
	if r.URL.Scheme != "http" || (r.URL.Host != "www.fwz233.com" && r.URL.Host != "www.fwz233.com:80") || r.Context().Err() != nil {
		return response, err
	}
	if err == nil && !(response.StatusCode == 403 || response.StatusCode == 404 || response.StatusCode >= 500 || response.StatusCode >= 300 && response.StatusCode < 400) {
		return response, nil
	}
	if r.Body != nil && r.GetBody == nil {
		return response, err
	}
	retry := r.Clone(r.Context())
	u := *r.URL
	u.Host = "123.56.214.77"
	retry.URL = &u
	retry.Host = ""
	if r.Body != nil {
		body, e := r.GetBody()
		if e != nil {
			return response, err
		}
		retry.Body = body
	}
	if response != nil {
		response.Body.Close()
	}
	return t.base.RoundTrip(retry)
}
