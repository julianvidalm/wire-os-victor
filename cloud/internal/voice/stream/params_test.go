package stream

import (
	"encoding/json"
	"testing"

	"github.com/digital-dream-labs/api-clients/chipper"
	"github.com/digital-dream-labs/vector-cloud/internal/clad/cloud"
)

type intentReceiver struct{ got *cloud.IntentResult }

func (r *intentReceiver) OnError(cloud.ErrorType, error)             {}
func (r *intentReceiver) OnStreamOpen(string)                        {}
func (r *intentReceiver) OnIntent(i *cloud.IntentResult)             { r.got = i }
func (r *intentReceiver) OnConnectionResult(*cloud.ConnectionResult) {}

func decodeParams(t *testing.T, r *intentReceiver) map[string]any {
	t.Helper()
	if r.got == nil {
		t.Fatal("no intent")
	}
	var m map[string]any
	if err := json.Unmarshal([]byte(r.got.Parameters), &m); err != nil {
		t.Fatal(err)
	}
	return m
}

func TestKnowledgeBypassIntentCarriesTypedExternalVoice(t *testing.T) {
	var r intentReceiver
	sendIntentResponse(&chipper.IntentResult{Action: "intent_knowledge_response_extend_bypass", Parameters: map[string]string{
		"answer": "Ottawa", "answer_type": "VOICE_COMMAND", "query_text": "capital of Canada",
		"external_voice": "true", "expected_duration_ms": "1830",
	}}, &r)
	m := decodeParams(t, &r)
	if m["external_voice"] != true || m["expected_duration_ms"] != float64(1830) || m["answer"] != "Ottawa" {
		t.Fatalf("params %#v", m)
	}
}

func TestOtherIntentsKeepStringParameters(t *testing.T) {
	var r intentReceiver
	sendIntentResponse(&chipper.IntentResult{Action: "intent_clock_settimer", Parameters: map[string]string{
		"timer_duration": "300", "external_voice": "true",
	}}, &r)
	m := decodeParams(t, &r)
	if m["timer_duration"] != "300" || m["external_voice"] != "true" {
		t.Fatalf("params %#v", m)
	}
}
