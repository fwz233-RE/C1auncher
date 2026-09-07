package repository

import (
	"bytes"
	"encoding/json"
	"strings"
	"testing"
)

func TestUTF8LabelPolicy(t *testing.T) {
	valid := []string{"App", "中文软件", "張三 / 王小明", "阅读器 Reader", "Café", "内部　空格", "𠮷野家", strings.Repeat("A", 40), strings.Repeat("中", 13) + "A"}
	invalid := []string{
		"", " leading", "trailing ", "　中文", "中文　", "\u00a0App",
		"bad\tfield", "bad\nfield", "bad\x01", "bad\x7f", "bad\u0085",
		"\x80", "\xc0\xaf", "\xc1\xbf", "\xc2", "\xe4\xb8", "\xe0\x80\x80",
		"\xed\xa0\x80", "\xf0\x80\x80\x80", "\xf4\x90\x80\x80", "\xf5\x80\x80\x80", "\xff",
		"A\u202eB", "A\u2066B", "A\u061cB", "A\u200bB", "A\ufeffB", "A\u2028B",
		"A\ufdd0B", "A\uffffB", "A\ufffdB", "A\U000e0100B", "A\ufe0fB",
		strings.Repeat("A", 41), strings.Repeat("中", 13) + "AB", strings.Repeat("中", 14),
	}
	for _, s := range valid {
		if !label(s) {
			t.Errorf("valid label rejected: %q (%d bytes)", s, len(s))
		}
		m := Metadata{ID: "reader", Version: "1.0.0", Name: s, Entry: "bin/reader"}
		if err := m.Validate(); err != nil {
			t.Errorf("valid UTF-8 metadata rejected: %q: %v", s, err)
		}
	}
	for _, s := range invalid {
		if label(s) {
			t.Errorf("invalid label accepted: %q", s)
		}
		m := Metadata{ID: "reader", Version: "1.0.0", Name: s, Entry: "bin/reader"}
		if err := m.Validate(); err == nil {
			t.Errorf("invalid UTF-8 metadata accepted: %q", s)
		}
	}
}

func TestUTF8LabelJSONAndIndex(t *testing.T) {
	const name, author = "中文阅读器", "张三 / 王小明"
	var m Metadata
	if err := json.Unmarshal([]byte(`{"id":"reader","version":"1.0.0","name":"中文阅读器","entry":"bin/reader"}`), &m); err != nil {
		t.Fatal(err)
	}
	if err := m.Validate(); err != nil {
		t.Fatal(err)
	}
	if m.Name != name || !label(author) {
		t.Fatal("UTF-8 label changed")
	}
	catalog := Catalog{Sequence: 1, Packages: []Package{{Metadata: m, Author: author, Archive: "reader.tar.gz", SHA256: strings.Repeat("a", 64), Size: 100}}}
	for _, version := range []int{1, 2} {
		wire := index(catalog, version)
		if !bytes.Contains(wire, []byte("\t"+name+"\t")) {
			t.Fatalf("index v%d did not preserve UTF-8 name: %q", version, wire)
		}
		if version == 2 && !bytes.HasSuffix(wire, []byte("\t"+author+"\n")) {
			t.Fatalf("index v2 did not preserve UTF-8 author: %q", wire)
		}
	}
	// encoding/json repairs invalid UTF-8 and unpaired surrogate escapes with
	// U+FFFD; label validation must reject the repaired metadata too.
	for _, bad := range []string{"\xff", `\ud800`, `\udfff`} {
		raw := []byte(`{"id":"reader","version":"1.0.0","name":"` + bad + `","entry":"bin/reader"}`)
		if err := json.Unmarshal(raw, &m); err != nil {
			t.Fatal(err)
		}
		if err := m.Validate(); err == nil {
			t.Fatalf("JSON-repaired malformed label accepted: %q", bad)
		}
	}
}
