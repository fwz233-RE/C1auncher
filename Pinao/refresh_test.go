package main

import (
	"testing"
	"time"
)

func TestRefreshInputBypassesBackgroundDeadline(t *testing.T) {
	m := newModel(defaultSong())
	now := time.Unix(100, 0)
	r := newRefreshScheduler(render(m, now), now)
	calls := 0
	submit := func(frame) { calls++ }
	press := now.Add(time.Millisecond)
	m.handle(keyEvent{Code: 30, Down: true}, press)
	r.update(m, press, true, submit)
	if calls != 1 {
		t.Fatal("input waited for background deadline")
	}
	m.handle(keyEvent{Code: 17, Down: true}, press.Add(time.Millisecond))
	r.update(m, press.Add(time.Millisecond), true, submit)
	if calls != 2 {
		t.Fatal("second chord key waited for background deadline")
	}
}

func TestRefreshShortTapStaysVisibleWithoutDelayingNoteOff(t *testing.T) {
	for _, code := range []uint16{30, 17, 31, 18, 32, 33, 20, 34, 21, 35, 22, 36, 37} {
		m := newModel(defaultSong())
		now := time.Unix(100, 0)
		idle := render(m, now)
		m.handle(keyEvent{Code: code, Down: true}, now)
		pressed := render(m, now)
		releasedAt := now.Add(20 * time.Millisecond)
		commands, _ := m.handle(keyEvent{Code: code}, releasedAt)
		if len(commands) != 1 || commands[0].Kind != "off" || commands[0].ID != int(code) || len(m.Held) != 0 {
			t.Fatalf("key %d release changed audio semantics: %+v", code, commands)
		}
		if pressed == idle || render(m, releasedAt) != pressed {
			t.Fatalf("key %d short tap erased before display could catch up", code)
		}
		if !m.keyLit(code, releasedAt.Add(keyFeedbackDuration-time.Millisecond)) {
			t.Fatalf("key %d feedback expired too soon", code)
		}
		end := releasedAt.Add(keyFeedbackDuration)
		m.tick(end)
		if m.keyLit(code, end) || render(m, end) != idle {
			t.Fatalf("key %d feedback did not expire", code)
		}
	}
}

func TestRefreshHeldKeyAndRetrigger(t *testing.T) {
	m := newModel(defaultSong())
	now := time.Unix(100, 0)
	m.handle(keyEvent{Code: 30, Down: true}, now)
	later := now.Add(3 * time.Second)
	if !m.keyLit(30, later) {
		t.Fatal("held key lost highlight")
	}
	m.handle(keyEvent{Code: 30}, later)
	m.handle(keyEvent{Code: 30, Down: true}, later.Add(time.Second))
	m.handle(keyEvent{Code: 30}, later.Add(time.Second))
	if !m.keyLit(30, later.Add(2*time.Second)) {
		t.Fatal("retrigger did not extend feedback")
	}
	commands, _ := m.handle(keyEvent{Reset: true}, later.Add(2*time.Second))
	if m.keyLit(30, later.Add(2*time.Second)) || len(m.Sparks) != 0 || len(commands) != 1 || commands[0].Kind != "off-all" {
		t.Fatal("lost-input reset did not clear feedback and notes")
	}
}

func TestRefreshIdleAndTapReleaseDoNotSubmitDuplicates(t *testing.T) {
	m := newModel(defaultSong())
	now := time.Unix(100, 0)
	r := newRefreshScheduler(render(m, now), now)
	calls := 0
	submit := func(frame) { calls++ }
	for i := 0; i < 1000; i++ {
		r.update(m, now.Add(time.Duration(i)*10*time.Millisecond), false, submit)
	}
	if calls != 0 {
		t.Fatalf("idle submitted %d frames", calls)
	}
	now = now.Add(10 * time.Second)
	m.handle(keyEvent{Code: 30, Down: true}, now)
	r.update(m, now, true, submit)
	m.handle(keyEvent{Code: 30}, now.Add(10*time.Millisecond))
	r.update(m, now.Add(10*time.Millisecond), true, submit)
	if calls != 1 {
		t.Fatalf("short release submitted duplicate/erase: %d frames", calls)
	}
}

func TestRefreshIndicatorsStayStillAndDrumsExpire(t *testing.T) {
	m := newModel(defaultSong())
	now := time.Unix(100, 0)
	idle := render(m, now)
	for code := uint16(46); code <= 49; code++ {
		m.handle(keyEvent{Code: code, Down: true}, now)
	}
	active := render(m, now)
	if active == idle {
		t.Fatal("missing drum feedback")
	}
	for i := 1; i < 12; i++ {
		at := now.Add(time.Duration(i) * 100 * time.Millisecond)
		m.tick(at)
		if render(m, at) != active {
			t.Fatal("time alone animated recent activity")
		}
	}
	m.tick(now.Add(keyFeedbackDuration))
	if render(m, now.Add(keyFeedbackDuration)) != idle {
		t.Fatal("drum activity did not expire")
	}
}

func TestRefreshBackgroundNotStarvedByUnchangedInput(t *testing.T) {
	m := newModel(defaultSong())
	now := time.Unix(100, 0)
	m.message("TEST", now)
	r := newRefreshScheduler(render(m, now), now)
	calls := 0
	submit := func(frame) { calls++ }
	for i := 1; i <= 20; i++ {
		r.update(m, now.Add(time.Duration(i)*100*time.Millisecond), true, submit)
	}
	if calls != 1 {
		t.Fatalf("notice expiry not submitted exactly once: %d", calls)
	}
	if r.next.Sub(now.Add(2*time.Second)) > backgroundRefreshInterval {
		t.Fatal("input postponed background deadline")
	}
}

func TestRefreshBackgroundTracksLoopAndFeedbackExpiry(t *testing.T) {
	m := newModel(defaultSong())
	now := time.Unix(100, 0)
	m.handle(keyEvent{Code: 30, Down: true}, now)
	m.handle(keyEvent{Code: 30}, now)
	r := newRefreshScheduler(render(m, now), now)
	calls := 0
	submit := func(frame) { calls++ }
	for i := 1; i <= 200; i++ {
		at := now.Add(time.Duration(i) * 10 * time.Millisecond)
		m.tick(at)
		r.update(m, at, false, submit)
	}
	if calls != 1 {
		t.Fatalf("feedback expiry submissions = %d", calls)
	}
	m.start(now.Add(2 * time.Second))
	m.tick(now.Add(2 * time.Second))
	r.update(m, now.Add(2*time.Second), false, submit)
	m.tick(now.Add(2500 * time.Millisecond))
	r.update(m, now.Add(2500*time.Millisecond), false, submit)
	if calls != 3 {
		t.Fatal("loop progress stopped updating")
	}
}
