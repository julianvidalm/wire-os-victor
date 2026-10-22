package mictap

import (
	"encoding/json"
	"errors"
	"strings"
	"unicode/utf8"

	"github.com/digital-dream-labs/vector-cloud/internal/clad/cloud"
	pb "github.com/digital-dream-labs/vector-cloud/internal/proto/mictap"
)

// knowledgeBypassIntent makes the engine speak an answer with the knowledge-graph animations without opening
// a voice stream (BehaviorKnowledgeGraphQuestion), the way intent-graph answers from chipper are delivered.
const knowledgeBypassIntent = "intent_knowledge_response_extend_bypass"

// maxAnswerBytes keeps the packed message under the engine's 4096-byte receive buffer for cloud intents
// (BehaviorComponentCloudServer), leaving room for the JSON and CLAD framing.
const maxAnswerBytes = 3000

// CommandToIntent turns a command from the cortex into the intent result vic-cloud hands to the engine, as if
// it answered the robot's own voice request.
func CommandToIntent(cmd *pb.Command) (*cloud.IntentResult, error) {
	switch c := cmd.GetCommand().(type) {
	case *pb.Command_IntentResult:
		if c.IntentResult.GetIntent() == "" {
			return nil, errors.New("intent result without an intent")
		}
		return &cloud.IntentResult{
			Intent:     c.IntentResult.GetIntent(),
			Parameters: c.IntentResult.GetParametersJson(),
			Metadata:   c.IntentResult.GetMetadata(),
		}, nil
	case *pb.Command_Knowledge:
		fields := map[string]any{
			"answer":      shorten(c.Knowledge.GetAnswer(), maxAnswerBytes),
			"answer_type": "VOICE_COMMAND",
			"query_text":  c.Knowledge.GetQueryText(),
		}
		if c.Knowledge.GetExternalVoice() {
			// the cortex streams the spoken answer (Command.audio); the engine only shows the talking face
			fields["external_voice"] = true
			fields["expected_duration_ms"] = c.Knowledge.GetExpectedDurationMs()
		}
		params, err := json.Marshal(fields)
		if err != nil {
			return nil, err
		}
		return &cloud.IntentResult{Intent: knowledgeBypassIntent, Parameters: string(params)}, nil
	case *pb.Command_Audio:
		return nil, errors.New("audio is not an intent")
	default:
		return nil, errors.New("empty command")
	}
}

// shorten cuts s to at most n bytes at a word boundary, never inside a UTF-8 character.
func shorten(s string, n int) string {
	if len(s) <= n {
		return s
	}
	cut := n
	for cut > 0 && !utf8.RuneStart(s[cut]) {
		cut--
	}
	s = s[:cut]
	if i := strings.LastIndexByte(s, ' '); i > 0 {
		s = s[:i]
	}
	return strings.TrimRight(s, " ")
}
