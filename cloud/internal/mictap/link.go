package mictap

import (
	"context"
	"errors"
	"time"
)

// healthySession is how long a Session must stay open to reset the redial backoff.
const healthySession = 10 * time.Second

// LinkConfig configures RunLink.
type LinkConfig struct {
	// Dial opens a Session (NewSessionDialer or NewSessionDialerWithStatus, optionally behind ResetOnSession).
	Dial Dialer
	// MinBackoff and MaxBackoff bound the wait before redialing after a call ends or a dial fails.
	MinBackoff, MaxBackoff time.Duration
	Logf                   func(format string, a ...any)
}

// waiter is a Stream that can wait for the cortex to end the call.
type waiter interface {
	Wait() error
}

var errNoSession = errors.New("the cortex has no Session rpc")

// RunLink keeps one command Session to the cortex open until ctx is done, sending no audio: the robot hears its
// own wake word and streams the voice request to chipper, so the Session only carries the cortex's commands down
// and the audio status reports up. It redials whenever the call ends, waiting MinBackoff after a session that
// stayed open at least healthySession and twice as long (up to MaxBackoff) after each shorter one.
func RunLink(ctx context.Context, cfg LinkConfig) {
	if cfg.MinBackoff <= 0 {
		cfg.MinBackoff = time.Second
	}
	if cfg.MaxBackoff < cfg.MinBackoff {
		cfg.MaxBackoff = 30 * time.Second
	}
	if cfg.Logf == nil {
		cfg.Logf = func(string, ...any) {}
	}
	backoff := cfg.MinBackoff
	for ctx.Err() == nil {
		opened := time.Now()
		err := runSession(ctx, cfg.Dial)
		if errors.Is(err, errNoSession) {
			cfg.Logf("mictap: %v; no cortex commands", err)
			return
		}
		if ctx.Err() != nil {
			return
		}
		if time.Since(opened) >= healthySession {
			backoff = cfg.MinBackoff
		}
		cfg.Logf("mictap: link ended, redialing in %v: %v", backoff, err)
		select {
		case <-ctx.Done():
			return
		case <-time.After(backoff):
		}
		backoff = min(2*backoff, cfg.MaxBackoff)
	}
}

// runSession opens one Session and returns when the call ends.
func runSession(ctx context.Context, dial Dialer) error {
	s, err := dial(ctx)
	if err != nil {
		return err
	}
	w, ok := s.(waiter)
	if !ok {
		s.CloseAndRecv()
		return errNoSession
	}
	err = w.Wait()
	s.CloseAndRecv()
	return err
}
