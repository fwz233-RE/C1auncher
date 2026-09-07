package main

import (
	"context"
	"fmt"
	"os"
	"os/signal"
	"path/filepath"
	"syscall"
	"time"

	"c1device"
)

var version = "dev"

const defaultBooksDir = "/storage/mtp/Book"

func main() {
	if len(os.Args) == 2 && os.Args[1] == "--version" {
		fmt.Printf("c1book-reader %s\n", version)
		return
	}
	if err := runWithDiagnostics(); err != nil {
		fmt.Fprintf(os.Stderr, "book-reader: %v\n", err)
		os.Exit(1)
	}
}

func run() error {
	fontPath := envOr("C1_FONT_PATH", filepath.Join("assets", "MiSans-Normal.ttf"))
	booksDir := envOr("C1_BOOKS_DIR", defaultBooksDir)
	if err := os.MkdirAll(booksDir, 0755); err != nil {
		return fmt.Errorf("prepare Book directory: %w", err)
	}
	home := envOr("C1_BOOK_READER_HOME", "/usr/data/c1/book-reader")
	fontData, err := os.ReadFile(fontPath)
	if err != nil {
		return fmt.Errorf("read font %s: %w", fontPath, err)
	}
	typeface, err := c1device.ParseTypeface(fontData)
	if err != nil {
		return err
	}
	uiFace, err := typeface.NewFace(readerUIFontSize)
	if err != nil {
		return err
	}
	defer uiFace.Close()
	bodyFace, err := typeface.NewFace(readerBodyFontSize)
	if err != nil {
		return err
	}
	defer bodyFace.Close()
	app, err := newReaderApp(
		booksDir,
		uiFace,
		bodyFace,
		ProgressStore{Dir: home},
		BookmarkStore{Dir: home},
	)
	if err != nil {
		return fmt.Errorf("scan library: %w", err)
	}
	platform, err := c1device.OpenPlatform()
	if err != nil {
		return err
	}
	defer c1device.ReturnToDesktop()
	defer platform.Close()
	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM, syscall.SIGHUP)
	defer stop()
	if err := platform.Draw(app.render(), true); err != nil {
		return err
	}
	var saveTimer *time.Timer
	var saveChannel <-chan time.Time
	refreshes := 0
	save := func() {
		if err := app.saveProgress(); err != nil {
			app.message = err.Error()
		}
	}
	defer func() {
		if saveTimer != nil {
			saveTimer.Stop()
		}
		save()
	}()
	for {
		select {
		case <-ctx.Done():
			return nil
		case <-saveChannel:
			save()
			saveChannel = nil
		case event, ok := <-platform.Events():
			if !ok {
				return fmt.Errorf("input event stream closed unexpectedly")
			}
			if app.handleEvent(event) {
				return nil
			}
			if app.dirty {
				if saveTimer == nil {
					saveTimer = time.NewTimer(750 * time.Millisecond)
				} else {
					if !saveTimer.Stop() {
						select {
						case <-saveTimer.C:
						default:
						}
					}
					saveTimer.Reset(750 * time.Millisecond)
				}
				saveChannel = saveTimer.C
			}
			refreshes++
			if err := platform.Draw(app.render(), refreshes%12 == 0); err != nil {
				return fmt.Errorf("draw view=%d chapter=%d page=%d key=%d rune=%q: %w", app.view, app.chapterIndex, app.pageIndex, event.Key, event.Rune, err)
			}
		}
	}
}

func envOr(name, fallback string) string {
	if value := os.Getenv(name); value != "" {
		return value
	}
	return fallback
}
