package mictap

import (
	"errors"
	"sync"

	pb "github.com/digital-dream-labs/vector-cloud/internal/proto/mictap"
)

// AudioEngine plays one audio stream at a time on Vector's speaker (the engine's external audio stream).
type AudioEngine interface {
	Prepare(sampleRate, volume uint32) error
	Chunk(samples []byte) error
	Complete() error
	Cancel() error
}

// EngineAudioState is what the engine reports about the playing stream.
type EngineAudioState int

const (
	EngineAudioCompleted EngineAudioState = iota
	EngineAudioFailed
	EngineAudioOverflow
)

const (
	// maxAudioChunkBytes is the engine's limit for one chunk (SDKComponent::HandleAudioStreamChunkRequest).
	maxAudioChunkBytes = 1024
	minAudioRate       = 8000
	maxAudioRate       = 16025
	defaultAudioVolume = 100
	// DefaultAudioQueueLen holds about 16 s of 16 kHz audio in full chunks (32 ms each).
	DefaultAudioQueueLen = 512
	audioStatusQueueLen  = 16
)

// AudioPlayer turns the cortex's Audio commands into engine calls. Handle never blocks (it runs on the
// Session's receive loop): steps go through a bounded queue to a sender goroutine, and when the queue is full
// the stream is dropped and cancelled. Status reports go upstream through report, also off the caller's
// goroutine.
type AudioPlayer struct {
	engine AudioEngine
	report func(*pb.AudioStatus)
	logf   func(format string, a ...any)

	queue    chan *pb.Audio
	wake     chan struct{}
	statuses chan *pb.AudioStatus

	mu        sync.Mutex
	current   uint32 // stream playing (prepared on the engine), 0 if none
	dropped   uint32 // stream dropped after an overflow; its remaining steps are ignored
	cancelFor uint32 // stream the sender must cancel because of an overflow, 0 if none
}

// NewAudioPlayer starts the sender goroutines. report may be nil (no status upstream).
func NewAudioPlayer(engine AudioEngine, queueLen int, report func(*pb.AudioStatus),
	logf func(format string, a ...any)) *AudioPlayer {
	if queueLen <= 0 {
		queueLen = DefaultAudioQueueLen
	}
	if report == nil {
		report = func(*pb.AudioStatus) {}
	}
	if logf == nil {
		logf = func(string, ...any) {}
	}
	p := &AudioPlayer{
		engine:   engine,
		report:   report,
		logf:     logf,
		queue:    make(chan *pb.Audio, queueLen),
		wake:     make(chan struct{}, 1),
		statuses: make(chan *pb.AudioStatus, audioStatusQueueLen),
	}
	go p.send()
	go p.reportLoop()
	return p
}

// Handle queues one step from the cortex. It never blocks.
func (p *AudioPlayer) Handle(a *pb.Audio) {
	if a == nil {
		return
	}
	select {
	case p.queue <- a:
	default:
		p.overflow(a.GetStreamId())
	}
}

func (p *AudioPlayer) overflow(id uint32) {
	p.mu.Lock()
	first := p.dropped != id
	p.dropped = id
	p.cancelFor = id
	p.mu.Unlock()
	if first {
		p.logf("mictap: audio stream %d dropped: queue full (%d steps)", id, cap(p.queue))
	}
	select {
	case p.wake <- struct{}{}:
	default:
	}
}

// EngineStatus records what the engine reported about the playing stream and passes it upstream.
func (p *AudioPlayer) EngineStatus(state EngineAudioState) {
	p.mu.Lock()
	id := p.current
	p.current = 0
	p.mu.Unlock()
	if id == 0 {
		return
	}
	switch state {
	case EngineAudioCompleted:
		p.logf("mictap: audio stream %d completed", id)
		p.status(id, pb.AudioStatus_COMPLETED)
	case EngineAudioOverflow:
		p.logf("mictap: audio stream %d overflowed the engine buffer", id)
		p.status(id, pb.AudioStatus_OVERFLOW)
	default:
		p.logf("mictap: audio stream %d failed in the engine", id)
		p.status(id, pb.AudioStatus_FAILED)
	}
}

func (p *AudioPlayer) status(id uint32, state pb.AudioStatus_State) {
	select {
	case p.statuses <- &pb.AudioStatus{StreamId: id, State: state}:
	default:
		p.logf("mictap: audio status for stream %d not sent: status queue full", id)
	}
}

func (p *AudioPlayer) reportLoop() {
	for s := range p.statuses {
		p.report(s)
	}
}

func (p *AudioPlayer) send() {
	for {
		select {
		case <-p.wake:
			p.cancelOverflowed()
		case a := <-p.queue:
			// an overflow seen while this step waited is handled before it
			p.cancelOverflowed()
			p.step(a)
		}
	}
}

func (p *AudioPlayer) cancelOverflowed() {
	p.mu.Lock()
	id := p.cancelFor
	p.cancelFor = 0
	playing := id != 0 && p.current == id
	if playing {
		p.current = 0
	}
	p.mu.Unlock()
	if id == 0 {
		return
	}
	if playing {
		if err := p.engine.Cancel(); err != nil {
			p.logf("mictap: cannot cancel audio stream %d: %v", id, err)
		}
	}
	p.status(id, pb.AudioStatus_OVERFLOW)
}

func (p *AudioPlayer) step(a *pb.Audio) {
	id := a.GetStreamId()
	p.mu.Lock()
	current, dropped := p.current, p.dropped
	p.mu.Unlock()

	switch s := a.GetStep().(type) {
	case *pb.Audio_Prepare:
		if id == 0 || id == dropped {
			p.logf("mictap: ignoring audio prepare for stream %d", id)
			return
		}
		rate, volume, err := prepareParams(s.Prepare)
		if err != nil {
			// an invalid stream never replaces the one playing
			p.logf("mictap: audio stream %d: %v", id, err)
			p.status(id, pb.AudioStatus_FAILED)
			return
		}
		if current != 0 {
			// one stream at a time: a new answer replaces the one playing
			p.logf("mictap: audio stream %d replaced by %d", current, id)
			if err := p.engine.Cancel(); err != nil {
				p.logf("mictap: cannot cancel audio stream %d: %v", current, err)
			}
			p.setCurrent(0)
			p.status(current, pb.AudioStatus_CANCELLED)
		}
		if err := p.engine.Prepare(rate, volume); err != nil {
			p.logf("mictap: cannot prepare audio stream %d: %v", id, err)
			p.status(id, pb.AudioStatus_FAILED)
			return
		}
		p.setCurrent(id)
		p.logf("mictap: audio stream %d prepared: %d Hz, volume %d, expected %d ms", id, rate, volume,
			s.Prepare.GetExpectedDurationMs())
		p.status(id, pb.AudioStatus_STARTED)
	case *pb.Audio_Chunk:
		if id != current || id == 0 {
			return // a stream that ended, was replaced or was dropped
		}
		samples := s.Chunk.GetSamples()
		if len(samples) == 0 {
			return
		}
		if len(samples) > maxAudioChunkBytes || len(samples)%2 != 0 {
			p.logf("mictap: audio stream %d: invalid chunk of %d bytes, cancelling", id, len(samples))
			p.engine.Cancel()
			p.setCurrent(0)
			p.status(id, pb.AudioStatus_FAILED)
			return
		}
		if err := p.engine.Chunk(samples); err != nil {
			p.logf("mictap: cannot send audio chunk of stream %d: %v", id, err)
		}
	case *pb.Audio_Complete:
		if id != current || id == 0 {
			return
		}
		// still current until the engine reports the end of playback
		if err := p.engine.Complete(); err != nil {
			p.logf("mictap: cannot complete audio stream %d: %v", id, err)
		}
	case *pb.Audio_Cancel:
		if id != current || id == 0 {
			return
		}
		if err := p.engine.Cancel(); err != nil {
			p.logf("mictap: cannot cancel audio stream %d: %v", id, err)
		}
		p.setCurrent(0)
		p.logf("mictap: audio stream %d cancelled", id)
		p.status(id, pb.AudioStatus_CANCELLED)
	default:
		p.logf("mictap: ignoring empty audio step for stream %d", id)
	}
}

func (p *AudioPlayer) setCurrent(id uint32) {
	p.mu.Lock()
	p.current = id
	p.mu.Unlock()
}

func prepareParams(prep *pb.AudioPrepare) (rate, volume uint32, err error) {
	rate = prep.GetSampleRate()
	if rate < minAudioRate || rate > maxAudioRate {
		return 0, 0, errors.New("sample rate out of 8000-16025 Hz")
	}
	volume = prep.GetVolume()
	if volume == 0 {
		volume = defaultAudioVolume
	}
	if volume > 100 {
		volume = 100
	}
	return rate, volume, nil
}
