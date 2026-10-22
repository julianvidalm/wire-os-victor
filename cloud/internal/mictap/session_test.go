package mictap

import (
	"bytes"
	"context"
	"io"
	"net"
	"sync"
	"testing"
	"time"

	pb "github.com/digital-dream-labs/vector-cloud/internal/proto/mictap"
	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/test/bufconn"
)

// sessionServer is a stand-in for wire-pod with the Session rpc: it records the audio and answers with one
// command once audio arrived.
type sessionServer struct {
	pb.UnimplementedMicTapServer
	mu       sync.Mutex
	bytes    []byte
	statuses []*pb.AudioStatus
}

func (s *sessionServer) Session(stream pb.MicTap_SessionServer) error {
	answered := false
	for {
		chunk, err := stream.Recv()
		if err == io.EOF {
			return nil
		}
		if err != nil {
			return err
		}
		s.mu.Lock()
		s.bytes = append(s.bytes, chunk.GetData()...)
		if chunk.GetAudioStatus() != nil {
			s.statuses = append(s.statuses, chunk.GetAudioStatus())
		}
		s.mu.Unlock()
		if !answered {
			answered = true
			stream.Send(&pb.Command{Command: &pb.Command_IntentResult{IntentResult: &pb.IntentResult{Intent: "intent_clock_time"}}})
		}
	}
}

// streamOnlyServer is an older wire-pod without Session.
type streamOnlyServer struct {
	pb.UnimplementedMicTapServer
	mu    sync.Mutex
	bytes []byte
}

func (s *streamOnlyServer) Stream(stream pb.MicTap_StreamServer) error {
	for {
		chunk, err := stream.Recv()
		if err == io.EOF {
			return stream.SendAndClose(&pb.StreamSummary{})
		}
		if err != nil {
			return err
		}
		s.mu.Lock()
		s.bytes = append(s.bytes, chunk.GetData()...)
		s.mu.Unlock()
	}
}

func dialBufconn(t *testing.T, register func(*grpc.Server)) *grpc.ClientConn {
	t.Helper()
	lis := bufconn.Listen(1 << 20)
	srv := grpc.NewServer()
	register(srv)
	go srv.Serve(lis)
	t.Cleanup(srv.Stop)
	conn, err := grpc.NewClient("passthrough:///bufnet",
		grpc.WithContextDialer(func(ctx context.Context, _ string) (net.Conn, error) { return lis.DialContext(ctx) }),
		grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { conn.Close() })
	return conn
}

func TestSessionDialerDeliversCommands(t *testing.T) {
	srv := &sessionServer{}
	conn := dialBufconn(t, func(g *grpc.Server) { pb.RegisterMicTapServer(g, srv) })
	got := make(chan *pb.Command, 1)
	dial := NewSessionDialer(conn, "0dd1b652", func(c *pb.Command) { got <- c }, t.Logf)
	s, err := dial(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	if err := s.Send(hello1); err != nil {
		t.Fatal(err)
	}
	select {
	case c := <-got:
		if c.GetIntentResult().GetIntent() != "intent_clock_time" {
			t.Fatalf("command %v", c)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("no command delivered")
	}
	if err := s.CloseAndRecv(); err != nil {
		t.Fatal(err)
	}
	srv.mu.Lock()
	defer srv.mu.Unlock()
	if !bytes.Equal(srv.bytes, hello1) {
		t.Fatalf("server got %v", srv.bytes)
	}
}

func TestSessionDialerFallsBackToStreamOnAnOlderServer(t *testing.T) {
	srv := &streamOnlyServer{}
	conn := dialBufconn(t, func(g *grpc.Server) { pb.RegisterMicTapServer(g, srv) })
	dial := NewSessionDialer(conn, "0dd1b652", func(*pb.Command) {}, t.Logf)
	// The first upload uses Session, which the server rejects; the forwarder then redials, and the dialer
	// switches to Stream for good.
	deadline := time.Now().Add(2 * time.Second)
	for {
		s, err := dial(context.Background())
		if err == nil {
			s.Send(hello1)
			s.CloseAndRecv()
		}
		srv.mu.Lock()
		n := len(srv.bytes)
		srv.mu.Unlock()
		if n > 0 {
			break
		}
		if time.Now().After(deadline) {
			t.Fatal("audio never reached the Stream-only server")
		}
		time.Sleep(10 * time.Millisecond)
	}
}

func TestStatusSenderReportsOnTheOpenSession(t *testing.T) {
	srv := &sessionServer{}
	conn := dialBufconn(t, func(g *grpc.Server) { pb.RegisterMicTapServer(g, srv) })
	var reports StatusSender
	st := &pb.AudioStatus{StreamId: 4, State: pb.AudioStatus_COMPLETED}
	if err := reports.Send(st); err != ErrNoSession {
		t.Fatalf("send without a session: %v", err)
	}
	dial := NewSessionDialerWithStatus(conn, "0dd1b652", func(*pb.Command) {}, &reports, t.Logf)
	s, err := dial(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	// audio and reports share the call
	var wg sync.WaitGroup
	wg.Add(2)
	go func() { defer wg.Done(); s.Send(hello1) }()
	go func() { defer wg.Done(); reports.Send(st) }()
	wg.Wait()
	if err := s.CloseAndRecv(); err != nil {
		t.Fatal(err)
	}
	srv.mu.Lock()
	if !bytes.Equal(srv.bytes, hello1) || len(srv.statuses) != 1 || srv.statuses[0].GetStreamId() != 4 ||
		srv.statuses[0].GetState() != pb.AudioStatus_COMPLETED {
		t.Fatalf("server got %v and %v", srv.bytes, srv.statuses)
	}
	srv.mu.Unlock()
	if err := reports.Send(st); err != ErrNoSession {
		t.Fatalf("send after the session ended: %v", err)
	}
}
