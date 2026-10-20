/**
 * File: behaviorCortexListening.h
 *
 * Created: 2026-10-19
 *
 * Description: "I'm listening" reaction for the PC cortex. Responds to the cortex_listening user intent (sent
 * by the cortex when it hears the wake word) with the stock "hey vector" gesture: the wake word earcon and
 * get-in (head up), then the stock turn toward the direction the voice came from (robot mic direction
 * history over the wake phrase: the window ends at the robot time where the cortex detected the wake word,
 * cortex_listening's wake_ts_ms, and counts only samples with voice activity; without that time, the window
 * before the intent arrived with every sample, as before. No turn on the charger, when picked up, or when the
 * voice came from the front or with too little confidence), then the head up at the get-in's angle with a silent eye pose held
 * while the cortex captures the user's speech. The eyes take a listening color (not saved as the eye color
 * setting) until the behavior ends by any path. The cortex_listening_prompt user intent (the cortex heard no
 * speech for a while) keeps listening while one native clip plays: the eyes raise their brows ("huh?") while a
 * white question mark draws itself above them, then fades, and the eyes settle back. The
 * cortex_thinking user intent (the user's speech ended and the cortex is preparing the answer) brings the
 * user's eye color back and shows a "looking it up" face, held until the answer, cortex_listening_end (the
 * give-up gesture: nothing to answer) or a longer thinking timeout. It ends, without any post-behavior
 * suggestion, when:
 *   - any other user intent becomes pending (e.g. the cortex's answer). That intent is left pending so the
 *     behavior that handles it can claim it right after.
 *   - the cortex_listening_end user intent arrives: it plays a short "never mind" gesture, then ends.
 *   - a safety timeout expires.
 * A stray cortex_listening_end, cortex_listening_prompt or cortex_thinking (nothing listening, e.g. after this
 * behavior was interrupted) is consumed silently, so it never counts as an unclaimed intent.
 *
 **/

#ifndef __Engine_AiComponent_BehaviorComponent_Behaviors_BehaviorCortexListening__
#define __Engine_AiComponent_BehaviorComponent_Behaviors_BehaviorCortexListening__
#pragma once

#include "clad/audio/audioEventTypes.h"
#include "clad/types/animationTrigger.h"
#include "engine/aiComponent/behaviorComponent/behaviors/iCozmoBehavior.h"
#include "engine/components/mics/micDirectionTypes.h"

#include <string>
#include <vector>

namespace Anki {
namespace Vector {

class BehaviorReactToMicDirection;
class MicDirectionHistory;

class BehaviorCortexListening : public ICozmoBehavior
{
public:
  virtual ~BehaviorCortexListening();

  // Why the behavior last ended (exposed for tests and logs)
  enum class EndReason {
    None,
    NextIntent,    // another user intent became pending
    ListeningEnd,  // cortex_listening_end arrived while listening (after the give-up gesture)
    StrayEnd,      // cortex_listening_end arrived while not listening
    StrayPrompt,   // cortex_listening_prompt arrived while not listening
    StrayThinking, // cortex_thinking arrived while not listening
    Timeout,       // the safety timeout (listening or thinking) expired
  };

  EndReason GetLastEndReason() const { return _lastEndReason; }

  // Exposed for tests
  bool IsShowingListeningEyeColor() const { return _showingListeningEyeColor; }
  bool IsGivingUp() const { return _dVars.givingUp; }
  bool IsThinking() const { return _dVars.thinking; }
  int GetNumPromptsShown() const { return _dVars.numPrompts; }
  bool IsShowingPrompt() const { return _dVars.promptShowing; }
  const std::string& GetPromptAnimName() const { return _iConfig.promptAnimName; }

  // Direction the voice came from, picked when the behavior last activated (kMicDirectionUnknown if none)
  MicDirectionIndex GetLastVoiceDirection() const { return _dVars.voiceDirection; }
  // Whether that direction was voted over the wake phrase (the cortex sent the wake time) rather than the
  // fallback window before the intent
  bool WasLastDirectionFromWakeTime() const { return _dVars.directionFromWakeTime; }

  struct VoiceDirectionVote {
    MicDirectionIndex direction = kMicDirectionUnknown;
    // average SignalEssence confidence of the winning direction's counted samples
    MicDirectionConfidence confidence = 0;
    // samples counted for the winning direction (10 ms each)
    uint32_t samples = 0;
  };

  // Most common direction heard in the window [end_ms - window_ms, end_ms], counting only samples whose
  // average confidence is above minConfidence and, with voiceOnly, only samples with voice activity (VAD).
  // Without voiceOnly it is the stock trigger word reaction's vote
  // (BehaviorReactToVoiceCommand::ComputeReactionDirectionFromStream)
  static VoiceDirectionVote VoteVoiceDirection(const MicDirectionHistory& history,
                                               EngineTimeStamp_t end_ms,
                                               TimeStamp_t window_ms,
                                               MicDirectionConfidence minConfidence,
                                               bool voiceOnly);

  // Most common direction heard in the window [end_ms - window_ms, end_ms], counting only samples whose
  // average confidence is above minConfidence (same vote as the stock trigger word reaction,
  // BehaviorReactToVoiceCommand::ComputeReactionDirectionFromStream). kMicDirectionUnknown if nothing passes.
  static MicDirectionIndex PickVoiceDirection(const MicDirectionHistory& history,
                                              EngineTimeStamp_t end_ms,
                                              TimeStamp_t window_ms,
                                              MicDirectionConfidence minConfidence);

protected:

  // Enforce creation through BehaviorFactory
  friend class BehaviorFactory;
  explicit BehaviorCortexListening(const Json::Value& config);

  virtual void GetBehaviorOperationModifiers(BehaviorOperationModifiers& modifiers) const override;
  virtual void GetBehaviorJsonKeys(std::set<const char*>& expectedKeys) const override;
  virtual void GetAllDelegates(std::set<IBehavior*>& delegates) const override;
  virtual void InitBehavior() override;

  virtual bool WantsToBeActivatedBehavior() const override { return true; }
  virtual void OnBehaviorActivated() override;
  virtual void OnBehaviorDeactivated() override;
  virtual void BehaviorUpdate() override;

private:

  struct InstanceConfig {
    std::string animName;
    float timeout_s;
    AnimationTrigger getInAnim;
    AudioMetaData::GameEvent::GenericEvent earCon;
    std::string turnBehaviorID;
    std::shared_ptr<BehaviorReactToMicDirection> turnBehavior;
    float headAngle_deg;
    float directionWindow_s;
    float wakeDirectionWindow_s;
    bool voiceOnly;
    MicDirectionConfidence minDirectionConfidence;
    float eyeHue;
    float eyeSaturation;
    std::string promptAnimName;
    std::string giveUpAnimName;
    std::string thinkingGetInAnimName;
    std::string thinkingLoopAnimName;
    float thinkingTimeout_s;
  };

  struct DynamicVariables {
    float listeningStart_s = 0.0f;
    MicDirectionIndex voiceDirection = kMicDirectionUnknown;
    bool directionFromWakeTime = false;
    bool givingUp = false;
    float giveUpStart_s = 0.0f;
    bool promptShowing = false;
    int numPrompts = 0;
    bool thinking = false;
  };

  void PickDirectionTowardVoice();
  void TransitionToTurnTowardVoice();
  void TransitionToHoldListening();
  void ShowPrompt();
  void TransitionToGiveUp();
  void TransitionToThinking();
  void TransitionToThinkingLoop();
  void RestoreEyeColor();
  void EndListening(EndReason reason);

  InstanceConfig _iConfig;
  DynamicVariables _dVars;
  EndReason _lastEndReason = EndReason::None;
  bool _showingListeningEyeColor = false;
};

} // namespace Vector
} // namespace Anki

#endif // __Engine_AiComponent_BehaviorComponent_Behaviors_BehaviorCortexListening__
