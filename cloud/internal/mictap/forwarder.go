// Package mictap forwards MicTap, the stream of what Vector hears and plays, from vic-anim to the cortex.
//
// vic-anim sends the MicTap v1 stream to the local unix socket mictap_sock (see
// animProcess/src/cozmoAnim/micData/micTap.h): each hello alone in one datagram, frames batched several per
// datagram. The Forwarder opens an upload to wire-pod when frames arrive, starts every upload with the latest
// hello, uploads the frames in one chunk per FlushInterval, and closes the upload after a quiet period. The robot is the client, so
// no port is open on the robot and the link works across NAT boundaries.
package mictap

import (
	"bytes"
	"context"
	"sync/atomic"
	"time"
)

const helloSize = 16

// healthyFrames sent in one upload (about 1 s of mic audio) prove the cortex accepts it; only then is the
// reconnect backoff reset, so a server that rejects every upload is retried less and less often.
const healthyFrames = 100

var helloMagic = []byte("VMT1")

func isHello(b []byte) bool {
	return len(b) == helloSize && bytes.HasPrefix(b, helloMagic)
}

// Stream is one upload to the cortex.
type Stream interface {
	Send(chunk []byte) error
	// CloseAndRecv ends the upload and returns the server's status.
	CloseAndRecv() error
}

type Dialer func(ctx context.Context) (Stream, error)

type Config struct {
	Dial Dialer
	// IdleTimeout closes the upload after this long without datagrams (MicTap off, or vic-anim gone).
	IdleTimeout time.Duration
	// MinBackoff and MaxBackoff bound the wait before reconnecting after a failed dial or send.
	MinBackoff, MaxBackoff time.Duration
	// FlushInterval batches frames: one upload chunk per interval instead of one per datagram keeps the TLS
	// and gRPC cost low on the robot (about 130 datagrams per second arrive).
	FlushInterval time.Duration
	Logf          func(format string, a ...any)
}

// maxChunkBytes flushes early so one chunk stays well under gRPC's default 4 MB message limit.
const maxChunkBytes = 64 * 1024

type Forwarder struct {
	cfg     Config
	hello   []byte
	stream  Stream
	backoff time.Duration
	sent    int // frames sent in the current upload
	pending []byte
	queued  int // frames in pending
	retryAt time.Time
	dropped uint64 // frames not forwarded: before a hello, while reconnecting, or after a failed send
}

func New(cfg Config) *Forwarder {
	if cfg.IdleTimeout <= 0 {
		cfg.IdleTimeout = 5 * time.Second
	}
	if cfg.MinBackoff <= 0 {
		cfg.MinBackoff = time.Second
	}
	if cfg.MaxBackoff < cfg.MinBackoff {
		cfg.MaxBackoff = 30 * time.Second
	}
	if cfg.FlushInterval <= 0 {
		cfg.FlushInterval = 100 * time.Millisecond
	}
	if cfg.Logf == nil {
		cfg.Logf = func(string, ...any) {}
	}
	return &Forwarder{cfg: cfg}
}

// Run forwards datagrams until ctx is done or datagrams is closed.
func (f *Forwarder) Run(ctx context.Context, datagrams <-chan []byte) {
	idle := time.NewTimer(f.cfg.IdleTimeout)
	stopTimer(idle)
	flush := time.NewTicker(f.cfg.FlushInterval)
	defer flush.Stop()
	defer f.closeStream("stopping")
	for {
		select {
		case <-ctx.Done():
			return
		case <-flush.C:
			f.flush()
		case <-idle.C:
			f.closeStream("idle")
		case b, ok := <-datagrams:
			if !ok {
				return
			}
			f.handle(ctx, b)
			if f.stream != nil {
				stopTimer(idle)
				idle.Reset(f.cfg.IdleTimeout)
			}
		}
	}
}

func (f *Forwarder) handle(ctx context.Context, b []byte) {
	if isHello(b) {
		if bytes.Equal(b, f.hello) {
			return // vic-anim repeats its hello every second; it is sent once per upload
		}
		changed := f.hello != nil
		f.hello = append([]byte(nil), b...)
		if changed {
			f.closeStream("hello changed")
		}
		return
	}
	if f.hello == nil || (f.stream == nil && !f.open(ctx)) {
		f.dropped++
		return
	}
	f.pending = append(f.pending, b...)
	f.queued++
	if len(f.pending) >= maxChunkBytes {
		f.flush()
	}
}

// flush uploads the batched frames as one chunk.
func (f *Forwarder) flush() {
	if f.stream == nil || len(f.pending) == 0 {
		return
	}
	err := f.stream.Send(f.pending)
	frames := f.queued
	f.pending, f.queued = f.pending[:0], 0
	if err != nil {
		f.dropped += uint64(frames)
		f.abort("send", err)
		return
	}
	before := f.sent
	f.sent += frames
	if before < healthyFrames && f.sent >= healthyFrames {
		f.backoff = 0
	}
}

func (f *Forwarder) open(ctx context.Context) bool {
	if time.Now().Before(f.retryAt) {
		return false
	}
	s, err := f.cfg.Dial(ctx)
	if err != nil {
		f.backOff("dial", err)
		return false
	}
	if err := s.Send(f.hello); err != nil {
		s.CloseAndRecv()
		f.backOff("send hello", err)
		return false
	}
	f.stream, f.sent = s, 0
	f.cfg.Logf("mictap: upload started")
	return true
}

func (f *Forwarder) backOff(op string, err error) {
	if f.backoff == 0 {
		f.backoff = f.cfg.MinBackoff
	} else {
		f.backoff = min(2*f.backoff, f.cfg.MaxBackoff)
	}
	f.retryAt = time.Now().Add(f.backoff)
	f.cfg.Logf("mictap: %s failed, retrying in %v: %v", op, f.backoff, err)
}

// abort ends a failed upload without trying to send what is pending, and backs off.
func (f *Forwarder) abort(op string, err error) {
	f.pending, f.queued = f.pending[:0], 0
	if f.stream != nil {
		f.stream.CloseAndRecv()
		f.stream = nil
		f.cfg.Logf("mictap: upload ended (%s failed), %d frames not forwarded so far", op, f.dropped)
	}
	f.backOff(op, err)
}

// closeStream uploads what is pending and ends the upload.
func (f *Forwarder) closeStream(reason string) {
	if f.stream == nil {
		return
	}
	f.flush()
	if f.stream == nil {
		return // the final flush failed and already ended the upload
	}
	err := f.stream.CloseAndRecv()
	f.stream = nil
	f.cfg.Logf("mictap: upload ended (%s), %d frames not forwarded so far, server: %v", reason, f.dropped, err)
}

func stopTimer(t *time.Timer) {
	if !t.Stop() {
		select {
		case <-t.C:
		default:
		}
	}
}

// Pump moves datagrams from read into queue until read returns nil, dropping (and counting in dropped) any
// that do not fit, so a slow network never blocks the socket reader.
func Pump(read func() []byte, queue chan<- []byte, dropped *uint64) {
	for {
		b := read()
		if b == nil {
			return
		}
		select {
		case queue <- b:
		default:
			atomic.AddUint64(dropped, 1)
		}
	}
}
