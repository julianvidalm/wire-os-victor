package main

import (
	"context"
	"crypto/tls"
	"time"

	"github.com/digital-dream-labs/vector-cloud/internal/config"
	"github.com/digital-dream-labs/vector-cloud/internal/log"
	"github.com/digital-dream-labs/vector-cloud/internal/mictap"
	mictappb "github.com/digital-dream-labs/vector-cloud/internal/proto/mictap"
	"github.com/digital-dream-labs/vector-cloud/internal/robot"
	"github.com/digital-dream-labs/vector-cloud/internal/voice"

	"github.com/gwatts/rootcerts"
	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials"
)

// runMicTap keeps a command link to the cortex (wire-pod), on the chipper address from server_config.json, over
// TLS with the trusted roots that include the pinned wire-pod certificate. The robot hears its own wake word and
// streams the voice request to chipper as stock Vector does; the link carries no audio, only the cortex's
// commands (intents, its voice) and the reports on that voice.
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

	logf := func(format string, a ...any) { log.Printf(format+"\n", a...) }

	// The cortex's own voice: Command.audio steps go to the engine's external audio stream, never blocking the
	// Session's receive loop; what happened to each stream goes back upstream on the same Session.
	var reports mictap.StatusSender
	audio := mictap.NewAudioPlayer(engineAudio{}, mictap.DefaultAudioQueueLen, func(st *mictappb.AudioStatus) {
		if err := reports.Send(st); err != nil {
			logf("mictap: audio status %d %s not reported: %v", st.GetStreamId(), st.GetState(), err)
		}
	}, logf)

	log.Println("mictap: command link to", config.Env.Chipper)
	// a wire-pod restart is noticed within 10 s; commands sent before the robot redials are lost
	mictap.RunLink(context.Background(), mictap.LinkConfig{
		MaxBackoff: 10 * time.Second,
		// a new or closed Session cancels whatever vic-anim plays, so a stream cut off by a vic-cloud restart
		// or a lost Session never blocks the next one
		Dial: mictap.ResetOnSession(mictap.NewSessionDialerWithStatus(conn, esn, func(cmd *mictappb.Command) {
			if a := cmd.GetAudio(); a != nil {
				listenEngineAudioStatus(audio)
				audio.Handle(a)
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
		}, &reports, logf), audio),
		Logf: logf,
	})
}
