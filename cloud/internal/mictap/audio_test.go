package mictap

import (
	"context"
	"fmt"
	"sync"
	"testing"
	"time"

	pb "github.com/digital-dream-labs/vector-cloud/internal/proto/mictap"
)

// fakeEngine records the calls in order; block (if set) holds every call until it is closed.
type fakeEngine struct {
	mu    sync.Mutex
	calls []string
	block chan struct{}
}

func (e *fakeEngine) record(s string) error {
	if e.block != nil {
		<-e.block
	}
	e.mu.Lock()
	e.calls = append(e.calls, s)
	e.mu.Unlock()
	return nil
}

func (e *fakeEngine) Prepare(rate, volume uint32) error {
	return e.record(fmt.Sprintf("prepare %d %d", rate, volume))
}
func (e *fakeEngine) Chunk(b []byte) error { return e.record(fmt.Sprintf("chunk %d", len(b))) }
func (e *fakeEngine) Complete() error      { return e.record("complete") }
func (e *fakeEngine) Cancel() error        { return e.record("cancel") }

func (e *fakeEngine) snapshot() []string {
	e.mu.Lock()
	defer e.mu.Unlock()
	return append([]string(nil), e.calls...)
}

type statusLog struct {
	mu sync.Mutex
	s  []string
}

func (l *statusLog) add(st *pb.AudioStatus) {
	l.mu.Lock()
	l.s = append(l.s, fmt.Sprintf("%d %s", st.GetStreamId(), st.GetState()))
	l.mu.Unlock()
}

func (l *statusLog) snapshot() []string {
	l.mu.Lock()
	defer l.mu.Unlock()
	return append([]string(nil), l.s...)
}

func waitFor(t *testing.T, what string, cond func() bool) {
	t.Helper()
	deadline := time.Now().Add(2 * time.Second)
	for !cond() {
		if time.Now().After(deadline) {
			t.Fatalf("timed out waiting for %s", what)
		}
		time.Sleep(time.Millisecond)
	}
}

func equal(a, b []string) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

func prepare(id, rate, volume uint32) *pb.Audio {
	return &pb.Audio{StreamId: id, Step: &pb.Audio_Prepare{Prepare: &pb.AudioPrepare{SampleRate: rate, Volume: volume,
		ExpectedDurationMs: 1000}}}
}
func chunk(id uint32, n int) *pb.Audio {
	return &pb.Audio{StreamId: id, Step: &pb.Audio_Chunk{Chunk: &pb.AudioChunk{Samples: make([]byte, n)}}}
}
func complete(id uint32) *pb.Audio {
	return &pb.Audio{StreamId: id, Step: &pb.Audio_Complete{Complete: &pb.AudioComplete{}}}
}
func cancel(id uint32) *pb.Audio {
	return &pb.Audio{StreamId: id, Step: &pb.Audio_Cancel{Cancel: &pb.AudioCancel{}}}
}

func TestAudioStreamReachesTheEngineAndReportsItsEnd(t *testing.T) {
	e := &fakeEngine{}
	var st statusLog
	p := NewAudioPlayer(e, 0, st.add, t.Logf)
	p.Handle(prepare(7, 16000, 80))
	p.Handle(chunk(7, 1024))
	p.Handle(chunk(7, 512))
	p.Handle(complete(7))
	want := []string{"cancel", "prepare 16000 80", "chunk 1024", "chunk 512", "complete"}
	waitFor(t, "engine calls", func() bool { return len(e.snapshot()) == len(want) })
	if got := e.snapshot(); !equal(got, want) {
		t.Fatalf("engine calls %v, want %v", got, want)
	}
	p.EngineStatus(EngineAudioCompleted)
	waitFor(t, "status", func() bool { return len(st.snapshot()) == 2 })
	if got := st.snapshot(); !equal(got, []string{"7 STARTED", "7 COMPLETED"}) {
		t.Fatalf("status %v", got)
	}
	// a late engine report with nothing playing is not passed on
	p.EngineStatus(EngineAudioFailed)
	time.Sleep(20 * time.Millisecond)
	if got := st.snapshot(); len(got) != 2 {
		t.Fatalf("status %v", got)
	}
}

func TestAudioPrepareDefaultsAndLimits(t *testing.T) {
	e := &fakeEngine{}
	var st statusLog
	p := NewAudioPlayer(e, 0, st.add, t.Logf)
	p.Handle(prepare(1, 16000, 0))   // volume 0 means 100
	p.Handle(prepare(2, 16000, 250)) // replaces stream 1, volume capped
	p.Handle(prepare(3, 44100, 50))  // rate out of range: never reaches the engine
	waitFor(t, "status", func() bool { return len(st.snapshot()) == 4 })
	if got, want := e.snapshot(), []string{"cancel", "prepare 16000 100", "cancel", "prepare 16000 100"}; !equal(got, want) {
		t.Fatalf("engine calls %v, want %v", got, want)
	}
	if got, want := st.snapshot(), []string{"1 STARTED", "1 CANCELLED", "2 STARTED", "3 FAILED"}; !equal(got, want) {
		t.Fatalf("status %v, want %v", got, want)
	}
}

func TestAudioStepsOfOtherStreamsAreIgnored(t *testing.T) {
	e := &fakeEngine{}
	var st statusLog
	p := NewAudioPlayer(e, 0, st.add, t.Logf)
	p.Handle(chunk(5, 100)) // nothing prepared
	p.Handle(prepare(6, 16000, 100))
	p.Handle(chunk(5, 100)) // not the current stream
	p.Handle(complete(5))
	p.Handle(cancel(5))
	p.Handle(chunk(6, 1025)) // over the engine's limit: the stream fails
	p.Handle(chunk(6, 100))  // after the failure
	waitFor(t, "status", func() bool { return len(st.snapshot()) == 2 })
	time.Sleep(20 * time.Millisecond)
	if got, want := e.snapshot(), []string{"cancel", "prepare 16000 100", "cancel"}; !equal(got, want) {
		t.Fatalf("engine calls %v, want %v", got, want)
	}
	if got, want := st.snapshot(), []string{"6 STARTED", "6 FAILED"}; !equal(got, want) {
		t.Fatalf("status %v, want %v", got, want)
	}
}

func TestAudioCancelStopsTheStream(t *testing.T) {
	e := &fakeEngine{}
	var st statusLog
	p := NewAudioPlayer(e, 0, st.add, t.Logf)
	p.Handle(prepare(9, 16000, 100))
	p.Handle(chunk(9, 1024))
	p.Handle(cancel(9))
	p.Handle(chunk(9, 1024))
	waitFor(t, "status", func() bool { return len(st.snapshot()) == 2 })
	time.Sleep(20 * time.Millisecond)
	if got, want := e.snapshot(), []string{"cancel", "prepare 16000 100", "chunk 1024", "cancel"}; !equal(got, want) {
		t.Fatalf("engine calls %v, want %v", got, want)
	}
	if got, want := st.snapshot(), []string{"9 STARTED", "9 CANCELLED"}; !equal(got, want) {
		t.Fatalf("status %v, want %v", got, want)
	}
}

func TestAudioNeverBlocksTheSessionAndCancelsOnOverflow(t *testing.T) {
	e := &fakeEngine{block: make(chan struct{})}
	var st statusLog
	p := NewAudioPlayer(e, 4, st.add, t.Logf)
	start := time.Now()
	p.Handle(prepare(3, 16000, 100))
	time.Sleep(20 * time.Millisecond) // the sender is now stuck in the engine's prepare
	start = time.Now()
	for i := 0; i < 1000; i++ { // the engine is stuck: far more than the queue holds
		p.Handle(chunk(3, 1024))
	}
	if d := time.Since(start); d > 500*time.Millisecond {
		t.Fatalf("Handle blocked for %v", d)
	}
	close(e.block)
	waitFor(t, "overflow status", func() bool {
		s := st.snapshot()
		return len(s) > 0 && s[len(s)-1] == "3 OVERFLOW"
	})
	time.Sleep(20 * time.Millisecond)
	calls := e.snapshot()
	if len(calls) < 3 || calls[1] != "prepare 16000 100" || calls[len(calls)-1] != "cancel" {
		t.Fatalf("engine calls %v", calls)
	}
	if len(calls) > 3+4+1 { // cancel, prepare, at most the queued chunks, cancel
		t.Fatalf("%d engine calls after an overflow", len(calls))
	}
	// the next stream plays normally
	p.Handle(prepare(4, 16000, 100))
	waitFor(t, "next stream", func() bool {
		s := st.snapshot()
		return s[len(s)-1] == "4 STARTED"
	})
}

func TestAudioEngineOverflowAndFailureAreReported(t *testing.T) {
	e := &fakeEngine{}
	var st statusLog
	p := NewAudioPlayer(e, 0, st.add, t.Logf)
	p.Handle(prepare(1, 16000, 100))
	waitFor(t, "started", func() bool { return len(st.snapshot()) == 1 })
	p.EngineStatus(EngineAudioOverflow)
	p.Handle(chunk(1, 1024)) // the engine dropped the stream: no more chunks for it
	p.Handle(prepare(2, 16000, 100))
	waitFor(t, "second", func() bool { return len(st.snapshot()) == 3 })
	p.EngineStatus(EngineAudioFailed)
	waitFor(t, "failed", func() bool { return len(st.snapshot()) == 4 })
	if got, want := st.snapshot(), []string{"1 STARTED", "1 OVERFLOW", "2 STARTED", "2 FAILED"}; !equal(got, want) {
		t.Fatalf("status %v, want %v", got, want)
	}
	if got, want := e.snapshot(), []string{"cancel", "prepare 16000 100", "cancel", "prepare 16000 100"}; !equal(got, want) {
		t.Fatalf("engine calls %v, want %v", got, want)
	}
}

func TestAudioPrepareAlwaysCancelsFirst(t *testing.T) {
	// vic-anim may still hold a stream vic-cloud does not know about (vic-cloud restarted mid-stream): every
	// prepare is preceded by a cancel so it never fails with "Already playing audio"
	e := &fakeEngine{}
	p := NewAudioPlayer(e, 0, nil, t.Logf)
	p.Handle(prepare(1, 16000, 100))
	waitFor(t, "engine calls", func() bool { return len(e.snapshot()) == 2 })
	if got, want := e.snapshot(), []string{"cancel", "prepare 16000 100"}; !equal(got, want) {
		t.Fatalf("engine calls %v, want %v", got, want)
	}
}

func TestAudioResetCancelsTheStream(t *testing.T) {
	e := &fakeEngine{}
	var st statusLog
	p := NewAudioPlayer(e, 0, st.add, t.Logf)
	p.Reset() // a session starts: whatever vic-anim holds is cancelled
	waitFor(t, "reset cancel", func() bool { return len(e.snapshot()) == 1 })
	p.Handle(prepare(2, 16000, 100))
	p.Handle(chunk(2, 1024))
	waitFor(t, "stream", func() bool { return len(e.snapshot()) == 4 })
	p.Reset()                // the session closes mid-stream
	p.Handle(chunk(2, 1024)) // a late chunk of the old stream is ignored
	waitFor(t, "status", func() bool { return len(st.snapshot()) == 2 })
	time.Sleep(20 * time.Millisecond)
	if got, want := e.snapshot(), []string{"cancel", "cancel", "prepare 16000 100", "chunk 1024", "cancel"}; !equal(got, want) {
		t.Fatalf("engine calls %v, want %v", got, want)
	}
	if got, want := st.snapshot(), []string{"2 STARTED", "2 CANCELLED"}; !equal(got, want) {
		t.Fatalf("status %v, want %v", got, want)
	}
}

type nopStream struct{}

func (nopStream) Send([]byte) error   { return nil }
func (nopStream) CloseAndRecv() error { return nil }

func TestResetOnSessionCancelsAtStartAndClose(t *testing.T) {
	e := &fakeEngine{}
	p := NewAudioPlayer(e, 0, nil, t.Logf)
	dial := ResetOnSession(func(context.Context) (Stream, error) { return nopStream{}, nil }, p)
	s, err := dial(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	waitFor(t, "cancel at start", func() bool { return len(e.snapshot()) == 1 })
	s.CloseAndRecv()
	waitFor(t, "cancel at close", func() bool { return len(e.snapshot()) == 2 })
	if got, want := e.snapshot(), []string{"cancel", "cancel"}; !equal(got, want) {
		t.Fatalf("engine calls %v, want %v", got, want)
	}
}
