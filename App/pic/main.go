package main

import (
	"context"
	"crypto/sha256"
	"encoding/json"
	"fmt"
	"os"
	"os/signal"
	"path/filepath"
	"runtime"
	"runtime/debug"
	"strconv"
	"strings"
	"syscall"
	"time"

	"c1device"
)

var version = "dev"

const defaultPicturesDir = "/storage/mtp/Pic"

func main() {
	if len(os.Args) == 2 && os.Args[1] == "--version" {
		fmt.Printf("c1pic %s\n", version)
		return
	}
	debug.SetMemoryLimit(16 << 20)
	debug.SetGCPercent(50)
	if len(os.Args) == 2 && os.Args[1] == "--check-library" {
		pictures, err := scanPictureDirectories(envOr("C1_PICTURES_DIR", defaultPicturesDir))
		counts := map[string]int{}
		for _, picture := range pictures {
			counts[strings.ToLower(filepath.Ext(picture.Path))]++
		}
		if err != nil {
			fmt.Fprintln(os.Stderr, err)
		}
		if encodeErr := json.NewEncoder(os.Stdout).Encode(struct {
			Count   int            `json:"count"`
			Formats map[string]int `json:"formats"`
			Partial bool           `json:"partial"`
		}{len(pictures), counts, err != nil}); encodeErr != nil {
			os.Exit(1)
		}
		return
	}
	if len(os.Args) == 3 && os.Args[1] == "--check-image" {
		if err := checkImage(os.Args[2]); err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
		return
	}
	if err := run(); err != nil {
		fmt.Fprintf(os.Stderr, "pic: %v\n", err)
		os.Exit(1)
	}
}

// Read-only diagnostics: never opens the panel or modifies the wallpaper.
func checkImage(path string) error {
	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()
	start := time.Now()
	picture, err := loadPictureContext(ctx, path)
	if err != nil {
		return err
	}
	frame := renderImmersive(picture)
	var stats runtime.MemStats
	runtime.ReadMemStats(&stats)
	return json.NewEncoder(os.Stdout).Encode(struct {
		Version      string `json:"version"`
		Width        int    `json:"width"`
		Height       int    `json:"height"`
		FrameSHA256  string `json:"frameSHA256"`
		Milliseconds int64  `json:"milliseconds"`
		HeapAlloc    uint64 `json:"heapAlloc"`
		TotalAlloc   uint64 `json:"totalAlloc"`
		PeakRSSKB    int64  `json:"peakRSSKB"`
	}{version, picture.Bounds().Dx(), picture.Bounds().Dy(), fmt.Sprintf("%x", sha256.Sum256(frame[:])), time.Since(start).Milliseconds(), stats.HeapAlloc, stats.TotalAlloc, peakRSSKB()})
}

func peakRSSKB() int64 {
	data, err := os.ReadFile("/proc/self/status")
	if err != nil {
		return 0
	}
	for _, line := range strings.Split(string(data), "\n") {
		fields := strings.Fields(line)
		if len(fields) >= 2 && fields[0] == "VmHWM:" {
			value, _ := strconv.ParseInt(fields[1], 10, 64)
			return value
		}
	}
	return 0
}

func run() error {
	picturesDir := envOr("C1_PICTURES_DIR", defaultPicturesDir)
	if err := os.MkdirAll(picturesDir, 0755); err != nil {
		return fmt.Errorf("prepare Pic directory: %w", err)
	}
	fontData, err := os.ReadFile(envOr("C1_FONT_PATH", filepath.Join("assets", "MiSans-Normal.ttf")))
	if err != nil {
		return fmt.Errorf("read font: %w", err)
	}
	typeface, err := c1device.ParseTypeface(fontData)
	if err != nil {
		return err
	}
	face, err := typeface.NewFace(16)
	if err != nil {
		return err
	}
	defer face.Close()
	platform, err := c1device.OpenPlatform()
	if err != nil {
		return err
	}
	defer platform.Close()
	defer c1device.ReturnToDesktop()
	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM, syscall.SIGHUP)
	defer stop()
	loader := newPictureLoader(ctx, loadPictureContext)
	defer loader.close()
	state := pictureState{selected: -1, picturesDir: picturesDir, notice: "正在扫描 Pic / Pictures…"}
	draw := func() error { return platform.Draw(renderPicture(state, face, face), true) }
	if err := draw(); err != nil {
		return err
	}
	pictures, scanErr := scanPictureDirectories(picturesDir)
	state.pictures = pictures
	if len(pictures) > 0 {
		state.selected = 0
	}
	state.requestSelected()
	if scanErr != nil {
		state.notice = "部分目录未读取，R可重试"
		fmt.Fprintf(os.Stderr, "scan: %v\n", scanErr)
	}
	var generation uint64
	submit := func() {
		if !state.loadRequested {
			return
		}
		state.loadRequested = false
		path := ""
		if state.selected >= 0 && state.selected < len(state.pictures) {
			path = state.pictures[state.selected].Path
		}
		generation = loader.selectPath(path)
	}
	submit()
	if err := draw(); err != nil {
		return err
	}
	var noticeTimer *time.Timer
	var noticeDeadline <-chan time.Time
	defer func() {
		if noticeTimer != nil {
			noticeTimer.Stop()
		}
	}()
	resetNotice := func() {
		if noticeTimer != nil {
			noticeTimer.Stop()
		}
		noticeDeadline = nil
		// Success overlays disappear so immersive view becomes image-only again.
		if state.immersive && state.current != nil && state.notice != "" {
			noticeTimer = time.NewTimer(2500 * time.Millisecond)
			noticeDeadline = noticeTimer.C
		}
	}
	for {
		select {
		case <-ctx.Done():
			return nil
		case <-noticeDeadline:
			noticeDeadline = nil
			state.notice = ""
			if err := draw(); err != nil {
				return err
			}
		case result := <-loader.results:
			if result.generation != generation {
				continue
			}
			state.loading = false
			state.current = result.picture
			state.notice = ""
			if result.err != nil {
				state.notice = pictureErrorNotice(result.err)
				fmt.Fprintf(os.Stderr, "decode: %v\n", result.err)
			}
			if err := draw(); err != nil {
				return err
			}
		case event, ok := <-platform.Events():
			if !ok {
				return nil
			}
			changed, quit := state.handle(event)
			if quit {
				return nil
			}
			if state.refreshRequested {
				state.refreshRequested = false
				previous := ""
				if state.selected >= 0 && state.selected < len(state.pictures) {
					previous = state.pictures[state.selected].Path
				}
				state.pictures, err = scanPictureDirectories(picturesDir)
				state.selected = -1
				if len(state.pictures) > 0 {
					state.selected = 0
				}
				for i, picture := range state.pictures {
					if picture.Path == previous {
						state.selected = i
						break
					}
				}
				state.requestSelected()
				if err != nil {
					state.notice = "部分目录未读取，R可重试"
					fmt.Fprintf(os.Stderr, "scan: %v\n", err)
				}
			}
			submit()
			if changed {
				resetNotice()
				if err := draw(); err != nil {
					return err
				}
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
