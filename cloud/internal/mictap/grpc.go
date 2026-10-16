package mictap

import (
	"context"

	pb "github.com/digital-dream-labs/vector-cloud/internal/proto/mictap"
	"google.golang.org/grpc"
	"google.golang.org/grpc/metadata"
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
