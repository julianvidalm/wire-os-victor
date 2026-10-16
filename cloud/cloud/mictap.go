package main

import (
	"context"
	"crypto/tls"
	"time"

	"github.com/digital-dream-labs/vector-cloud/internal/config"
	"github.com/digital-dream-labs/vector-cloud/internal/ipc"
	"github.com/digital-dream-labs/vector-cloud/internal/log"
	"github.com/digital-dream-labs/vector-cloud/internal/mictap"
	"github.com/digital-dream-labs/vector-cloud/internal/robot"

	"github.com/gwatts/rootcerts"
	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials"
)

// micTapQueueLen holds about 2.5 s of mic frames while an upload (re)connects.
const micTapQueueLen = 256

// runMicTap forwards vic-anim's MicTap datagrams (mictap_sock) to wire-pod, on the chipper address from
// server_config.json, over TLS with the trusted roots that include the pinned wire-pod certificate.
func runMicTap() {
	esn, err := robot.ReadESN()
	if err != nil {
		log.Println("mictap: cannot read ESN, not forwarding:", err)
		return
	}
	creds := credentials.NewTLS(&tls.Config{RootCAs: rootcerts.ServerCertPool()})
	conn, err := grpc.NewClient(config.Env.Chipper, grpc.WithTransportCredentials(creds))
	if err != nil {
		log.Println("mictap: cannot create client for", config.Env.Chipper, err)
		return
	}
	defer conn.Close()

	sock := getSocketWithRetry(ipc.GetSocketPath("mictap_sock"), "cp_mictap")
	defer sock.Close()
	// vic-anim only learns its client from a datagram; repeat the connection packet so a restarted vic-anim
	// finds us again.
	go func() {
		for range time.Tick(5 * time.Second) {
			sock.Write([]byte("ANKICONN"))
		}
	}()

	queue := make(chan []byte, micTapQueueLen)
	var queueDropped uint64
	go mictap.Pump(sock.ReadBlock, queue, &queueDropped)

	log.Println("mictap: forwarding to", config.Env.Chipper)
	mictap.New(mictap.Config{
		Dial: mictap.NewGRPCDialer(conn, esn),
		Logf: func(format string, a ...any) { log.Printf(format+"\n", a...) },
	}).Run(context.Background(), queue)
}
