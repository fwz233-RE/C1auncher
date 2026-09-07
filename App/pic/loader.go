package main

import (
	"context"
	"image"
)

type pictureLoadRequest struct {
	ctx        context.Context
	generation uint64
	path       string
}
type pictureLoadResult struct {
	generation uint64
	picture    image.Image
	err        error
}
type pictureLoader struct {
	ctx        context.Context
	stop       context.CancelFunc
	cancel     context.CancelFunc
	requests   chan pictureLoadRequest
	results    chan pictureLoadResult
	done       chan struct{}
	generation uint64
}

func newPictureLoader(parent context.Context, decode func(context.Context, string) (image.Image, error)) *pictureLoader {
	ctx, stop := context.WithCancel(parent)
	loader := &pictureLoader{ctx: ctx, stop: stop, requests: make(chan pictureLoadRequest, 1), results: make(chan pictureLoadResult, 1), done: make(chan struct{})}
	go func() {
		defer close(loader.done)
		for {
			select {
			case <-ctx.Done():
				return
			case request := <-loader.requests:
				if request.ctx.Err() != nil {
					continue
				}
				picture, err := decode(request.ctx, request.path)
				if request.ctx.Err() != nil {
					continue
				}
				select {
				case loader.results <- pictureLoadResult{request.generation, picture, err}:
				case <-request.ctx.Done():
				case <-ctx.Done():
					return
				}
			}
		}
	}()
	return loader
}

// Called only by the event loop. At most one decode is active and one request
// awaits it; rapidly pressing arrows never accumulates original image buffers.
func (loader *pictureLoader) selectPath(path string) uint64 {
	if loader.cancel != nil {
		loader.cancel()
	}
	loader.generation++
	select {
	case <-loader.requests:
	default:
	}
	if path != "" {
		ctx, cancel := context.WithCancel(loader.ctx)
		loader.cancel = cancel
		loader.requests <- pictureLoadRequest{ctx, loader.generation, path}
	}
	return loader.generation
}
func (loader *pictureLoader) close() {
	if loader.cancel != nil {
		loader.cancel()
	}
	loader.stop()
	<-loader.done
}
