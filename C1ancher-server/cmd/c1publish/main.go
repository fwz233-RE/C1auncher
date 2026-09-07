// c1publish uploads compiled payloads without SSH access or signing keys.
package main

import (
	"archive/tar"
	"c1repo/internal/repository"
	"compress/gzip"
	"encoding/base64"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"io/fs"
	"net"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"time"
)

const publisherVersion = "1.1.0"

func main() {
	if len(os.Args) == 1 {
		if err := wizard(os.Stdin, os.Stdout); err != nil {
			fmt.Fprintln(os.Stderr, "c1publish:", err)
			os.Exit(1)
		}
		return
	}
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, "c1publish:", err)
		os.Exit(1)
	}
}
func run() error {
	return runArgs(os.Args[1:])
}

func runArgs(args []string) error {
	executable, err := os.Executable()
	if err != nil {
		return err
	}
	return runArgsInDir(args, filepath.Dir(executable))
}

func runArgsInDir(args []string, directory string) error {
	return runArgsWithOutput(args, directory, os.Stdout)
}

func runArgsWithOutput(args []string, directory string, output io.Writer) error {
	flags := flag.NewFlagSet("c1publish", flag.ContinueOnError)
	flags.SetOutput(output)
	server := flags.String("server", "", "repository origin; default: executable-adjacent server.url, otherwise http://www.fwz233.com")
	publicKey := flags.String("public-key", "", "trusted raw Ed25519 public key for catalog verification only; default: executable-adjacent repository.ed25519.pub; never an upload credential")
	allowHTTP := flags.Bool("allow-insecure-http", false, "explicitly accept public HTTP tampering risk (and plaintext token exposure in token mode)")
	open := flags.Bool("open", false, "explicit anonymous publishing; requires server publishMode=open; no token or public-key credential; anyone may update open apps")
	register := flags.Bool("register", false, "self-register an author; creates token file before sending; retry with the same file after network failure")
	author := flags.String("author", "", "unique author name, 1-40 UTF-8 bytes, for -register; self-selected name, not verified identity")
	toolVersion := flags.Bool("tool-version", false, "print publisher tool version and exit")
	tokenFile := flags.String("token-file", "", "publisher token file; created exclusively by -register, reused for publishing; mutually exclusive with -open")
	id := flags.String("id", "", "application ID; first successful authenticated publication owns a new unassigned ID")
	version := flags.String("version", "", "numeric version also embedded in your executable")
	name := flags.String("name", "", "display name, 1-40 UTF-8 bytes; no controls or invisible characters")
	entry := flags.String("entry", "", "entry relative to payload directory")
	mode := flags.String("mode", "", "optional terminal or direct launch mode; requires package-v2 server and device support")
	payload := flags.String("payload", "", "directory with binary and assets")
	binaryFile := flags.String("binary", "", "upload one binary instead of a directory")
	next := flags.Bool("next-version", false, "print suggested next version; no upload or reservation")
	if err := flags.Parse(args); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			return nil
		}
		return err
	}
	if flags.NArg() != 0 {
		return errors.New("unexpected positional arguments")
	}
	if *toolVersion {
		fmt.Fprintln(output, "C1-Slim Publisher "+publisherVersion)
		return nil
	}
	if *register {
		if *open || *next || *author == "" || *tokenFile == "" || *id != "" || *version != "" || *name != "" || *entry != "" || *mode != "" || *payload != "" || *binaryFile != "" {
			return errors.New("-register requires -author and -token-file, and cannot be combined with publishing or -next-version")
		}
		if err := validateAuthor(*author); err != nil {
			return err
		}
	} else if *author != "" {
		return errors.New("-author is only valid with -register; publishing author comes from your token")
	}
	if *open && *tokenFile != "" {
		return errors.New("-open and -token-file are mutually exclusive; open uploads send no credentials")
	}
	if !*next && !*open && *tokenFile == "" {
		return errors.New("provide -token-file to publish and update your own application IDs, or -open for explicit anonymous publishing")
	}
	configuredOrigin, trustKey, err := loadPublicConfig(directory, *server, *publicKey)
	if err != nil {
		return err
	}
	*server = configuredOrigin
	u, err := url.Parse(*server)
	if err != nil || u.Host == "" || u.User != nil || u.RawQuery != "" || u.Fragment != "" || (u.Path != "" && u.Path != "/") {
		return errors.New("server must be an origin without path, credentials or query")
	}
	ip := net.ParseIP(u.Hostname())
	if u.Scheme != "https" && !(u.Scheme == "http" && (*next || *allowHTTP || (ip != nil && ip.IsLoopback()))) {
		return errors.New("public HTTP allows upload tampering and exposes tokens in token mode; explicitly pass -allow-insecure-http to accept this risk")
	}
	client := &http.Client{Transport: &fallbackTransport{base: &http.Transport{Proxy: http.ProxyFromEnvironment, DialContext: (&net.Dialer{Timeout: 10 * time.Second}).DialContext, ResponseHeaderTimeout: 120 * time.Second}}, Timeout: 10 * time.Minute, CheckRedirect: func(r *http.Request, via []*http.Request) error {
		return errors.New("redirect refused; use the canonical server origin")
	}}
	origin := strings.TrimRight(*server, "/")
	if *register {
		// Signature verification detects the wrong catalog key, but does not
		// make registration over plaintext HTTP safe from interception.
		if len(trustKey) != 0 {
			if _, err = verifiedCatalog(client, origin, trustKey); err != nil {
				return err
			}
		}
		return registerPublisherWithOutput(client, origin, *author, *tokenFile, output)
	}
	if *next && len(trustKey) != 0 {
		catalog, e := verifiedCatalog(client, origin, trustKey)
		if e != nil {
			return e
		}
		current := ""
		for _, p := range catalog.Packages {
			if p.ID == *id {
				current = p.Version
			}
		}
		v, e := repository.NextVersion(current)
		if e != nil {
			return e
		}
		fmt.Fprintln(output, v)
		return nil
	}
	if *next {
		resp, e := client.Get(origin + "/api/v1/catalog")
		if e != nil {
			return e
		}
		defer resp.Body.Close()
		if resp.StatusCode != 200 {
			return fmt.Errorf("catalog request: HTTP %d", resp.StatusCode)
		}
		var c repository.Catalog
		if e = json.NewDecoder(io.LimitReader(resp.Body, 256<<10)).Decode(&c); e != nil {
			return e
		}
		current := ""
		for _, p := range c.Packages {
			if p.ID == *id {
				current = p.Version
			}
		}
		v, e := repository.NextVersion(current)
		if e != nil {
			return e
		}
		fmt.Fprintln(output, v)
		return nil
	}
	if (*payload == "") == (*binaryFile == "") {
		return errors.New("provide exactly one of -payload or -binary")
	}
	if *entry == "" && *binaryFile != "" {
		*entry = filepath.Base(*binaryFile)
	}
	m := repository.Metadata{ID: *id, Version: *version, Name: *name, Entry: *entry, Mode: *mode}
	if err = m.Validate(); err != nil {
		return err
	}
	secret := ""
	var tokenInfo os.FileInfo
	if !*open {
		secret, err = readToken(*tokenFile)
		if err != nil {
			return err
		}
		tokenInfo, err = os.Stat(*tokenFile)
		if err != nil {
			return errors.New("cannot inspect token file")
		}
	}
	archive, err := os.CreateTemp("", "c1-upload-*.tar.gz")
	if err != nil {
		return err
	}
	defer os.Remove(archive.Name())
	defer archive.Close()
	gz := gzip.NewWriter(archive)
	tw := tar.NewWriter(gz)
	total := int64(0)
	count := 0
	add := func(file, relative string) error {
		info, e := os.Lstat(file)
		if e != nil {
			return e
		}
		if tokenInfo != nil && os.SameFile(info, tokenInfo) {
			return errors.New("payload contains your publisher token file; move it outside the payload before publishing")
		}
		if !info.Mode().IsRegular() {
			return errors.New("payload must contain only regular files/directories, no links")
		}
		count++
		total += info.Size()
		if count > 1024 || info.Size() > repository.MaxFile || total > repository.MaxUnpacked {
			return errors.New("payload size limit exceeded")
		}
		f, e := os.Open(file)
		if e != nil {
			return e
		}
		defer f.Close()
		if e = tw.WriteHeader(&tar.Header{Name: filepath.ToSlash(relative), Mode: 0644, Size: info.Size(), Typeflag: tar.TypeReg, Format: tar.FormatGNU}); e != nil {
			return e
		}
		_, e = io.CopyN(tw, f, info.Size())
		return e
	}
	if *binaryFile != "" {
		err = add(*binaryFile, *entry)
	} else {
		rootInfo, e := os.Lstat(*payload)
		if e != nil || !rootInfo.IsDir() {
			return errors.New("payload must be a real directory")
		}
		err = filepath.WalkDir(*payload, func(file string, d fs.DirEntry, e error) error {
			if e != nil {
				return e
			}
			if d.IsDir() {
				return nil
			}
			relative, e := filepath.Rel(*payload, file)
			if e != nil {
				return e
			}
			return add(file, relative)
		})
	}
	if err != nil {
		return err
	}
	if err = tw.Close(); err != nil {
		return err
	}
	if err = gz.Close(); err != nil {
		return err
	}
	info, err := archive.Stat()
	if err != nil {
		return err
	}
	if info.Size() > repository.MaxArchive {
		return errors.New("compressed payload exceeds 32 MiB")
	}
	if _, err = archive.Seek(0, 0); err != nil {
		return err
	}
	// Validate before transmitting. The server repeats validation independently.
	if _, err = repository.BuildPackage(m, archive); err != nil {
		return err
	}
	if _, err = archive.Seek(0, 0); err != nil {
		return err
	}
	// A bundled public key verifies repository metadata before any upload.
	// This does not authenticate a publisher or make plaintext HTTP uploads safe.
	if len(trustKey) != 0 {
		if _, err = verifiedCatalog(client, origin, trustKey); err != nil {
			return err
		}
	}
	metadata, _ := json.Marshal(m)
	request, err := http.NewRequest(http.MethodPost, origin+"/api/v1/publish", archive)
	if err != nil {
		return err
	}
	request.ContentLength = info.Size()
	request.GetBody = func() (io.ReadCloser, error) { return os.Open(archive.Name()) }
	if *open {
		request.Header.Set("X-C1-Publish-Mode", "open")
	} else {
		request.Header.Set("Authorization", "Bearer "+secret)
	}
	request.Header.Set("X-C1-Metadata", base64.RawURLEncoding.EncodeToString(metadata))
	request.Header.Set("Content-Type", "application/gzip")
	response, err := client.Do(request)
	if err != nil {
		return errors.New("upload failed; safely retry the same version and payload")
	}
	defer response.Body.Close()
	data, err := io.ReadAll(io.LimitReader(response.Body, 8193))
	if err != nil || len(data) > 8192 {
		return errors.New("upload confirmation unavailable or too large; retry the same version and payload with the same token")
	}
	if response.StatusCode != http.StatusCreated {
		return publisherHTTPError("upload", response.StatusCode)
	}
	// Never echo a remote response: it can reflect credentials or contain
	// terminal control sequences, even on an otherwise successful request.
	if *open {
		fmt.Fprintf(output, "Published %s version %s anonymously; other anonymous publishers may update it.\n", m.ID, m.Version)
	} else {
		fmt.Fprintf(output, "Published %s version %s. Keep the same token file for updates.\n", m.ID, m.Version)
	}
	return nil
}
