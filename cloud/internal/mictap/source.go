package mictap

import (
	"context"
	"time"
)

// connectionPacket is how a datagram client announces itself to vic-anim's LocalUdpServer.
const connectionPacket = "ANKICONN"

// Conn is a datagram connection to vic-anim's mictap_sock (an ipc.Conn).
type Conn interface {
	ReadBlock() []byte
	Write(b []byte) (int, error)
	Close() error
}

// RunSource reads vic-anim's MicTap datagrams into queue until ctx is done. It announces itself every
// keepAlive; when that fails, the socket it holds was replaced (MicTap switched off and on, or vic-anim
// restarted), so it closes the connection and opens a new one.
func RunSource(ctx context.Context, open func() (Conn, error), keepAlive time.Duration, queue chan<- []byte,
	dropped *uint64, logf func(format string, a ...any)) {
	for ctx.Err() == nil {
		conn, err := open()
		if err != nil {
			wait(ctx, keepAlive)
			continue
		}
		pumped := make(chan struct{})
		go func() { Pump(conn.ReadBlock, queue, dropped); close(pumped) }()
		keepConnected(ctx, conn, keepAlive, pumped, logf)
		conn.Close()
		<-pumped
	}
}

// keepConnected announces conn to vic-anim now and every keepAlive, and returns when ctx is done, an
// announcement fails, or the connection stopped delivering.
func keepConnected(ctx context.Context, conn Conn, keepAlive time.Duration, pumped <-chan struct{},
	logf func(format string, a ...any)) {
	tick := time.NewTicker(keepAlive)
	defer tick.Stop()
	for {
		if _, err := conn.Write([]byte(connectionPacket)); err != nil {
			logf("mictap: vic-anim socket gone (%v), reconnecting", err)
			return
		}
		select {
		case <-ctx.Done():
			return
		case <-pumped:
			return
		case <-tick.C:
		}
	}
}

func wait(ctx context.Context, d time.Duration) {
	t := time.NewTimer(d)
	defer t.Stop()
	select {
	case <-ctx.Done():
	case <-t.C:
	}
}
