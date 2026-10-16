package mictap

import (
	"context"
	"errors"
	"sync"
	"testing"
	"time"
)

// fakeConn is a datagram connection to vic-anim: it yields its datagrams, then blocks until closed.
type fakeConn struct {
	mu        sync.Mutex
	datagrams chan []byte
	writes    []string
	failWrite bool
	closed    chan struct{}
	once      sync.Once
}

func newFakeConn(failWrite bool, datagrams ...[]byte) *fakeConn {
	c := &fakeConn{datagrams: make(chan []byte, len(datagrams)), failWrite: failWrite, closed: make(chan struct{})}
	for _, d := range datagrams {
		c.datagrams <- d
	}
	return c
}

func (c *fakeConn) ReadBlock() []byte {
	select {
	case b := <-c.datagrams:
		return b
	case <-c.closed:
		return nil
	}
}

func (c *fakeConn) Write(b []byte) (int, error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.writes = append(c.writes, string(b))
	if c.failWrite {
		return 0, errors.New("connection refused")
	}
	return len(b), nil
}

func (c *fakeConn) Close() error { c.once.Do(func() { close(c.closed) }); return nil }

func (c *fakeConn) isClosed() bool {
	select {
	case <-c.closed:
		return true
	default:
		return false
	}
}

func TestSourceReconnectsWhenTheKeepAliveFails(t *testing.T) {
	// The first connection points at a socket vic-anim has since replaced (MicTap switched off and on, or
	// vic-anim restarted): its keep-alive write fails, so the source must open a new connection.
	stale := newFakeConn(true, []byte("a"))
	fresh := newFakeConn(false, []byte("b"))
	conns := []*fakeConn{stale, fresh}
	var mu sync.Mutex
	opened := 0
	open := func() (Conn, error) {
		mu.Lock()
		defer mu.Unlock()
		if opened >= len(conns) {
			return nil, errors.New("no more")
		}
		c := conns[opened]
		opened++
		return c, nil
	}
	queue := make(chan []byte, 8)
	var dropped uint64
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	go func() { RunSource(ctx, open, 20*time.Millisecond, queue, &dropped, t.Logf); close(done) }()

	// The stale connection is closed as soon as its announcement fails, so what it had queued may be lost;
	// what matters is that the replacement connection delivers.
	got := map[string]bool{}
	deadline := time.After(2 * time.Second)
	for !got["b"] {
		select {
		case b := <-queue:
			got[string(b)] = true
		case <-deadline:
			t.Fatalf("datagrams received: %v", got)
		}
	}
	cancel()
	<-done
	if !stale.isClosed() || !fresh.isClosed() {
		t.Fatalf("connections not closed: stale %v fresh %v", stale.isClosed(), fresh.isClosed())
	}
	fresh.mu.Lock()
	defer fresh.mu.Unlock()
	if len(fresh.writes) == 0 || fresh.writes[0] != connectionPacket {
		t.Fatalf("fresh connection keep-alive writes %v", fresh.writes)
	}
}

func TestSourceRetriesOpeningUntilTheSocketExists(t *testing.T) {
	var mu sync.Mutex
	attempts := 0
	conn := newFakeConn(false, []byte("x"))
	open := func() (Conn, error) {
		mu.Lock()
		defer mu.Unlock()
		attempts++
		if attempts < 3 {
			return nil, errors.New("no such file")
		}
		return conn, nil
	}
	queue := make(chan []byte, 8)
	var dropped uint64
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	go func() { RunSource(ctx, open, 10*time.Millisecond, queue, &dropped, t.Logf); close(done) }()
	select {
	case <-queue:
	case <-time.After(2 * time.Second):
		t.Fatal("no datagram after the socket appeared")
	}
	cancel()
	<-done
}
