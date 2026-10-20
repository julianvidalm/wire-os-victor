/**
* File: behaviorReactToVoiceCommand.h
*
* Author: Jarrod Hatfield
* Created: 2/16/2018
*
* Description: Simple behavior to immediately respond to the voice command keyphrase, while waiting for further commands.
*
* Copyright: Anki, Inc. 2017
*
**/

#ifndef __Cozmo_Basestation_Behaviors_BehaviorReactToVoiceCommand_H__
#define __Cozmo_Basestation_Behaviors_BehaviorReactToVoiceCommand_H__

#include "clad/audio/audioEventTypes.h"
#include "coretech/common/engine/utils/recentOccurrenceTracker.h"
#include "engine/aiComponent/behaviorComponent/behaviors/iCozmoBehavior.h"
#include "engine/components/backpackLights/engineBackpackLightComponentTypes.h"
#include "engine/components/mics/micDirectionTypes.h"
#include "engine/engineTimeStamp.h"

namespace Anki {
namespace Vector {

class BehaviorReactToMicDirection;
class ConditionUserIntentPending;
enum class AnimationTrigger : int32_t;

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
class BehaviorReactToVoiceCommand : public ICozmoBehavior
{
  friend class BehaviorFactory;
  BehaviorReactToVoiceCommand(const Json::Value& config);


public:

  // Public destructor must be explicitly defined to allow std::unique_ptr<FwdDeclaredType> (ConditionUserIntentPending)
  ~BehaviorReactToVoiceCommand();

  // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

  virtual bool WantsToBeActivatedBehavior() const override;
  virtual void GetBehaviorOperationModifiers( BehaviorOperationModifiers& modifiers ) const override;
  virtual void GetBehaviorJsonKeys( std::set<const char*>& expectedKeys ) const override;
  
  // Empty override of AddListener because the strategy that controls this behavior is a listener
  // The strategy controls multiple different behaviors and listeners are necessary for the other behaviors
  // since they are generic PlayAnim behaviors (reactToVoiceCommand_Wakeup)
  virtual void AddListener(ISubtaskListener* listener) override {};

  // Allow other behaviors to specify a timestamp (generally the current timestamp)
  // on which the turn on trigger/intent is disabled
  void DisableTurnForTimestamp(EngineTimeStamp_t timestampToDisableFor){
    _dVars.timestampToDisableTurnFor = timestampToDisableFor;
  }
  
  void SetListeningAnims(const AnimationTrigger& listeningLoop,
                         const AnimationTrigger& listeningGetOut);
  
  void ResetListeningAnimsToConfig();

protected:

  // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
  enum class EState : uint8_t
  {
    GetIn,
    ListeningGetIn,
    ListeningLoop,
    Thinking,
    IntentReceived,
  };

  enum class EIntentStatus : uint8_t
  {
    IntentHeard,
    IntentUnknown,
    SilenceTimeout,
    NoIntentHeard,
    Error
  };


  // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
  
  virtual void InitBehavior() override;
  virtual void GetAllDelegates( std::set<IBehavior*>& delegates ) const override;

  virtual void AlwaysHandleInScope( const RobotToEngineEvent& event ) override;

  virtual void OnBehaviorEnteredActivatableScope() override;
  virtual void OnBehaviorActivated() override;
  virtual void OnBehaviorDeactivated() override;
  virtual void OnBehaviorLeftActivatableScope() override;

  virtual void BehaviorUpdate() override;

  
  void UpdateUserIntentStatus();

  // state / transition functions
  void StartListening();
  void StopListening();

  void TransitionToThinking();
  void TransitionToIntentReceived();

  // coincide with the beginning of the stream being opened
  void OnStreamingBegin();
  void OnStreamingEnd();

  // this is the state when victor is "listening" for the users intent
  // and should therefore cue the user to speak
  void OnVictorListeningBegin();
  void OnVictorListeningEnd();

  void HandleStreamFailure();

  // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
  // Listening feedback (the "listeningFeedback" config; off without it, which is the stock behavior)

  bool IsListeningFeedbackEnabled() const { return _iVars.feedback.enabled; }
  // the direction of the wake phrase, picked at activation before anything moves
  void PickWakeDirection();
  // after the get-in: turn toward the wake phrase, then hold the listening pose
  void StartFeedbackLook();
  void TransitionToFeedbackHold();
  void OnFeedbackLookEnd();
  // no intent pending: speech detection, the '?' prompt, the give-up and the thinking face
  void UpdateListeningFeedback();
  // while giving up or after it: the late result of the stream. True when it handled this tick
  bool UpdateFeedbackGiveUp();
  void ShowFeedbackPrompt();
  void TransitionToFeedbackGiveUp();
  void TransitionToFeedbackThinking();
  void TransitionToFeedbackThinkingLoop();
  void RestoreListeningEyeColor();

  // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
  // Direction Helpers

  // cache the direction we want to react to
  void ComputeReactionDirectionFromStream();

  // get the direction we want to react to
  MicDirectionIndex GetReactionDirection() const;
  // get the "best recent" direction from the mic history
  MicDirectionIndex GetDirectionFromMicHistory() const;
  
  // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
  // Time Helpers

  double GetStreamingDuration() const;
  double GetListeningTimeout() const;


private:

  // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
  struct InstanceConfig
  {
    InstanceConfig();

    // earcon is an audible cue to tell the user victor is listening
    AudioMetaData::GameEvent::GenericEvent earConBegin;
    AudioMetaData::GameEvent::GenericEvent earConSuccess;
    AudioMetaData::GameEvent::GenericEvent earConFail;
    
    bool pushResponse;
    
    AnimationTrigger animListeningGetIn;
    AnimationTrigger animListeningLoop;
    AnimationTrigger animListeningGetOut;

    bool backpackLights;
    
    bool exitAfterGetIn;
    
    // If we are not streaming audio to the cloud, then this causes the behavior to exit after playing the "unheard"
    // animation. This is to prevent the accumulation of "errors" due to unreceived intents (we would not expect any
    // intents to come down if we are not streaming)
    bool exitAfterListeningIfNotStreaming;

    // response behavior to hearing the trigger word (or intent)
    std::string reactionBehaviorString;
    std::shared_ptr<BehaviorReactToMicDirection> reactionBehavior;

    // behaviors to handle specific failure cases
    ICozmoBehaviorPtr unmatchedIntentBehavior;
    ICozmoBehaviorPtr silenceIntentBehavior;
    ICozmoBehaviorPtr noCloudBehavior;
    ICozmoBehaviorPtr noWifiBehavior;

    // tracking for when to trigger failure behaviors
    float _errorTrackingWindow_s = 0.0f;
    int _numErrorsToTriggerAnim = 0;
    RecentOccurrenceTracker cloudErrorTracker;
    RecentOccurrenceTracker wifiErrorTracker;
    // when this handle's conditions are met, we animate to show the user there was a failure (and possibly
    // trigger an attention transfer)
    RecentOccurrenceTracker::Handle wifiErrorHandle;
    RecentOccurrenceTracker::Handle cloudErrorHandle;

    std::unique_ptr<ConditionUserIntentPending> intentWhitelistCondition;

    // the listening feedback after the wake (listening eye color, turn toward the voice at the wake, the '?'
    // prompt and the give-up when the robot hears no speech, the thinking face until the answer)
    struct ListeningFeedback
    {
      bool enabled = false;
      float eyeHue = 0.0f;
      float eyeSaturation = 0.0f;
      bool turnAtWake = false;
      float wakeDirectionWindow_s = 0.0f;
      MicDirectionConfidence minDirectionConfidence = 0;
      float headAngle_deg = 0.0f;
      std::string holdAnimName;
      std::string promptAnimName;
      float promptAfter_s = 0.0f;
      std::string giveUpAnimName;
      float giveUpAfter_s = 0.0f;
      float giveUpWait_s = 0.0f;
      float minSpeech_s = 0.0f;
      float speechWindow_s = 0.0f;
      float settleAfterLook_s = 0.0f;
      std::string thinkingGetInAnimName;
      std::string thinkingLoopAnimName;
      float thinkingDelay_s = 0.0f;
    } feedback;

  } _iVars;


  // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

  struct DynamicVariables
  {
    DynamicVariables();

    EState                    state;
    MicDirectionIndex         reactionDirection;
    BackpackLightDataLocator  lightsHandle;

    double                    streamingBeginTime;
    double                    streamingEndTime;

    EIntentStatus             intentStatus;
    EngineTimeStamp_t         timestampToDisableTurnFor;

    bool                      expectingStream;

    enum class EFeedbackPhase : uint8_t
    {
      Off,       // no listening feedback (not configured, or before the get-in ends)
      Turning,   // turning toward the wake phrase
      Holding,   // head up to the listening pose
      Listening, // the look gesture ended: waiting for speech, the '?' prompt and give-up timers run
      GivingUp,  // the give-up gesture is playing
      GaveUp,    // after the give-up gesture: waiting briefly for the stream's late result
      Thinking,  // the stream ended: thinking face until the intent arrives
    };

    struct Feedback
    {
      EFeedbackPhase    phase = EFeedbackPhase::Off;
      MicDirectionIndex wakeDirection = kMicDirectionUnknown;
      double            lookEnd_s = 0.0;
      EngineTimeStamp_t lookEnd_ms = 0;
      bool              speechHeard = false;
      bool              promptShown = false;
      bool              resultAfterGiveUp = false;
      double            giveUpEnd_s = 0.0;
      double            streamEndSeen_s = 0.0;
      double            lastVadSample_s = 0.0;
    } feedback;
    
    struct Persistent {

      Persistent();

      AnimationTrigger          forcedAnimListeningLoop;
      AnimationTrigger          forcedAnimListeningGetOut;
      
      bool                      listeningAnimsResetQueued;
    };
    Persistent persistent;
  } _dVars;

  // these are dynamic vars that live beyond the activation scope ...
  MicDirectionIndex         _triggerDirection;

  // the listening eye color is showing (restored on every exit)
  bool                      _showingListeningEyeColor = false;

  bool IsTurnEnabled() const;

}; // class BehaviorReactToVoiceCommand

} // namespace Vector
} // namespace Anki

#endif // __Cozmo_Basestation_Behaviors_BehaviorReactToVoiceCommand_H__
