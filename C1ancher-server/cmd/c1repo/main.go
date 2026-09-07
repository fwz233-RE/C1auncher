package main

import (
	"c1repo/internal/repository"
	"context"
	"flag"
	"fmt"
	"log"
	"net"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"
)

func main() {
	config := flag.String("config", "/etc/c1repo/config.json", "configuration file")
	check := flag.Bool("check", false, "validate configuration and signed storage without serving")
	importDir := flag.String("import-legacy", "", "import a locally verified old release (service must be stopped)")
	flag.Parse()
	c, err := repository.LoadConfig(*config)
	if err != nil {
		log.Fatal(err)
	}
	host, _, err := net.SplitHostPort(c.Listen)
	if err != nil || net.ParseIP(host) == nil || !net.ParseIP(host).IsLoopback() {
		log.Fatal("listen must be a loopback IP:port; use HTTPS reverse proxy or SSH tunnel")
	}
	s, err := repository.Open(c)
	if err != nil {
		log.Fatal(err)
	}
	defer s.Close()
	if *importDir != "" {
		if err = repository.ImportLegacy(s, c, *importDir); err != nil {
			log.Fatal(err)
		}
		fmt.Println("Legacy catalog imported and verified")
		return
	}
	if *check {
		fmt.Printf("Repository verified: sequence %d, %d applications\n", s.Catalog().Sequence, len(s.Catalog().Packages))
		return
	}
	server := &http.Server{Addr: c.Listen, Handler: repository.NewServer(c, s), ReadHeaderTimeout: 10 * time.Second, ReadTimeout: 10 * time.Minute, WriteTimeout: 15 * time.Minute, IdleTimeout: 30 * time.Second, MaxHeaderBytes: 8192}
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	go func() {
		<-ctx.Done()
		deadline, cancel := context.WithTimeout(context.Background(), 30*time.Second)
		defer cancel()
		_ = server.Shutdown(deadline)
	}()
	log.Printf("Repository listening on %s; %d downloads, %d waiting, %d bytes/s", c.Listen, c.Downloads, c.Queue, c.BytesPerSecond)
	if err = server.ListenAndServe(); err != nil && err != http.ErrServerClosed {
		log.Fatal(err)
	}
}
