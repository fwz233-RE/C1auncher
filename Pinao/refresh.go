package main

import "time"

const backgroundRefreshInterval = 250 * time.Millisecond

// Input bypasses the background deadline. Identical frames are suppressed here
// as well as at the device boundary, so releases of short taps don't enqueue an
// immediate erase. The screen writer remains asynchronous and latest-only.
type refreshScheduler struct {
	last      frame
	next      time.Time
	view      viewState
	viewReady bool
}

func newRefreshScheduler(initial frame, now time.Time) refreshScheduler {
	return refreshScheduler{last: initial, next: now.Add(backgroundRefreshInterval)}
}

func (r *refreshScheduler) update(m *model, now time.Time, input bool, submit func(frame)) {
	if !input && now.Before(r.next) {
		return
	}
	// Input must not postpone background work indefinitely (notice expiry,
	// feedback expiry, and the loop marker still need to be checked).
	if !now.Before(r.next) {
		r.next = now.Add(backgroundRefreshInterval)
	}
	v := m.view(now)
	if r.viewReady && v == r.view {
		return
	}
	r.view, r.viewReady = v, true
	f := renderView(v)
	if f != r.last {
		submit(f)
		r.last = f
	}
}
