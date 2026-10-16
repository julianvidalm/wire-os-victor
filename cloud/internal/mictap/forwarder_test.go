package mictap

import (
	"bytes"
	"context"
	"errors"
	"sync"
	"testing"
	"time"
)

var (
	hello1 = append([]byte("VMT1"), 1, 0, 0x80, 0x3e, 0, 0, 0, 0x7d, 0, 0, 0, 0) // v1, 16000, 32000
	hello2 = append([]byte("VMT1"), 1, 0, 0x80, 0x3e, 0, 0, 0, 0xfa, 0, 0, 0, 0) // v1, 16000, 64000
)

func frame(seq byte) []byte { return []byte{1, 0, 0, 0, seq, 0, 0, 0} }

// fakeStream records what one upload received.
type fakeStream struct {
	mu      sync.Mutex
	chunks  [][]byte
	closed  bool
	failAt  int // Send fails once this many chunks were sent (0: never)
	sendErr error
}

func (s *fakeStream) Send(b []byte) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.failAt > 0 && len(s.chunks) >= s.failAt {
		return errors.New("broken pipe")
	}
	s.chunks = append(s.chunks, append([]byte(nil), b...))
	return nil
}

func (s *fakeStream) CloseAndRecv() error {
	s.mu.Lock()
	s.closed = true
	s.mu.Unlock()
	return nil
}

func (s *fakeStream) snapshot() ([][]byte, bool) {
	s.mu.Lock()
	defer s.mu.Unlock()
	return append([][]byte(nil), s.chunks...), s.closed
}

// fakeDialer hands out streams in order and records dial times.
type fakeDialer struct {
	mu      sync.Mutex
	streams []*fakeStream
	failN   int // the first failN dials fail
	dials   []time.Time
	prepare func(n int, s *fakeStream)
}

func (d *fakeDialer) Dial(context.Context) (Stream, error) {
	d.mu.Lock()
	defer d.mu.Unlock()
	d.dials = append(d.dials, time.Now())
	if len(d.dials) <= d.failN {
		return nil, errors.New("connection refused")
	}
	s := &fakeStream{}
	if d.prepare != nil {
		d.prepare(len(d.streams), s)
	}
	d.streams = append(d.streams, s)
	return s, nil
}

func (d *fakeDialer) stream(i int) *fakeStream {
	d.mu.Lock()
	defer d.mu.Unlock()
	if i >= len(d.streams) {
		return nil
	}
	return d.streams[i]
}

func (d *fakeDialer) dialCount() int {
	d.mu.Lock()
	defer d.mu.Unlock()
	return len(d.dials)
}

func start(t *testing.T, d *fakeDialer, cfg Config) (chan<- []byte, func()) {
	t.Helper()
	cfg.Dial = d.Dial
	if cfg.IdleTimeout == 0 {
		cfg.IdleTimeout = time.Hour
	}
	if cfg.MinBackoff == 0 {
		cfg.MinBackoff = 50 * time.Millisecond
	}
	if cfg.MaxBackoff == 0 {
		cfg.MaxBackoff = 50 * time.Millisecond
	}
	cfg.Logf = t.Logf
	in := make(chan []byte, 64)
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	go func() { New(cfg).Run(ctx, in); close(done) }()
	return in, func() { cancel(); <-done }
}

func eventually(t *testing.T, what string, cond func() bool) {
	t.Helper()
	deadline := time.Now().Add(2 * time.Second)
	for !cond() {
		if time.Now().After(deadline) {
			t.Fatalf("timed out waiting for %s", what)
		}
		time.Sleep(5 * time.Millisecond)
	}
}

func TestFramesBeforeHelloAreDroppedWithoutDialing(t *testing.T) {
	d := &fakeDialer{}
	in, stop := start(t, d, Config{})
	in <- frame(0)
	in <- frame(1)
	time.Sleep(50 * time.Millisecond)
	stop()
	if d.dialCount() != 0 {
		t.Fatalf("dialed %d times without a hello", d.dialCount())
	}
}

func TestStreamStartsWithHelloThenFramesInOrder(t *testing.T) {
	d := &fakeDialer{}
	in, stop := start(t, d, Config{})
	in <- hello1
	in <- frame(0)
	in <- hello1 // periodic repeat: not sent again
	in <- frame(1)
	eventually(t, "3 chunks", func() bool { s := d.stream(0); c, _ := snap(s); return len(c) == 3 })
	stop()
	chunks, closed := d.stream(0).snapshot()
	if !bytes.Equal(chunks[0], hello1) || !bytes.Equal(chunks[1], frame(0)) || !bytes.Equal(chunks[2], frame(1)) {
		t.Fatalf("chunks %v", chunks)
	}
	if !closed || d.dialCount() != 1 {
		t.Fatalf("closed %v dials %d", closed, d.dialCount())
	}
}

func TestChangedHelloRestartsTheStream(t *testing.T) {
	d := &fakeDialer{}
	in, stop := start(t, d, Config{})
	in <- hello1
	in <- frame(0)
	in <- hello2
	in <- frame(1)
	eventually(t, "second stream", func() bool { c, _ := snap(d.stream(1)); return len(c) == 2 })
	stop()
	first, closed := d.stream(0).snapshot()
	second, _ := d.stream(1).snapshot()
	if !closed || len(first) != 2 {
		t.Fatalf("first stream %v closed %v", first, closed)
	}
	if !bytes.Equal(second[0], hello2) || !bytes.Equal(second[1], frame(1)) {
		t.Fatalf("second stream %v", second)
	}
}

func TestSendFailureReconnectsAfterBackoffAndResendsHello(t *testing.T) {
	d := &fakeDialer{prepare: func(n int, s *fakeStream) {
		if n == 0 {
			s.failAt = 2 // hello and frame 0 go through, frame 1 fails
		}
	}}
	in, stop := start(t, d, Config{MinBackoff: 100 * time.Millisecond, MaxBackoff: 100 * time.Millisecond})
	in <- hello1
	in <- frame(0)
	in <- frame(1) // fails: stream closed, backoff starts
	in <- frame(2) // during backoff: dropped
	eventually(t, "first stream closed", func() bool { _, closed := snap(d.stream(0)); return closed })
	time.Sleep(150 * time.Millisecond)
	in <- frame(3) // after backoff: redial with the cached hello
	eventually(t, "second stream", func() bool { c, _ := snap(d.stream(1)); return len(c) == 2 })
	stop()
	second, _ := d.stream(1).snapshot()
	if !bytes.Equal(second[0], hello1) || !bytes.Equal(second[1], frame(3)) {
		t.Fatalf("second stream %v", second)
	}
	if d.dialCount() != 2 {
		t.Fatalf("dials %d", d.dialCount())
	}
}

func TestDialFailureBacksOff(t *testing.T) {
	d := &fakeDialer{failN: 1}
	in, stop := start(t, d, Config{MinBackoff: 100 * time.Millisecond, MaxBackoff: 100 * time.Millisecond})
	in <- hello1
	in <- frame(0) // dial fails
	in <- frame(1) // within the backoff: no new dial
	time.Sleep(30 * time.Millisecond)
	if d.dialCount() != 1 {
		t.Fatalf("redialed during backoff: %d dials", d.dialCount())
	}
	time.Sleep(100 * time.Millisecond)
	in <- frame(2)
	eventually(t, "stream after backoff", func() bool { c, _ := snap(d.stream(0)); return len(c) == 2 })
	stop()
}

func TestIdleStreamIsClosedAndReopenedOnNextFrame(t *testing.T) {
	d := &fakeDialer{}
	in, stop := start(t, d, Config{IdleTimeout: 80 * time.Millisecond})
	in <- hello1
	in <- frame(0)
	eventually(t, "idle close", func() bool { _, closed := snap(d.stream(0)); return closed })
	in <- frame(1)
	eventually(t, "reopened stream", func() bool { c, _ := snap(d.stream(1)); return len(c) == 2 })
	stop()
}

func TestPumpDropsWhenTheQueueIsFull(t *testing.T) {
	queue := make(chan []byte, 2)
	reads := [][]byte{frame(0), frame(1), frame(2), nil}
	i := 0
	var dropped uint64
	Pump(func() []byte { b := reads[i]; i++; return b }, queue, &dropped)
	if len(queue) != 2 || dropped != 1 {
		t.Fatalf("queued %d dropped %d", len(queue), dropped)
	}
}

func snap(s *fakeStream) ([][]byte, bool) {
	if s == nil {
		return nil, false
	}
	return s.snapshot()
}
