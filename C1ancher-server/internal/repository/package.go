package repository

import (
	"archive/tar"
	"bytes"
	"compress/gzip"
	"debug/elf"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"sort"
	"time"
)

// ValidateELF never executes uploaded code. Dynamic binaries need a separately
// specified runtime contract and are deliberately excluded from this repository.
func ValidateELF(data []byte) error {
	f, err := elf.NewFile(bytes.NewReader(data))
	if err != nil {
		return errors.New("entry is not ELF")
	}
	defer f.Close()
	if f.Class != elf.ELFCLASS32 || f.Data != elf.ELFDATA2LSB || f.Machine != elf.EM_MIPS || f.Type != elf.ET_EXEC {
		return errors.New("entry must be an ELF32 little-endian MIPS executable")
	}
	if len(data) < 52 {
		return errors.New("short ELF")
	}
	flags := binary.LittleEndian.Uint32(data[36:40])
	// MIPS32 and MIPS32r2 o32, not MIPS64, microMIPS, or MIPS16.
	isa := flags & 0xf0000000
	if (isa != 0x50000000 && isa != 0x70000000) || flags&0x0000f000 != 0x1000 || flags&0x06000000 != 0 {
		return errors.New("entry requires unsupported MIPS ISA or ABI")
	}
	executable := false
	for _, p := range f.Progs {
		if p.Type == elf.PT_INTERP || p.Type == elf.PT_DYNAMIC {
			return errors.New("entry must be statically linked")
		}
		if p.Type == elf.PT_LOAD && p.Flags&elf.PF_X != 0 {
			executable = true
		}
	}
	if !executable {
		return errors.New("entry has no executable segment")
	}
	// Require explicit ABI evidence rather than accepting an unmarked soft-float
	// executable. Cap the section before reading untrusted ELF section sizes.
	s := f.Section(".MIPS.abiflags")
	if s == nil || s.Size != 24 {
		return errors.New("entry lacks a supported .MIPS.abiflags section")
	}
	b, e := s.Data()
	if e != nil || len(b) != 24 || b[0] != 0 || b[1] != 0 || b[2] != 32 || (b[3] != 1 && b[3] != 2) || b[7] != 1 || b[4] != 1 || b[5] != 1 {
		return errors.New("entry must use MIPS32/r2 o32 double-precision hard-float")
	}
	return nil
}

// BuildPackage accepts a tar.gz of payload files, creates the trusted manifest,
// and strips all uploader-supplied owners/modes/timestamps. Links, devices,
// traversal, reserved metadata, duplicates and decompression bombs are rejected.
func BuildPackage(m Metadata, input io.Reader) ([]byte, error) {
	if err := m.Validate(); err != nil {
		return nil, err
	}
	compressed := &io.LimitedReader{R: input, N: MaxArchive + 1}
	gz, err := gzip.NewReader(compressed)
	if err != nil {
		return nil, errors.New("invalid gzip upload")
	}
	defer gz.Close()
	expanded := &io.LimitedReader{R: gz, N: MaxUnpacked + (2 << 20) + 1}
	tr := tar.NewReader(expanded)
	files := map[string][]byte{}
	seen := map[string]bool{}
	total := int64(0)
	records := 0
	for {
		h, e := tr.Next()
		if e == io.EOF {
			break
		}
		if e != nil {
			return nil, errors.New("invalid tar upload")
		}
		records++
		if records > 2048 {
			return nil, errors.New("too many archive entries")
		}
		name := h.Name
		if h.Typeflag == tar.TypeDir {
			name = bytesToStringTrimSlash(name)
		}
		if !safePath(name) || seen[name] {
			return nil, errors.New("unsafe or duplicate payload path")
		}
		seen[name] = true
		if h.Typeflag == tar.TypeDir {
			continue
		}
		if h.Typeflag != tar.TypeReg || h.Size < 0 || h.Size > MaxFile || total+h.Size > MaxUnpacked || len(files) >= 1024 {
			return nil, errors.New("unsupported file or payload limit exceeded")
		}
		b, e := io.ReadAll(io.LimitReader(tr, h.Size+1))
		if e != nil || int64(len(b)) != h.Size {
			return nil, errors.New("truncated payload")
		}
		files[name] = b
		total += h.Size
	}
	// Consume the trailer so CRC/truncation/extra compressed data cannot hide.
	if _, err = io.Copy(io.Discard, expanded); err != nil || expanded.N == 0 || compressed.N == 0 {
		return nil, errors.New("invalid or oversized gzip stream")
	}
	entry, ok := files[m.Entry]
	if !ok {
		return nil, errors.New("entry not found")
	}
	if err = ValidateELF(entry); err != nil {
		return nil, err
	}
	for name := range files {
		for parent := pathParent(name); parent != "."; parent = pathParent(parent) {
			if _, ok := files[parent]; ok {
				return nil, errors.New("file conflicts with directory")
			}
		}
	}
	var out bytes.Buffer
	zw := gzip.NewWriter(&out)
	tw := tar.NewWriter(zw)
	write := func(name string, b []byte, mode int64) error {
		if e := tw.WriteHeader(&tar.Header{Name: name, Mode: mode, Size: int64(len(b)), Typeflag: tar.TypeReg, ModTime: time.Unix(0, 0), Format: tar.FormatGNU}); e != nil {
			return e
		}
		_, e := tw.Write(b)
		return e
	}
	manifest := fmt.Sprintf("C1PKG-PACKAGE 1\nid\t%s\nversion\t%s\nentry\t%s\n", m.ID, m.Version, m.Entry)
	if m.Mode != "" {
		manifest = fmt.Sprintf("C1PKG-PACKAGE 2\nid\t%s\nversion\t%s\nentry\t%s\nmode\t%s\n", m.ID, m.Version, m.Entry, m.Mode)
	}
	if err = write("manifest.v1", []byte(manifest), 0644); err != nil {
		return nil, err
	}
	names := make([]string, 0, len(files))
	for n := range files {
		names = append(names, n)
	}
	sort.Strings(names)
	for _, n := range names {
		mode := int64(0644)
		if n == m.Entry {
			mode = 0755
		}
		if err = write("payload/"+n, files[n], mode); err != nil {
			return nil, err
		}
	}
	if err = tw.Close(); err != nil {
		return nil, err
	}
	if err = zw.Close(); err != nil {
		return nil, err
	}
	if out.Len() > MaxArchive {
		return nil, errors.New("built package exceeds 32 MiB")
	}
	return out.Bytes(), nil
}
func bytesToStringTrimSlash(s string) string {
	if len(s) > 0 && s[len(s)-1] == '/' {
		return s[:len(s)-1]
	}
	return s
}
func pathParent(s string) string {
	for i := len(s) - 1; i >= 0; i-- {
		if s[i] == '/' {
			return s[:i]
		}
	}
	return "."
}
