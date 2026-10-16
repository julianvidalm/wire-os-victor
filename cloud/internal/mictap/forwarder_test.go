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

func join(parts ...[]byte) []byte { return bytes.Join(parts, nil) }

// fakeStream records what one upload received.
type fakeStream struct {
	mu     sync.Mutex
	chunks [][]byte
	closed bool
	failAt int // Send fails once this many chunks were sent (0: never)
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

// bytes returns everything the upload received, and the number of Send calls.
func (s *fakeStream) bytes() ([]byte, int, bool) {
	if s == nil {
		return nil, 0, false
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	return join(s.chunks...), len(s.chunks), s.closed
}

// fakeDialer hands out streams in order.
type fakeDialer struct {
	mu      sync.Mutex
	streams []*fakeStream
	failN   int // the first failN dials fail
	dials   int
	prepare func(n int, s *fakeStream)
}

func (d *fakeDialer) Dial(context.Context) (Stream, error) {
	d.mu.Lock()
	defer d.mu.Unlock()
	d.dials++
	if d.dials <= d.failN {
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
	return d.dials
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
	if cfg.FlushInterval == 0 {
		cfg.FlushInterval = 5 * time.Millisecond
	}
	cfg.Logf = t.Logf
	in := make(chan []byte, 256)
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

func received(d *fakeDialer, i int) []byte { b, _, _ := d.stream(i).bytes(); return b }

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
	want := join(hello1, frame(0), frame(1))
	eventually(t, "hello and both frames", func() bool { return bytes.Equal(received(d, 0), want) })
	stop()
	if _, _, closed := d.stream(0).bytes(); !closed || d.dialCount() != 1 {
		t.Fatalf("closed %v dials %d", closed, d.dialCount())
	}
}

func TestFramesAreBatchedIntoFewUploads(t *testing.T) {
	d := &fakeDialer{}
	in, stop := start(t, d, Config{FlushInterval: 100 * time.Millisecond})
	in <- hello1
	for seq := byte(0); seq < 30; seq++ { // 30 frames well within one flush interval
		in <- frame(seq)
	}
	eventually(t, "30 frames", func() bool { return len(received(d, 0)) == len(hello1)+30*len(frame(0)) })
	stop()
	if _, sends, _ := d.stream(0).bytes(); sends > 3 {
		t.Fatalf("%d Send calls for one hello and 30 frames: frames are not batched", sends)
	}
}

func TestChangedHelloRestartsTheStream(t *testing.T) {
	d := &fakeDialer{}
	in, stop := start(t, d, Config{})
	in <- hello1
	in <- frame(0)
	in <- hello2
	in <- frame(1)
	eventually(t, "second stream", func() bool { return bytes.Equal(received(d, 1), join(hello2, frame(1))) })
	stop()
	first, _, closed := d.stream(0).bytes()
	if !closed || !bytes.Equal(first, join(hello1, frame(0))) {
		t.Fatalf("first stream %v closed %v", first, closed)
	}
}

func TestSendFailureReconnectsAfterBackoffAndResendsHello(t *testing.T) {
	d := &fakeDialer{prepare: func(n int, s *fakeStream) {
		if n == 0 {
			s.failAt = 2 // the hello and one batch go through, the next batch fails
		}
	}}
	in, stop := start(t, d, Config{MinBackoff: 100 * time.Millisecond, MaxBackoff: 100 * time.Millisecond})
	in <- hello1
	in <- frame(0)
	eventually(t, "first batch", func() bool { _, n, _ := d.stream(0).bytes(); return n == 2 })
	in <- frame(1) // this batch fails: stream closed, backoff starts
	eventually(t, "first stream closed", func() bool { _, _, closed := d.stream(0).bytes(); return closed })
	in <- frame(2) // during backoff: dropped
	time.Sleep(150 * time.Millisecond)
	in <- frame(3) // after backoff: redial with the cached hello
	eventually(t, "second stream", func() bool { return bytes.Equal(received(d, 1), join(hello1, frame(3))) })
	stop()
	if d.dialCount() != 2 {
		t.Fatalf("dials %d", d.dialCount())
	}
}

// A server that accepts the connection but rejects every upload (for example an older wire-pod answering
// Unimplemented) must not be retried every MinBackoff forever: the backoff keeps growing until an upload works.
func TestRejectedUploadsBackOffExponentially(t *testing.T) {
	d := &fakeDialer{prepare: func(_ int, s *fakeStream) { s.failAt = 1 }} // hello accepted, first batch fails
	in, stop := start(t, d, Config{MinBackoff: 40 * time.Millisecond, MaxBackoff: time.Second})
	in <- hello1
	deadline := time.Now().Add(500 * time.Millisecond)
	for seq := byte(0); time.Now().Before(deadline); seq++ {
		in <- frame(seq)
		time.Sleep(5 * time.Millisecond)
	}
	stop()
	// Backoffs 40, 80, 160, 320 ms: at most 4-5 attempts in 500 ms; a constant 40 ms backoff gives about 12.
	if n := d.dialCount(); n > 5 {
		t.Fatalf("%d uploads in 500 ms: the backoff does not grow for rejected uploads", n)
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
	eventually(t, "stream after backoff", func() bool { return bytes.Equal(received(d, 0), join(hello1, frame(2))) })
	stop()
}

func TestIdleStreamIsClosedAndReopenedOnNextFrame(t *testing.T) {
	d := &fakeDialer{}
	in, stop := start(t, d, Config{IdleTimeout: 80 * time.Millisecond})
	in <- hello1
	in <- frame(0)
	eventually(t, "idle close", func() bool { _, _, closed := d.stream(0).bytes(); return closed })
	if !bytes.Equal(received(d, 0), join(hello1, frame(0))) {
		t.Fatalf("pending frames were not flushed before the idle close: %v", received(d, 0))
	}
	in <- frame(1)
	eventually(t, "reopened stream", func() bool { return bytes.Equal(received(d, 1), join(hello1, frame(1))) })
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
