// c1token creates a revocable publisher token, not a repository signing key.
package main

import (
	"c1repo/internal/repository"
	"crypto/rand"
	"encoding/base64"
	"flag"
	"fmt"
	"os"
)

func main() {
	output := flag.String("out", "", "new secret token file; will not overwrite")
	flag.Parse()
	if *output == "" {
		fmt.Fprintln(os.Stderr, "Use -out /secure/developer.token")
		os.Exit(2)
	}
	b := make([]byte, 32)
	if _, err := rand.Read(b); err != nil {
		panic(err)
	}
	token := base64.RawURLEncoding.EncodeToString(b)
	f, err := os.OpenFile(*output, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0600)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	if _, err = f.WriteString(token + "\n"); err != nil {
		f.Close()
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	if err = f.Close(); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	fmt.Println("Token written to secret file. Configure this SHA-256 hash only:")
	fmt.Println(repository.TokenHash(token))
}
