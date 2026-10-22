package mictap

import (
	"encoding/json"
	"strings"
	"testing"
	"unicode/utf8"

	pb "github.com/digital-dream-labs/vector-cloud/internal/proto/mictap"
)

func TestIntentCommandPassesThrough(t *testing.T) {
	r, err := CommandToIntent(&pb.Command{Command: &pb.Command_IntentResult{IntentResult: &pb.IntentResult{
		Intent: "intent_clock_time", ParametersJson: `{"a":1}`, Metadata: "m"}}})
	if err != nil {
		t.Fatal(err)
	}
	if r.Intent != "intent_clock_time" || r.Parameters != `{"a":1}` || r.Metadata != "m" {
		t.Fatalf("result %+v", r)
	}
}

func knowledgeParams(t *testing.T, answer string) map[string]string {
	t.Helper()
	r, err := CommandToIntent(&pb.Command{Command: &pb.Command_Knowledge{Knowledge: &pb.KnowledgeResponse{
		Answer: answer, QueryText: "qué hora es"}}})
	if err != nil {
		t.Fatal(err)
	}
	if r.Intent != "intent_knowledge_response_extend_bypass" {
		t.Fatalf("intent %q", r.Intent)
	}
	var params map[string]string
	if err := json.Unmarshal([]byte(r.Parameters), &params); err != nil {
		t.Fatal(err)
	}
	return params
}

func TestKnowledgeCommandBecomesASpokenAnswer(t *testing.T) {
	p := knowledgeParams(t, "Son las tres y media.")
	if p["answer"] != "Son las tres y media." || p["query_text"] != "qué hora es" || p["answer_type"] != "VOICE_COMMAND" {
		t.Fatalf("params %v", p)
	}
}

func TestLongAnswersFitTheEngineBuffer(t *testing.T) {
	long := strings.Repeat("palabra ñandú ", 600) // about 8.4 KB, past the engine's 4096-byte receive buffer
	p := knowledgeParams(t, long)
	if len(p["answer"]) > maxAnswerBytes || !utf8.ValidString(p["answer"]) || strings.HasSuffix(p["answer"], " ") {
		t.Fatalf("answer %d bytes, valid UTF-8 %v", len(p["answer"]), utf8.ValidString(p["answer"]))
	}
	if !strings.HasPrefix(long, p["answer"]) {
		t.Fatal("the shortened answer is not a prefix of the original")
	}
}

func TestEmptyCommandIsRejected(t *testing.T) {
	if _, err := CommandToIntent(&pb.Command{}); err == nil {
		t.Fatal("accepted a command without content")
	}
	if _, err := CommandToIntent(&pb.Command{Command: &pb.Command_IntentResult{IntentResult: &pb.IntentResult{}}}); err == nil {
		t.Fatal("accepted an intent result without an intent")
	}
}

func TestKnowledgeWithoutExternalVoiceKeepsTheRobotVoice(t *testing.T) {
	p := knowledgeParams(t, "Hola.")
	if _, ok := p["external_voice"]; ok {
		t.Fatalf("params %v carry external_voice", p)
	}
	if _, ok := p["expected_duration_ms"]; ok {
		t.Fatalf("params %v carry expected_duration_ms", p)
	}
}

func TestKnowledgeWithExternalVoiceTellsTheEngine(t *testing.T) {
	r, err := CommandToIntent(&pb.Command{Command: &pb.Command_Knowledge{Knowledge: &pb.KnowledgeResponse{
		Answer: "Son las tres.", QueryText: "qué hora es", ExternalVoice: true, ExpectedDurationMs: 2300}}})
	if err != nil {
		t.Fatal(err)
	}
	var p struct {
		Answer             string `json:"answer"`
		ExternalVoice      bool   `json:"external_voice"`
		ExpectedDurationMs uint32 `json:"expected_duration_ms"`
	}
	if err := json.Unmarshal([]byte(r.Parameters), &p); err != nil {
		t.Fatal(err)
	}
	if r.Intent != "intent_knowledge_response_extend_bypass" || p.Answer != "Son las tres." || !p.ExternalVoice ||
		p.ExpectedDurationMs != 2300 {
		t.Fatalf("intent %q params %s", r.Intent, r.Parameters)
	}
}

func TestAudioCommandIsNotAnIntent(t *testing.T) {
	if _, err := CommandToIntent(&pb.Command{Command: &pb.Command_Audio{Audio: &pb.Audio{StreamId: 1}}}); err == nil {
		t.Fatal("audio became an intent")
	}
}
