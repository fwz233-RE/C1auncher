package repository

import (
	"context"
	"errors"
	"sync"
	"time"
)

var ErrBusy = errors.New("download queue full")

type waiter struct {
	ready   chan struct{}
	granted bool
}
type Gate struct {
	mu                      sync.Mutex
	active, limit, capacity int
	waiting                 []*waiter
}

func NewGate(active, queue int) *Gate { return &Gate{limit: active, capacity: queue} }
func (g *Gate) Acquire(ctx context.Context) (func(), error) {
	g.mu.Lock()
	if ctx.Err() != nil {
		g.mu.Unlock()
		return nil, ctx.Err()
	}
	if g.active < g.limit && len(g.waiting) == 0 {
		g.active++
		g.mu.Unlock()
		return g.release, nil
	}
	if len(g.waiting) >= g.capacity {
		g.mu.Unlock()
		return nil, ErrBusy
	}
	w := &waiter{ready: make(chan struct{})}
	g.waiting = append(g.waiting, w)
	g.mu.Unlock()
	select {
	case <-w.ready:
		return g.release, nil
	case <-ctx.Done():
		g.mu.Lock()
		granted := w.granted
		if !granted {
			for i, v := range g.waiting {
				if v == w {
					g.waiting = append(g.waiting[:i], g.waiting[i+1:]...)
					break
				}
			}
		}
		g.mu.Unlock()
		if granted {
			g.release()
		}
		return nil, ctx.Err()
	}
}
func (g *Gate) release() {
	g.mu.Lock()
	defer g.mu.Unlock()
	if len(g.waiting) > 0 {
		w := g.waiting[0]
		g.waiting = g.waiting[1:]
		w.granted = true
		close(w.ready)
	} else {
		g.active--
	}
}
func (g *Gate) Stats() (int, int) { g.mu.Lock(); defer g.mu.Unlock(); return g.active, len(g.waiting) }

type bandwidth struct {
	mu   sync.Mutex
	next time.Time
	rate int64
}

func (b *bandwidth) wait(ctx context.Context, n int) error {
	b.mu.Lock()
	now := time.Now()
	if b.next.Before(now) {
		b.next = now
	}
	b.next = b.next.Add(time.Duration(int64(n) * int64(time.Second) / b.rate))
	wait := time.Until(b.next)
	b.mu.Unlock()
	timer := time.NewTimer(wait)
	defer timer.Stop()
	select {
	case <-ctx.Done():
		return ctx.Err()
	case <-timer.C:
		return nil
	}
}
