// Package mictap holds the generated gRPC client code for mictap.proto, whose canonical copy lives in
// julianvidalm/wire-pod (chipper/pkg/mictap/mictappb/mictap.proto); keep both copies in sync.
//
// Regenerate from this directory with protoc on PATH and the pinned plugins (compatible with this module's
// grpc and protobuf versions):
//
//	go install google.golang.org/protobuf/cmd/protoc-gen-go@v1.36.6
//	go install google.golang.org/grpc/cmd/protoc-gen-go-grpc@v1.5.1
//	go generate ./...
//
// cloud/.gitignore ignores *.pb.go; like vision.pb.go, these generated files are committed with git add -f.
package mictap

//go:generate protoc --go_out=. --go_opt=paths=source_relative --go-grpc_out=. --go-grpc_opt=paths=source_relative mictap.proto
