package mictap

import (
	"context"
	"errors"
	"io"
	"sync"
	"sync/atomic"

	pb "github.com/digital-dream-labs/vector-cloud/internal/proto/mictap"
	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/metadata"
	"google.golang.org/grpc/status"
)

// ESNMetadataKey carries the robot's ESN in the Stream request metadata (see wire-pod's MicTap service).
const ESNMetadataKey = "x-vector-esn"

// NewGRPCDialer returns a Dialer that opens MicTap.Stream on conn (wire-pod's chipper server), sending esn.
func NewGRPCDialer(conn grpc.ClientConnInterface, esn string) Dialer {
	client := pb.NewMicTapClient(conn)
	return func(ctx context.Context) (Stream, error) {
		ctx = metadata.AppendToOutgoingContext(ctx, ESNMetadataKey, esn)
		s, err := client.Stream(ctx)
		if err != nil {
			return nil, err
		}
		return &grpcStream{s: s}, nil
	}
}

type grpcStream struct {
	s pb.MicTap_StreamClient
}

func (g *grpcStream) Send(chunk []byte) error {
	return g.s.Send(&pb.Chunk{Data: chunk})
}

func (g *grpcStream) CloseAndRecv() error {
	_, err := g.s.CloseAndRecv()
	return err
}

// NewSessionDialer returns a Dialer that opens MicTap.Session, so the cortex can send commands back over the
// call the robot opened; each command is passed to onCommand. Against a cortex without Session (it answers
// Unimplemented), it switches to Stream for good: audio still flows, only commands are unavailable.
func NewSessionDialer(conn grpc.ClientConnInterface, esn string, onCommand func(*pb.Command),
	logf func(format string, a ...any)) Dialer {
	return NewSessionDialerWithStatus(conn, esn, onCommand, nil, logf)
}

// NewSessionDialerWithStatus is NewSessionDialer that also lets status (if not nil) send audio status reports
// upstream on whichever Session is open.
func NewSessionDialerWithStatus(conn grpc.ClientConnInterface, esn string, onCommand func(*pb.Command),
	reports *StatusSender, logf func(format string, a ...any)) Dialer {
	client := pb.NewMicTapClient(conn)
	stream := NewGRPCDialer(conn, esn)
	var unsupported atomic.Bool
	return func(ctx context.Context) (Stream, error) {
		if unsupported.Load() {
			return stream(ctx)
		}
		ctx = metadata.AppendToOutgoingContext(ctx, ESNMetadataKey, esn)
		s, err := client.Session(ctx)
		if err != nil {
			return nil, err
		}
		gs := &grpcSession{s: s, done: make(chan struct{})}
		if reports != nil {
			reports.set(gs)
		}
		go func() {
			defer close(gs.done)
			if reports != nil {
				defer reports.clear(gs)
			}
			for {
				cmd, err := s.Recv()
				if err != nil {
					if status.Code(err) == codes.Unimplemented && unsupported.CompareAndSwap(false, true) {
						logf("mictap: the cortex has no Session rpc; streaming without commands")
					}
					if !errors.Is(err, io.EOF) {
						gs.err = err
					}
					return
				}
				onCommand(cmd)
			}
		}()
		return gs, nil
	}
}

type grpcSession struct {
	s    pb.MicTap_SessionClient
	done chan struct{}
	err  error // set before done is closed

	sendMu sync.Mutex // gRPC allows one sender at a time: the link and the audio status reports
}

func (g *grpcSession) Send(chunk []byte) error {
	g.sendMu.Lock()
	defer g.sendMu.Unlock()
	return g.s.Send(&pb.Chunk{Data: chunk})
}

func (g *grpcSession) sendStatus(st *pb.AudioStatus) error {
	g.sendMu.Lock()
	defer g.sendMu.Unlock()
	return g.s.Send(&pb.Chunk{AudioStatus: st})
}

// ErrNoSession means there is no Session open to report on.
var ErrNoSession = errors.New("no MicTap session open")

// StatusSender sends audio status reports on the Session that is open, if any.
type StatusSender struct {
	mu  sync.Mutex
	cur *grpcSession
}

// Send reports st upstream. It fails with ErrNoSession when no Session is open (the report is lost).
func (s *StatusSender) Send(st *pb.AudioStatus) error {
	s.mu.Lock()
	gs := s.cur
	s.mu.Unlock()
	if gs == nil {
		return ErrNoSession
	}
	return gs.sendStatus(st)
}

func (s *StatusSender) set(gs *grpcSession) {
	s.mu.Lock()
	s.cur = gs
	s.mu.Unlock()
}

func (s *StatusSender) clear(gs *grpcSession) {
	s.mu.Lock()
	if s.cur == gs {
		s.cur = nil
	}
	s.mu.Unlock()
}

// Wait returns when the cortex ends the call (or the call fails), with its status.
func (g *grpcSession) Wait() error {
	<-g.done
	return g.err
}

// CloseAndRecv ends the upload and waits for the cortex to close the call, returning its status.
func (g *grpcSession) CloseAndRecv() error {
	g.s.CloseSend()
	<-g.done
	return g.err
}
