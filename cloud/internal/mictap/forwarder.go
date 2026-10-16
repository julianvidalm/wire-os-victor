// Package mictap forwards MicTap, the stream of what Vector hears and plays, from vic-anim to the cortex.
//
// vic-anim sends one MicTap v1 datagram per hello or frame to the local unix socket mictap_sock (see
// animProcess/src/cozmoAnim/micData/micTap.h). The Forwarder opens an upload to wire-pod when frames arrive,
// starts every upload with the latest hello, and closes it after a quiet period. The robot is the client, so
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
	Logf                   func(format string, a ...any)
}

type Forwarder struct {
	cfg     Config
	hello   []byte
	stream  Stream
	backoff time.Duration
	sent    int // frames sent in the current upload
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
	if cfg.Logf == nil {
		cfg.Logf = func(string, ...any) {}
	}
	return &Forwarder{cfg: cfg}
}

// Run forwards datagrams until ctx is done or datagrams is closed.
func (f *Forwarder) Run(ctx context.Context, datagrams <-chan []byte) {
	idle := time.NewTimer(f.cfg.IdleTimeout)
	stopTimer(idle)
	defer f.closeStream("stopping")
	for {
		select {
		case <-ctx.Done():
			return
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
	if err := f.stream.Send(b); err != nil {
		f.dropped++
		f.closeStream("send failed")
		f.backOff("send", err)
		return
	}
	if f.sent++; f.sent == healthyFrames {
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

func (f *Forwarder) closeStream(reason string) {
	if f.stream == nil {
		return
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
