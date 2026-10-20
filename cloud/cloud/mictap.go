package main

import (
	"context"
	"crypto/tls"
	"time"

	"github.com/digital-dream-labs/vector-cloud/internal/config"
	"github.com/digital-dream-labs/vector-cloud/internal/ipc"
	"github.com/digital-dream-labs/vector-cloud/internal/log"
	"github.com/digital-dream-labs/vector-cloud/internal/mictap"
	mictappb "github.com/digital-dream-labs/vector-cloud/internal/proto/mictap"
	"github.com/digital-dream-labs/vector-cloud/internal/robot"
	"github.com/digital-dream-labs/vector-cloud/internal/voice"

	"github.com/gwatts/rootcerts"
	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials"
)

// micTapQueueLen holds about 2.5 s of mic frames while an upload (re)connects.
const micTapQueueLen = 256

// runMicTap forwards vic-anim's MicTap datagrams (mictap_sock) to wire-pod, on the chipper address from
// server_config.json, over TLS with the trusted roots that include the pinned wire-pod certificate.
func runMicTap(process *voice.Process) {
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

	// The cortex's own voice: Command.audio steps go to the engine's external audio stream, never blocking the
	// Session's receive loop; what happened to each stream goes back upstream on the same Session.
	var reports mictap.StatusSender
	audio := mictap.NewAudioPlayer(engineAudio{}, mictap.DefaultAudioQueueLen, func(st *mictappb.AudioStatus) {
		if err := reports.Send(st); err != nil {
			logf("mictap: audio status %d %s not reported: %v", st.GetStreamId(), st.GetState(), err)
		}
	}, logf)

	log.Println("mictap: forwarding to", config.Env.Chipper)
	mictap.New(mictap.Config{
		Dial: mictap.NewSessionDialerWithStatus(conn, esn, func(cmd *mictappb.Command) {
			if a := cmd.GetAudio(); a != nil {
				listenEngineAudioStatus(audio)
				audio.Handle(a)
				return
			}
			// The cortex heard Vector's name: wake up as if "Hey Vector" was heard. The robot's voice request
			// that follows is answered by the cortex with the sentence it already transcribed.
			if cmd.GetWake() != nil {
				logf("mictap: cortex wake")
				if err := engineCladManager.SendFakeTriggerWord(); err != nil {
					logf("mictap: cannot wake: %v", err)
				}
				return
			}
			// The cortex answers what it heard: deliver it to the engine like a chipper response.
			result, err := mictap.CommandToIntent(cmd)
			if err != nil {
				logf("mictap: ignoring command: %v", err)
				return
			}
			logf("mictap: cortex command %s", result.Intent)
			process.InjectResult(result)
		}, &reports, logf),
		Logf: logf,
	}).Run(context.Background(), queue)
}
