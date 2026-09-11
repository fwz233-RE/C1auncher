package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"syscall"
	"time"
)

var version = "dev"

func main() {
	configureMemory()
	if err := run(os.Args[1:]); err != nil {
		fmt.Fprintln(os.Stderr, "pinao:", err)
		os.Exit(1)
	}
}
func run(args []string) error {
	flags := flag.NewFlagSet("pinao", flag.ContinueOnError)
	ver := flags.Bool("version", false, "print version without opening hardware")
	preview := flags.String("preview", "", "write a 3x PNG UI preview (new file)")
	wav := flags.String("demo-wav", "", "render a demo loop to a new WAV file, no hardware")
	export := flags.String("export-wav", "", "render your saved loop to a new WAV file")
	songPath := flags.String("song", defaultSongPath(), "local song file")
	smoke := flags.Bool("smoke-test", false, "play a quiet 6-second device demo, without touching your saved song")
	if err := flags.Parse(args); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			return nil
		}
		return err
	}
	if flags.NArg() != 0 {
		return errors.New("unexpected positional argument; use --help")
	}
	if *ver {
		fmt.Println("pinao", version)
		return nil
	}
	modes := 0
	for _, on := range []bool{*preview != "", *wav != "", *export != "", *smoke} {
		if on {
			modes++
		}
	}
	if modes > 1 {
		return errors.New("choose one output/test mode")
	}
	if *preview != "" {
		f, e := os.OpenFile(*preview, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0600)
		if e != nil {
			return e
		}
		m := newModel(demoSong())
		now := time.Unix(100, 0)
		m.start(now)
		m.Step = 4
		m.Held[30] = 60
		m.Held[34] = 67
		m.addSpark(0, false, now.Add(-200*time.Millisecond))
		m.addSpark(7, false, now.Add(-500*time.Millisecond))
		m.addSpark(4, false, now.Add(-900*time.Millisecond))
		e = writePNG(f, render(m, now), 3)
		return errors.Join(e, f.Close())
	}
	if *wav != "" {
		return exportWAV(*wav, demoSong())
	}
	song, e := loadSong(*songPath)
	if *smoke {
		song = demoSong()
		e = nil
	}
	if e != nil {
		return e
	}
	if *export != "" {
		return exportWAV(*export, song)
	}
	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM, syscall.SIGHUP)
	defer stop()
	if *smoke {
		var cancel context.CancelFunc
		ctx, cancel = context.WithTimeout(ctx, 6*time.Second)
		defer cancel()
	}
	return runDevice(ctx, song, *songPath, *smoke)
}
func runDevice(ctx context.Context, song Song, path string, smoke bool) (result error) {
	ctx, cancel := context.WithCancel(ctx)
	defer cancel()
	defer func() {
		if cause := recover(); cause != nil {
			writeSessionRecord(path, "panic", fmt.Errorf("%v", cause))
			panic(cause)
		}
		recordSessionResult(path, result)
	}()
	p, err := openPlatform()
	if err != nil {
		return err
	}
	defer func() {
		p.close()
		if !smoke {
			requestDesktop()
		}
	}()
	a, err := openAudio(song.Volume)
	if err != nil {
		return err
	}
	defer a.close()
	m := newModel(song)
	m.StoragePath = path
	if smoke {
		m.start(time.Now())
	}
	m.message("A-K: PLAY   L: HELP", time.Now())
	initialFrame := render(m, time.Now())
	if err = p.draw(initialFrame, true); err != nil {
		return err
	}
	screen := newScreenWriter(p.draw)
	defer func() { a.close(); screen.close() }()
	timer := time.NewTicker(10 * time.Millisecond)
	defer timer.Stop()
	refresh := newRefreshScheduler(initialFrame, time.Now())
	lastSaveAttempt := time.Time{}
	nextDiagnostic := time.Now()
	lastLoopTime := time.Now()
	exportDone := make(chan error, 1)
	exporting := false
	defer func() {
		if exporting {
			cancel()
			result = errors.Join(result, exportResult(<-exportDone))
		}
	}()
	defer func() {
		if !smoke && m.Dirty {
			result = errors.Join(result, saveSong(path, m.Song))
		}
	}()
	var commandStorage [10]soundCommand
	for {
		cmds := commandStorage[:0]
		action := ""
		inputRefresh := false
		now := time.Now()
		select {
		case <-ctx.Done():
			return nil
		case err := <-a.Errors:
			return err
		case err := <-screen.errors:
			return err
		case exportErr := <-exportDone:
			now = time.Now()
			inputRefresh = true
			exporting = false
			if exportErr != nil {
				m.message("WAV EXPORT FAILED", now)
			} else {
				m.message("WAV SAVED TO EXPORTS", now)
			}
		case e, ok := <-p.output:
			if !ok {
				return errors.New("keyboard disconnected")
			}
			now = time.Now()
			inputRefresh = true
			inputCommands, inputAction := handleInput(m, e, now, exporting)
			cmds = append(cmds, inputCommands...)
			action = inputAction
		case now = <-timer.C:
		}
		if now.Sub(lastLoopTime) > 2*time.Second {
			m.cancelPageGesture()
			clear(m.Held)
			m.Feedback = [13]time.Time{}
			m.Sparks = m.Sparks[:0]
			inputRefresh = true
			if err = a.send(soundCommand{Kind: "off-all"}); err != nil {
				return err
			}
		}
		lastLoopTime = now
		cmds = append(cmds, m.tick(now)...)
		for _, c := range cmds {
			if err = a.send(c); err != nil {
				return err
			}
		}
		if action == "exit" {
			return nil
		}
		if len(action) > 15 && action[:15] == "manager-delete:" {
			if err := deleteManagedFile(action[15:]); err != nil {
				m.message("DELETE FAILED", now)
			} else {
				m.message("WAV DELETED", now)
				m.ManagerItems, _ = listManagedFiles(path)
				if m.ManagerIndex >= len(m.ManagerItems) {
					m.ManagerIndex = max(0, len(m.ManagerItems)-1)
				}
			}
		}
		if len(action) > 13 && action[:13] == "manager-play:" {
			if err := exec.Command("aplay", action[13:]).Start(); err != nil {
				m.message("WAV PLAY FAILED", now)
			} else {
				m.message("PLAYING WAV", now)
			}
		}
		if len(action) > 13 && action[:13] == "manager-open:" {
			if next, e := loadSong(action[13:]); e != nil {
				m.message("OPEN FAILED", now)
			} else {
				m.Song = next
				m.StoragePath = action[13:]
				m.Dirty = false
				m.Page = 0
				m.message("SONG OPENED", now)
				path = m.StoragePath
				_ = a.send(soundCommand{Kind: "volume", Volume: m.Song.Volume})
			}
		}
		if action == "manager-create" || action == "manager-saveas" {
			name := archiveStamp()
			if action == "manager-saveas" {
				name += "-copy"
			}
			if p, e := createArchive(path, name); e != nil {
				m.message("CREATE FAILED", now)
			} else {
				if action == "manager-saveas" {
					_ = saveSong(p, m.Song)
					m.message("SAVED AS NEW SONG", now)
				} else {
					m.message("NEW SONG CREATED", now)
				}
				m.ManagerItems, _ = listManagedFiles(path)
			}
		}
		if !smoke && action == "export" && !exporting {
			if m.noteCount() == 0 {
				m.message("RECORD A LOOP FIRST", now)
			} else {
				dir, exportErr := ensureExportDirectory(path)
				if exportErr != nil {
					m.message("CANNOT CREATE EXPORT FOLDER", now)
				} else {
					m.cancelPageGesture()
					m.stop()
					clear(m.Held)
					if err = a.send(soundCommand{Kind: "off-all"}); err != nil {
						return err
					}
					copy := m.Song.clone()
					exporting = true
					m.message("EXPORTING WAV - PLEASE WAIT", now)
					go func() { exportDone <- exportWAVContext(ctx, exportName(dir, now), copy) }()
				}
			}
		}
		if !smoke && (action == "save" || m.pageSavePending || (m.Dirty && now.Sub(m.Changed) > 2*time.Second && now.Sub(lastSaveAttempt) > 5*time.Second)) {
			pageSave := m.pageSavePending
			lastSaveAttempt = now
			if err = saveSong(path, m.Song); err != nil {
				// Keep the pending flag set so a transient storage/backup error
				// can be retried on the next loop or explicit save.
				m.pageSavePending = pageSave
				m.message("SAVE FAILED - CHECK STORAGE", now)
			} else {
				m.pageSavePending = false
				m.Dirty = false
				if !pageSave {
					m.message("SAVED LOCALLY", now)
				}
			}
		}
		refresh.update(m, now, inputRefresh, screen.submit)
		if !now.Before(nextDiagnostic) {
			writeSessionRecord(path, "running", nil)
			nextDiagnostic = now.Add(30 * time.Second)
		}
	}
}

// Keep exports next to user data, never in the immutable installed package.
func ensureExportDirectory(songPath string) (string, error) {
	dir := filepath.Join(filepath.Dir(songPath), "exports")
	return dir, os.MkdirAll(dir, 0700)
}
