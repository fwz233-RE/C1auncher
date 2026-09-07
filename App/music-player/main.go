package main

import (
	"context"
	"fmt"
	"os"
	"os/signal"
	"runtime/debug"
	"strings"
	"syscall"
	"time"

	"c1device"
)

var version = "dev"

const (
	defaultMusicDir            = "/storage/mtp/Music"
	defaultFontPath            = "assets/MiSans-Normal.ttf"
	defaultDisplaySettingsPath = "/usr/data/c1/music-player/display.json"
	defaultVolumeSettingsPath  = "/usr/data/c1/music-player/volume.json"
)

func main() {
	if len(os.Args) > 1 && os.Args[1] == "--version" {
		fmt.Printf("c1music-player %s\n", version)
		return
	}
	if handled, err := handlePreview(os.Args[1:]); handled {
		if err != nil {
			fmt.Fprintf(os.Stderr, "preview: %v\n", err)
			os.Exit(1)
		}
		return
	}
	if err := runApp(); err != nil {
		fmt.Fprintf(os.Stderr, "music-player: %v\n", err)
		os.Exit(1)
	}
}

func runApp() error {
	debug.SetMemoryLimit(16 << 20)
	debug.SetGCPercent(50)
	musicDir := environmentOrDefault("C1_MUSIC_DIR", defaultMusicDir)
	if err := os.MkdirAll(musicDir, 0755); err != nil {
		return fmt.Errorf("prepare Music directory: %w", err)
	}
	fontPath := environmentOrDefault("C1_FONT_PATH", defaultFontPath)
	tracks, err := scanLibrary(musicDir)
	if err != nil {
		return fmt.Errorf("scan music library: %w", err)
	}
	fontData, err := os.ReadFile(fontPath)
	if err != nil {
		return fmt.Errorf("read font: %w", err)
	}
	typeface, err := c1device.ParseTypeface(fontData)
	if err != nil {
		return err
	}
	face, err := typeface.NewFace(musicTitleFontSize)
	if err != nil {
		return fmt.Errorf("create font face: %w", err)
	}
	defer face.Close()
	footerFace, err := typeface.NewFace(musicLabelFontSize)
	if err != nil {
		return fmt.Errorf("create footer face: %w", err)
	}
	defer footerFace.Close()

	platform, err := c1device.OpenPlatform()
	if err != nil {
		return fmt.Errorf("open platform: %w", err)
	}
	player := newPlayer(nil)
	defer func() {
		player.Stop()
		_ = platform.Close()
		c1device.ReturnToDesktop()
	}()

	ctx, cancel := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM, syscall.SIGHUP)
	defer cancel()
	settingsPath := environmentOrDefault("C1_MUSIC_DISPLAY_SETTINGS", defaultDisplaySettingsPath)
	volumePath := environmentOrDefault("C1_MUSIC_VOLUME_SETTINGS", defaultVolumeSettingsPath)
	volume, err := restoreVolume(volumePath, applyVolume)
	if err != nil {
		return err
	}
	state := appState{
		tracks:  tracks,
		current: -1,
		volume:  volume.Value(),
		order:   newPlaybackOrder(len(tracks), nil),
		visual:  loadVisualMode(settingsPath),
	}
	if len(tracks) > 0 {
		state.cover = loadTrackCover(tracks[0].Path)
	}
	lastProgressDraw := time.Time{}
	previousPlaying, previousPaused := false, false
	clock := &motionClock{}
	defer clock.stop()
	motionCache := &motionFrames{}
	var lastFrame c1device.Frame
	draw := func(initial bool) error {
		// Full clear on entry and when audio stops/pauses; other UI updates use
		// the fast path, including volume and visual-mode changes.
		full := shouldFullRefresh(initial, previousPlaying, previousPaused, state.playing, state.paused)
		lastFrame = renderMusic(state, face, footerFace)
		if err := platform.Draw(lastFrame, full); err != nil {
			return err
		}
		previousPlaying, previousPaused = state.playing, state.paused
		lastProgressDraw = time.Now()
		return nil
	}
	if err := draw(true); err != nil {
		return fmt.Errorf("draw initial screen: %w", err)
	}
	clock.sync(state)

	toggleShuffle := func() {
		state.shuffle = !state.shuffle
		current := state.current
		if current < 0 {
			current = state.selected
		}
		state.order.SetShuffle(state.shuffle, current)
		state.notice = ""
	}

	playIndex := func(index int) {
		if len(state.tracks) == 0 {
			return
		}
		if index < 0 {
			index = len(state.tracks) - 1
		}
		if index >= len(state.tracks) {
			index = 0
		}
		state.current = index
		state.selected = index
		state.order.Select(index)
		state.position = 0
		state.duration = 0
		state.paused = false
		state.notice = ""
		state.cover = loadTrackCover(state.tracks[index].Path)
		if err := player.Play(state.tracks[index].Path); err != nil {
			state.playing = false
			state.activeGen = 0
			state.notice = "播放失败: " + err.Error()
			return
		}
		state.playing = true
		state.activeGen = player.Generation()
	}

	for {
		redraw := false
		select {
		case <-ctx.Done():
			return nil
		case <-clock.ticks:
			clock.ticks = nil
			if motionActive(state) {
				state.visualTick = (state.visualTick + 1) % 16
				lastFrame = motionCache.apply(lastFrame, state)
				if err := platform.Draw(lastFrame, false); err != nil {
					return fmt.Errorf("refresh animation: %w", err)
				}
			}
			clock.sync(state)
		case event, ok := <-platform.Events():
			if !ok {
				return nil
			}
			switch event.Key {
			case c1device.KeyBack:
				return nil
			}
			action := musicActionForEvent(event)
			if action == actionNone {
				continue
			}
			state.notice = ""
			switch action {
			case actionSelectPrevious:
				state.selected = moveSelection(state.selected, len(state.tracks), -1)
				if state.selected >= 0 && !state.playing {
					state.cover = loadTrackCover(state.tracks[state.selected].Path)
				}
			case actionSelectNext:
				state.selected = moveSelection(state.selected, len(state.tracks), 1)
				if state.selected >= 0 && !state.playing {
					state.cover = loadTrackCover(state.tracks[state.selected].Path)
				}
			case actionPreviousTrack, actionNextTrack:
				index := state.current
				if index < 0 {
					index = state.selected
				}
				if action == actionPreviousTrack {
					playIndex(state.order.Previous(index))
				} else {
					playIndex(state.order.Next(index))
				}
			case actionPlayPause:
				if state.playing {
					if err := player.PauseToggle(); err != nil {
						state.notice = err.Error()
					} else {
						state.paused = !state.paused
					}
				} else {
					playIndex(state.selected)
				}
			case actionToggleShuffle:
				toggleShuffle()
			case actionCycleVisual:
				state.visual = state.visual.next()
				if err := saveVisualMode(settingsPath, state.visual); err != nil {
					state.notice = "视效已切换，本次未能保存设置"
				}
			case actionVolumeDown, actionVolumeUp:
				delta := -5
				if action == actionVolumeUp {
					delta = 5
				}
				if err := volume.Set(volume.Value() + delta); err != nil {
					state.notice = err.Error()
				}
				// A save error must not hide a successful mixer adjustment.
				state.volume = volume.Value()
			}
			redraw = true
		case event := <-player.Events():
			if !eventApplies(state.activeGen, event) {
				continue
			}
			switch event.Kind {
			case PlayerDuration:
				state.duration = event.Duration
				redraw = true
			case PlayerProgress:
				if state.playing && !state.paused {
					state.position = event.Position
					redraw = shouldDrawProgress(state, lastProgressDraw, time.Now())
				}
			case PlayerCompleted:
				state.position = state.duration
				playIndex(state.order.Next(state.current))
				redraw = true
			case PlayerFailed:
				state.playing = false
				state.paused = false
				state.activeGen = 0
				if event.Err != nil {
					state.notice = "播放错误: " + cleanProcessError(event.Err).Error()
				}
				redraw = true
			}
		}
		if redraw {
			if err := draw(false); err != nil {
				return fmt.Errorf("refresh screen: %w", err)
			}
			clock.sync(state)
		}
	}
}

func environmentOrDefault(name, fallback string) string {
	if value := strings.TrimSpace(os.Getenv(name)); value != "" {
		return value
	}
	return fallback
}
