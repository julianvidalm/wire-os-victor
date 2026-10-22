package main

import (
	"errors"
	"sync"
	"time"

	"github.com/digital-dream-labs/vector-cloud/internal/mictap"
	extint "github.com/digital-dream-labs/vector-cloud/internal/proto/external_interface"
)

// engineAudio plays the cortex's audio through the engine's external audio stream, the path the SDK's
// ExternalAudioStreamPlayback uses (vic-gateway runs inside vic-cloud, so the engine's proto socket is here).
type engineAudio struct{}

var errGatewayNotReady = errors.New("engine proto socket not connected yet")

// gatewayReady tells whether mainGateway has set up the engine proto manager (its channel map comes first).
func gatewayReady() bool {
	engineProtoManager.connMutex.Lock()
	defer engineProtoManager.connMutex.Unlock()
	return engineProtoManager.conn != nil
}

func (engineAudio) write(msg *extint.GatewayWrapper) error {
	if !gatewayReady() {
		return errGatewayNotReady
	}
	_, _, err := engineProtoManager.Write(msg)
	return err
}

func (e engineAudio) Prepare(sampleRate, volume uint32) error {
	return e.write(&extint.GatewayWrapper{OneofMessageType: &extint.GatewayWrapper_ExternalAudioStreamPrepare{
		ExternalAudioStreamPrepare: &extint.ExternalAudioStreamPrepare{AudioFrameRate: sampleRate, AudioVolume: volume},
	}})
}

func (e engineAudio) Chunk(samples []byte) error {
	return e.write(&extint.GatewayWrapper{OneofMessageType: &extint.GatewayWrapper_ExternalAudioStreamChunk{
		ExternalAudioStreamChunk: &extint.ExternalAudioStreamChunk{
			AudioChunkSizeBytes: uint32(len(samples)),
			AudioChunkSamples:   samples,
		},
	}})
}

func (e engineAudio) Complete() error {
	return e.write(&extint.GatewayWrapper{OneofMessageType: &extint.GatewayWrapper_ExternalAudioStreamComplete{
		ExternalAudioStreamComplete: &extint.ExternalAudioStreamComplete{},
	}})
}

func (e engineAudio) Cancel() error {
	return e.write(&extint.GatewayWrapper{OneofMessageType: &extint.GatewayWrapper_ExternalAudioStreamCancel{
		ExternalAudioStreamCancel: &extint.ExternalAudioStreamCancel{},
	}})
}

var engineAudioStatusOnce sync.Once

// listenEngineAudioStatus passes the engine's playback reports (SDKComponent::HandleStreamStatusEvent:
// completed, buffer overrun, failure; it reports no "playing" and no "cancelled") to the player. Started on the
// first audio command, when the gateway is long up. The ipc manager drops a listener that is slow for 250 ms,
// so the channel is drained at once and opened again if it is ever closed.
func listenEngineAudioStatus(player *mictap.AudioPlayer) {
	engineAudioStatusOnce.Do(func() {
		go func() {
			for {
				if !gatewayReady() {
					time.Sleep(time.Second)
					continue
				}
				// closed (and already removed) by the manager if this loop was ever too slow
				_, ch := engineProtoManager.CreateChannel(&extint.GatewayWrapper_ExternalAudioStreamResponse{}, 8)
				for {
					msg, ok := <-ch
					if !ok {
						break
					}
					resp := msg.GetExternalAudioStreamResponse()
					switch {
					case resp.GetAudioStreamPlaybackComplete() != nil:
						player.EngineStatus(mictap.EngineAudioCompleted)
					case resp.GetAudioStreamBufferOverrun() != nil:
						player.EngineStatus(mictap.EngineAudioOverflow)
					case resp.GetAudioStreamPlaybackFailyer() != nil:
						player.EngineStatus(mictap.EngineAudioFailed)
					}
				}
				time.Sleep(time.Second)
			}
		}()
	})
}
