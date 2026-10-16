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

	// RunSource reconnects to mictap_sock whenever vic-anim replaces it (MicTap switched off and on, or a
	// vic-anim restart), which a fixed connection would never notice.
	queue := make(chan []byte, micTapQueueLen)
	var queueDropped uint64
	path := ipc.GetSocketPath("mictap_sock")
	open := func() (mictap.Conn, error) { return ipc.NewUnixgramClient(path, "cp_mictap") }
	logf := func(format string, a ...any) { log.Printf(format+"\n", a...) }
	go mictap.RunSource(context.Background(), open, 5*time.Second, queue, &queueDropped, logf)

	log.Println("mictap: forwarding to", config.Env.Chipper)
	mictap.New(mictap.Config{
		Dial: mictap.NewGRPCDialer(conn, esn),
		Logf: logf,
	}).Run(context.Background(), queue)
}
