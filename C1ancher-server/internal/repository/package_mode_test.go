package repository

import (
	"archive/tar"
	"bytes"
	"compress/gzip"
	"encoding/json"
	"io"
	"strings"
	"testing"
)

func TestPackageLaunchMode(t *testing.T) {
	for _, mode := range []string{"", "terminal", "direct"} {
		t.Run("mode_"+mode, func(t *testing.T) {
			m := meta()
			m.Mode = mode
			archive, err := BuildPackage(m, bytes.NewReader(goodPayload(t)))
			if err != nil {
				t.Fatal(err)
			}
			gz, err := gzip.NewReader(bytes.NewReader(archive))
			if err != nil {
				t.Fatal(err)
			}
			defer gz.Close()
			tr := tar.NewReader(gz)
			header, err := tr.Next()
			if err != nil || header.Name != "manifest.v1" {
				t.Fatalf("manifest header: %v %v", header, err)
			}
			body, err := io.ReadAll(tr)
			if err != nil {
				t.Fatal(err)
			}
			want := "C1PKG-PACKAGE 1\nid\thello\nversion\t0.1.0\nentry\thello\n"
			if mode != "" {
				want = "C1PKG-PACKAGE 2\nid\thello\nversion\t0.1.0\nentry\thello\nmode\t" + mode + "\n"
			}
			if string(body) != want {
				t.Fatalf("manifest = %q; want %q", body, want)
			}
			if mode == "" {
				encoded, err := json.Marshal(m)
				if err != nil || bytes.Contains(encoded, []byte("mode")) {
					t.Fatalf("legacy metadata changed: %s %v", encoded, err)
				}
			}
		})
	}
	for _, mode := range []string{"auto", "Terminal", "direct\nentry\tevil", " terminal"} {
		m := meta()
		m.Mode = mode
		if _, err := BuildPackage(m, bytes.NewReader(goodPayload(t))); err == nil {
			t.Fatalf("accepted invalid mode %q", mode)
		}
	}
}

func TestPublishModeIdempotenceAndRestart(t *testing.T) {
	c, s := fixture(t)
	h := NewServer(c, s)
	m := meta()
	m.Mode = "terminal"
	body := goodPayload(t)
	for i := 0; i < 2; i++ {
		if w := publishRequest(t, h, m, strings.Repeat("a", 40), body); w.Code != 201 {
			t.Fatal(w.Code, w.Body.String())
		}
	}
	if s.Catalog().Sequence != 41 {
		t.Fatal("same-mode retry changed sequence")
	}
	m.Mode = "direct"
	if w := publishRequest(t, h, m, strings.Repeat("a", 40), body); w.Code != 409 {
		t.Fatal("same-version mode change accepted", w.Code)
	}
	s.Close()
	reopened, err := Open(c)
	if err != nil {
		t.Fatal(err)
	}
	defer reopened.Close()
	if reopened.Catalog().Sequence != 41 || len(reopened.Catalog().Packages) != 1 || reopened.Catalog().Packages[0].Mode != "terminal" {
		t.Fatal("mode package lost across restart")
	}
}

func TestReservedLaunchMetadataRejected(t *testing.T) {
	for _, name := range []string{".c1pkg-mode", ".c1pkg-entry", "assets/.c1pkg-mode"} {
		m := meta()
		m.Mode = "terminal"
		b := elfFixture()
		p := payload(t, []*tar.Header{
			{Name: "hello", Typeflag: tar.TypeReg, Mode: 0755, Size: int64(len(b))},
			{Name: name, Typeflag: tar.TypeReg, Mode: 0644, Size: 6},
		}, [][]byte{b, []byte("direct")})
		if _, err := BuildPackage(m, bytes.NewReader(p)); err == nil {
			t.Fatalf("accepted reserved path %s", name)
		}
	}
}
