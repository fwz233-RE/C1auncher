package main

import "sync"

// Keep at most the latest frame pending. Slow e-paper IO must not block musical
// input handling or replay a queue of old animation frames.
type screenWriter struct {
	frames     chan frame
	errors     chan error
	stop, done chan struct{}
	once       sync.Once
}

func newScreenWriter(draw func(frame, bool) error) *screenWriter {
	w := &screenWriter{frames: make(chan frame, 1), errors: make(chan error, 1), stop: make(chan struct{}), done: make(chan struct{})}
	go func() {
		defer close(w.done)
		for {
			select {
			case <-w.stop:
				return
			case f := <-w.frames:
				if err := draw(f, false); err != nil {
					w.errors <- err
					return
				}
			}
		}
	}()
	return w
}

// Only the model goroutine submits frames.
func (w *screenWriter) submit(f frame) {
	select {
	case <-w.done:
		return
	default:
	}
	select {
	case w.frames <- f:
		return
	default:
	}
	select {
	case <-w.frames:
	default:
	}
	select {
	case w.frames <- f:
	default:
	}
}
func (w *screenWriter) close() { w.once.Do(func() { close(w.stop); <-w.done }) }
