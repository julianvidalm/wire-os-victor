/**
 * File: behaviorCortexListening.cpp
 *
 * Created: 2026-10-19
 *
 * Description: "I'm listening" reaction for the PC cortex: the stock wake word gesture toward the voice, then
 * a silent held eye pose. See the header for when it ends.
 *
 **/

#include "engine/aiComponent/behaviorComponent/behaviors/reactions/behaviorCortexListening.h"

#include "clad/types/behaviorComponent/behaviorClasses.h"
#include "clad/types/behaviorComponent/userIntent.h"
#include "coretech/common/engine/jsonTools.h"
#include "coretech/common/engine/utils/timer.h"
#include "engine/actions/animActions.h"
#include "engine/actions/basicActions.h"
#include "engine/actions/compoundActions.h"
#include "engine/aiComponent/behaviorComponent/behaviorExternalInterface/beiRobotInfo.h"
#include "engine/aiComponent/behaviorComponent/behaviors/reactions/behaviorReactToMicDirection.h"
#include "engine/aiComponent/behaviorComponent/userIntentComponent.h"
#include "engine/audio/engineRobotAudioClient.h"
#include "engine/components/mics/micComponent.h"
#include "engine/components/mics/micDirectionHistory.h"

#include <algorithm>
#include <array>

#define LOG_CHANNEL "Behaviors"

namespace Anki {
namespace Vector {

namespace {
const char* kAnimNameKey = "animName";
const char* kTimeoutKey  = "timeout_s";
const char* kGetInAnimKey = "getInAnimTrigger";
const char* kEarConKey = "earConAudioEvent";
const char* kTurnBehaviorKey = "turnBehavior";
const char* kHeadAngleKey = "headAngle_deg";
const char* kDirectionWindowKey = "directionWindow_s";
const char* kMinDirectionConfidenceKey = "minDirectionConfidence";

const char* kDefaultAnimName = "anim_eyepose_curious";
constexpr float kDefaultTimeout_s = 10.0f;
// the stock "hey vector" response (triggerWordDetected.json, BehaviorReactToVoiceCommand)
constexpr AnimationTrigger kDefaultGetInAnim = AnimationTrigger::VC_ListeningGetIn;
const char* kDefaultEarCon = "Play__Robot_Vic_Sfx__Wake_Word_On";
const char* kDefaultTurnBehavior = "ReactToTriggerDirectionAwake";
// anim_wakeword_getin_01 ends with the head at 33 degrees; the stock turns end lower (12-19)
constexpr float kDefaultHeadAngle_deg = 33.0f;
// the cortex's wake intent arrives ~0.5s after the wake phrase ends, so this covers the whole phrase
constexpr float kDefaultDirectionWindow_s = 2.0f;
// same threshold as BehaviorReactToVoiceCommand's kDirStreamingConfToIgnore
constexpr MicDirectionConfidence kDefaultMinDirectionConfidence = 500;

const char* EndReasonToString(BehaviorCortexListening::EndReason reason)
{
  switch( reason ) {
    case BehaviorCortexListening::EndReason::None:         return "None";
    case BehaviorCortexListening::EndReason::NextIntent:   return "NextIntent";
    case BehaviorCortexListening::EndReason::ListeningEnd: return "ListeningEnd";
    case BehaviorCortexListening::EndReason::StrayEnd:     return "StrayEnd";
    case BehaviorCortexListening::EndReason::Timeout:      return "Timeout";
  }
  return "Unknown";
}

float GetCurrentTime_s()
{
  return BaseStationTimer::getInstance()->GetCurrentTimeInSeconds();
}
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
BehaviorCortexListening::BehaviorCortexListening(const Json::Value& config)
 : ICozmoBehavior(config)
{
  _iConfig.animName = kDefaultAnimName;
  JsonTools::GetValueOptional(config, kAnimNameKey, _iConfig.animName);

  _iConfig.timeout_s = kDefaultTimeout_s;
  JsonTools::GetValueOptional(config, kTimeoutKey, _iConfig.timeout_s);

  // an empty string disables the get-in, the earcon or the turn
  _iConfig.getInAnim = kDefaultGetInAnim;
  std::string getInAnim;
  if( JsonTools::GetValueOptional(config, kGetInAnimKey, getInAnim) ) {
    _iConfig.getInAnim = AnimationTrigger::Count;
    if( !getInAnim.empty() ) {
      ANKI_VERIFY( AnimationTriggerFromString(getInAnim, _iConfig.getInAnim),
                   "BehaviorCortexListening.Ctor.InvalidGetIn", "%s is not a valid animation trigger",
                   getInAnim.c_str() );
    }
  }

  std::string earCon = kDefaultEarCon;
  JsonTools::GetValueOptional(config, kEarConKey, earCon);
  _iConfig.earCon = earCon.empty() ? AudioMetaData::GameEvent::GenericEvent::Invalid
                                   : AudioMetaData::GameEvent::GenericEventFromString(earCon);

  _iConfig.turnBehaviorID = kDefaultTurnBehavior;
  JsonTools::GetValueOptional(config, kTurnBehaviorKey, _iConfig.turnBehaviorID);

  _iConfig.headAngle_deg = kDefaultHeadAngle_deg;
  JsonTools::GetValueOptional(config, kHeadAngleKey, _iConfig.headAngle_deg);

  _iConfig.directionWindow_s = kDefaultDirectionWindow_s;
  JsonTools::GetValueOptional(config, kDirectionWindowKey, _iConfig.directionWindow_s);

  int minDirectionConfidence = kDefaultMinDirectionConfidence;
  JsonTools::GetValueOptional(config, kMinDirectionConfidenceKey, minDirectionConfidence);
  _iConfig.minDirectionConfidence = static_cast<MicDirectionConfidence>(minDirectionConfidence);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
BehaviorCortexListening::~BehaviorCortexListening()
{
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BehaviorCortexListening::GetBehaviorJsonKeys(std::set<const char*>& expectedKeys) const
{
  expectedKeys.insert(kAnimNameKey);
  expectedKeys.insert(kTimeoutKey);
  expectedKeys.insert(kGetInAnimKey);
  expectedKeys.insert(kEarConKey);
  expectedKeys.insert(kTurnBehaviorKey);
  expectedKeys.insert(kHeadAngleKey);
  expectedKeys.insert(kDirectionWindowKey);
  expectedKeys.insert(kMinDirectionConfidenceKey);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BehaviorCortexListening::InitBehavior()
{
  if( _iConfig.turnBehaviorID.empty() ) {
    return;
  }

  ICozmoBehaviorPtr turnBehavior = FindBehavior(_iConfig.turnBehaviorID);
  if( ANKI_VERIFY( (turnBehavior != nullptr) && (turnBehavior->GetClass() == BehaviorClass::ReactToMicDirection),
                   "BehaviorCortexListening.InitBehavior.InvalidTurnBehavior",
                   "%s must be a ReactToMicDirection behavior", _iConfig.turnBehaviorID.c_str() ) ) {
    _iConfig.turnBehavior = std::static_pointer_cast<BehaviorReactToMicDirection>(turnBehavior);
  }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BehaviorCortexListening::GetAllDelegates(std::set<IBehavior*>& delegates) const
{
  if( _iConfig.turnBehavior != nullptr ) {
    delegates.insert(_iConfig.turnBehavior.get());
  }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
MicDirectionIndex BehaviorCortexListening::PickVoiceDirection(const MicDirectionHistory& history,
                                                              EngineTimeStamp_t end_ms,
                                                              TimeStamp_t window_ms,
                                                              MicDirectionConfidence minConfidence)
{
  if( window_ms == 0 ) {
    return kMicDirectionUnknown;
  }

  const EngineTimeStamp_t begin_ms = ( (TimeStamp_t)end_ms > window_ms ) ? EngineTimeStamp_t((TimeStamp_t)end_ms - window_ms)
                                                                          : EngineTimeStamp_t(0);
  MicDirectionNodeList list = history.GetHistoryAtTime(end_ms, window_ms);

  // only count the part of the first and last nodes that falls inside the window
  if( list.size() > 1 ) {
    MicDirectionNode& front = list.front();
    if( (front.timestampBegin < begin_ms) && (front.timestampEnd > front.timestampBegin) ) {
      const double nodeDuration = TimeStamp_t(front.timestampEnd - front.timestampBegin);
      const double timeInNode = (front.timestampEnd > begin_ms) ? TimeStamp_t(front.timestampEnd - begin_ms) : 0;
      front.count *= ( timeInNode / nodeDuration );
    }

    MicDirectionNode& back = list.back();
    if( (back.timestampEnd > end_ms) && (back.timestampEnd > back.timestampBegin) ) {
      const double nodeDuration = TimeStamp_t(back.timestampEnd - back.timestampBegin);
      const double timeInNode = (end_ms > back.timestampBegin) ? TimeStamp_t(end_ms - back.timestampBegin) : 0;
      back.count *= ( timeInNode / nodeDuration );
    }
  }

  std::array<uint32_t, kNumMicDirections + 1> counts{};
  for( const auto& node : list ) {
    if( (node.directionIndex <= kMicDirectionUnknown) && (node.confidenceAvg > minConfidence) ) {
      counts[node.directionIndex] += node.count;
    }
  }

  MicDirectionIndex best = kMicDirectionUnknown;
  uint32_t bestCount = 0;
  for( MicDirectionIndex i = kFirstMicDirectionIndex; i < kNumMicDirections; ++i ) {
    if( counts[i] > bestCount ) {
      best = i;
      bestCount = counts[i];
    }
  }
  return best;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BehaviorCortexListening::GetBehaviorOperationModifiers(BehaviorOperationModifiers& modifiers) const
{
  // the eye pose plays once and then the behavior holds it without delegating
  modifiers.behaviorAlwaysDelegates = false;
  modifiers.wantsToBeActivatedWhenCarryingObject = true;
  modifiers.wantsToBeActivatedWhenOffTreads = true;
  modifiers.wantsToBeActivatedWhenOnCharger = true;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BehaviorCortexListening::OnBehaviorActivated()
{
  _dVars = DynamicVariables();
  _lastEndReason = EndReason::None;

  // respondToUserIntents has already activated the intent that woke this behavior up
  const auto& uic = GetBehaviorComp<UserIntentComponent>();
  if( uic.IsUserIntentActive(USER_INTENT(cortex_listening_end)) ) {
    // the cortex ended a capture we are not showing (e.g. a higher priority reaction interrupted us). Claim
    // it so it never counts as unclaimed, and end silently
    EndListening(EndReason::StrayEnd);
    return;
  }

  _dVars.listeningStart_s = GetCurrentTime_s();

  // pick the direction before anything moves: the history is relative to the robot's current heading
  const MicDirectionHistory& micHistory = GetBEI().GetMicComponent().GetMicDirectionHistory();
  const TimeStamp_t window_ms = static_cast<TimeStamp_t>(std::max(0.0f, _iConfig.directionWindow_s) * 1000.0f);
  _dVars.voiceDirection = PickVoiceDirection(micHistory,
                                             BaseStationTimer::getInstance()->GetCurrentTimeStamp(),
                                             window_ms,
                                             _iConfig.minDirectionConfidence);
  LOG_INFO("BehaviorCortexListening.Activated", "voice direction %d", (int)_dVars.voiceDirection);

  // the stock trigger word response: earcon and get-in (head up)
  if( _iConfig.earCon != AudioMetaData::GameEvent::GenericEvent::Invalid ) {
    GetBEI().GetRobotAudioClient().PostEvent(_iConfig.earCon, AudioMetaData::GameObjectType::Behavior);
  }

  if( _iConfig.getInAnim != AnimationTrigger::Count ) {
    DelegateIfInControl(new TriggerLiftSafeAnimationAction(_iConfig.getInAnim),
                        &BehaviorCortexListening::TransitionToTurnTowardVoice);
  } else {
    TransitionToTurnTowardVoice();
  }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BehaviorCortexListening::TransitionToTurnTowardVoice()
{
  // the stock turn the robot does after "hey vector" (ReactToTriggerDirectionAwake). It decides by itself
  // not to turn on the charger, when picked up, or for voices from 11, 12 and 1 o'clock
  auto& turn = _iConfig.turnBehavior;
  if( (turn != nullptr) && (_dVars.voiceDirection != kMicDirectionUnknown) ) {
    turn->SetReactDirection(_dVars.voiceDirection);
    if( turn->WantsToBeActivated() &&
        DelegateIfInControl(turn.get(), &BehaviorCortexListening::TransitionToHoldListening) ) {
      return;
    }
    turn->ClearReactDirection();
  }

  TransitionToHoldListening();
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BehaviorCortexListening::TransitionToHoldListening()
{
  // end looking up at the user (the stock turns end with the head at 12-19 degrees), then hold the silent eyes
  auto* action = new CompoundActionParallel({
    new MoveHeadToAngleAction(DEG_TO_RAD(_iConfig.headAngle_deg)),
    new PlayAnimationAction(_iConfig.animName),
  });
  DelegateIfInControl(action);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BehaviorCortexListening::BehaviorUpdate()
{
  if( !IsActivated() ) {
    return;
  }

  auto& uic = GetBehaviorComp<UserIntentComponent>();

  if( uic.IsUserIntentPending(USER_INTENT(cortex_listening_end)) ) {
    uic.DropUserIntent(USER_INTENT(cortex_listening_end));
    EndListening(EndReason::ListeningEnd);
    return;
  }

  if( uic.IsUserIntentPending(USER_INTENT(cortex_listening)) ) {
    // a new wake while already listening: keep the eyes and restart the safety timeout
    uic.DropUserIntent(USER_INTENT(cortex_listening));
    _dVars.listeningStart_s = GetCurrentTime_s();
    return;
  }

  if( uic.IsAnyUserIntentPending() ) {
    // the answer (or any other intent) arrived. Leave it pending so its own behavior claims it next
    EndListening(EndReason::NextIntent);
    return;
  }

  if( GetCurrentTime_s() - _dVars.listeningStart_s >= _iConfig.timeout_s ) {
    EndListening(EndReason::Timeout);
  }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BehaviorCortexListening::EndListening(EndReason reason)
{
  _lastEndReason = reason;
  LOG_INFO("BehaviorCortexListening.EndListening", "%s after %.2fs",
           EndReasonToString(reason),
           (reason == EndReason::StrayEnd) ? 0.0f : GetCurrentTime_s() - _dVars.listeningStart_s);
  CancelSelf();
}

} // namespace Vector
} // namespace Anki
