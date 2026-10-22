package mictap

import (
	"context"
	"io"
	"sync"
	"testing"
	"time"

	pb "github.com/digital-dream-labs/vector-cloud/internal/proto/mictap"
	"google.golang.org/grpc"
)

// commandFirstServer is wire-pod with the robot's wake word on the robot: it sends a command as soon as the
// Session opens, with no audio, and ends the first callsEndedEarly calls right after that command.
type commandFirstServer struct {
	pb.UnimplementedMicTapServer
	callsEndedEarly int

	mu       sync.Mutex
	calls    int
	bytes    []byte
	statuses []*pb.AudioStatus
}

func (s *commandFirstServer) Session(stream pb.MicTap_SessionServer) error {
	s.mu.Lock()
	s.calls++
	endEarly := s.calls <= s.callsEndedEarly
	s.mu.Unlock()
	if err := stream.Send(&pb.Command{Command: &pb.Command_IntentResult{IntentResult: &pb.IntentResult{Intent: "intent_clock_time"}}}); err != nil {
		return err
	}
	if endEarly {
		return nil
	}
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
	}
}

func (s *commandFirstServer) snapshot() (calls int, bytes int, statuses int) {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.calls, len(s.bytes), len(s.statuses)
}

func TestLinkOpensACommandSessionWithoutAudio(t *testing.T) {
	srv := &commandFirstServer{}
	conn := dialBufconn(t, func(g *grpc.Server) { pb.RegisterMicTapServer(g, srv) })
	got := make(chan *pb.Command, 1)
	var reports StatusSender
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	go func() {
		defer close(done)
		RunLink(ctx, LinkConfig{
			Dial: NewSessionDialerWithStatus(conn, "0dd1b652", func(c *pb.Command) { got <- c }, &reports, t.Logf),
			Logf: t.Logf,
		})
	}()
	select {
	case c := <-got:
		if c.GetIntentResult().GetIntent() != "intent_clock_time" {
			t.Fatalf("command %v", c)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("no command delivered without audio")
	}
	// the open link also carries the audio status reports upstream
	if err := reports.Send(&pb.AudioStatus{StreamId: 2, State: pb.AudioStatus_COMPLETED}); err != nil {
		t.Fatal(err)
	}
	deadline := time.Now().Add(2 * time.Second)
	for {
		_, n, statuses := srv.snapshot()
		if statuses == 1 {
			if n != 0 {
				t.Fatalf("the link sent %d audio bytes", n)
			}
			break
		}
		if time.Now().After(deadline) {
			t.Fatal("status report never arrived")
		}
		time.Sleep(10 * time.Millisecond)
	}
	cancel()
	select {
	case <-done:
	case <-time.After(2 * time.Second):
		t.Fatal("RunLink did not stop with its context")
	}
}

func TestLinkRedialsWhenTheCortexEndsTheSession(t *testing.T) {
	srv := &commandFirstServer{callsEndedEarly: 2}
	conn := dialBufconn(t, func(g *grpc.Server) { pb.RegisterMicTapServer(g, srv) })
	got := make(chan *pb.Command, 8)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go RunLink(ctx, LinkConfig{
		Dial:       NewSessionDialer(conn, "0dd1b652", func(c *pb.Command) { got <- c }, t.Logf),
		MinBackoff: 10 * time.Millisecond,
		MaxBackoff: 20 * time.Millisecond,
		Logf:       t.Logf,
	})
	for i := 0; i < 3; i++ {
		select {
		case <-got:
		case <-time.After(2 * time.Second):
			t.Fatalf("command %d never arrived: the link did not redial", i+1)
		}
	}
	if calls, _, _ := srv.snapshot(); calls != 3 {
		t.Fatalf("%d calls, want 3 (two ended by the cortex, one open)", calls)
	}
}
