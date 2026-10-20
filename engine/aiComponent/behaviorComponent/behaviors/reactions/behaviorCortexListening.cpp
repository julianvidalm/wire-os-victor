/**
 * File: behaviorCortexListening.cpp
 *
 * Created: 2026-10-19
 *
 * Description: Silent "I'm listening" reaction for the PC cortex. See the header for when it ends.
 *
 **/

#include "engine/aiComponent/behaviorComponent/behaviors/reactions/behaviorCortexListening.h"

#include "clad/types/behaviorComponent/userIntent.h"
#include "coretech/common/engine/jsonTools.h"
#include "coretech/common/engine/utils/timer.h"
#include "engine/actions/animActions.h"
#include "engine/aiComponent/behaviorComponent/userIntentComponent.h"

#define LOG_CHANNEL "Behaviors"

namespace Anki {
namespace Vector {

namespace {
const char* kAnimNameKey = "animName";
const char* kTimeoutKey  = "timeout_s";

const char* kDefaultAnimName = "anim_eyepose_curious";
constexpr float kDefaultTimeout_s = 10.0f;

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
  DelegateIfInControl(new PlayAnimationAction(_iConfig.animName));
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
