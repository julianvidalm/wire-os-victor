package mictap

import (
	"bytes"
	"context"
	"io"
	"net"
	"sync"
	"testing"

	pb "github.com/digital-dream-labs/vector-cloud/internal/proto/mictap"
	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/metadata"
	"google.golang.org/grpc/test/bufconn"
)

// recordingServer is a stand-in for wire-pod's MicTap service.
type recordingServer struct {
	pb.UnimplementedMicTapServer
	mu    sync.Mutex
	esn   string
	bytes []byte
	done  chan struct{}
}

func (s *recordingServer) Stream(stream pb.MicTap_StreamServer) error {
	md, _ := metadata.FromIncomingContext(stream.Context())
	var data []byte
	for {
		chunk, err := stream.Recv()
		if err == io.EOF {
			break
		}
		if err != nil {
			return err
		}
		data = append(data, chunk.GetData()...)
	}
	s.mu.Lock()
	if v := md.Get(ESNMetadataKey); len(v) > 0 {
		s.esn = v[0]
	}
	s.bytes = data
	s.mu.Unlock()
	close(s.done)
	return stream.SendAndClose(&pb.StreamSummary{MicFrames: 2})
}

func TestGRPCDialerSendsESNAndChunks(t *testing.T) {
	lis := bufconn.Listen(1 << 20)
	rec := &recordingServer{done: make(chan struct{})}
	srv := grpc.NewServer()
	pb.RegisterMicTapServer(srv, rec)
	go srv.Serve(lis)
	defer srv.Stop()

	conn, err := grpc.NewClient("passthrough:///bufnet",
		grpc.WithContextDialer(func(ctx context.Context, _ string) (net.Conn, error) { return lis.DialContext(ctx) }),
		grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close()

	s, err := NewGRPCDialer(conn, "0dd1b652")(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	for _, b := range [][]byte{hello1, frame(0), frame(1)} {
		if err := s.Send(b); err != nil {
			t.Fatal(err)
		}
	}
	if err := s.CloseAndRecv(); err != nil {
		t.Fatal(err)
	}
	<-rec.done
	rec.mu.Lock()
	defer rec.mu.Unlock()
	want := append(append(append([]byte(nil), hello1...), frame(0)...), frame(1)...)
	if rec.esn != "0dd1b652" || !bytes.Equal(rec.bytes, want) {
		t.Fatalf("server got esn %q bytes %v", rec.esn, rec.bytes)
	}
}
