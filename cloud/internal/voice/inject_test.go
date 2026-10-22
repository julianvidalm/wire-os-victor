package voice

import (
	"testing"

	"github.com/digital-dream-labs/vector-cloud/internal/clad/cloud"
)

type recordingSender struct{ sent []*cloud.Message }

func (r *recordingSender) Send(m *cloud.Message) error { r.sent = append(r.sent, m); return nil }

func TestInjectResultReachesTheEngineWithoutAVoiceRequest(t *testing.T) {
	p := &Process{}
	engine := &recordingSender{}
	p.AddIntentWriter(engine)
	p.InjectResult(&cloud.IntentResult{Intent: "intent_clock_time", Parameters: "{}"})
	if len(engine.sent) != 1 {
		t.Fatalf("sent %d messages", len(engine.sent))
	}
	if got := engine.sent[0].GetResult(); got.Intent != "intent_clock_time" || got.Parameters != "{}" {
		t.Fatalf("result %+v", got)
	}
}
