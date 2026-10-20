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
#include "engine/components/animationComponent.h"
#include "engine/components/settingsManager.h"
#include "coretech/vision/shared/spritePathMap.h"
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
const char* kEyeHueKey = "eyeHue";
const char* kEyeSaturationKey = "eyeSaturation";
const char* kPromptSpriteKey = "promptSpriteName";
const char* kPromptDurationKey = "promptDuration_s";
const char* kGiveUpAnimKey = "giveUpAnimName";
const char* kThinkingGetInAnimKey = "thinkingGetInAnimName";
const char* kThinkingLoopAnimKey = "thinkingLoopAnimName";
const char* kThinkingTimeoutKey = "thinkingTimeout_s";

const char* kDefaultAnimName = "anim_eyepose_curious";
constexpr float kDefaultTimeout_s = 15.0f;
// UNCANNY_YELLOW in eye_color_config.json
constexpr float kDefaultEyeHue = 0.11f;
constexpr float kDefaultEyeSaturation = 1.0f;
// resources/config/sprites/independentSprites/cortex_question_mark.png (no stock question mark sprite exists)
const char* kDefaultPromptSprite = "cortex_question_mark";
constexpr float kDefaultPromptDuration_s = 3.0f;
// the stock "didn't get that" reaction after a wake word (VC_IntentNeutral / ag_vc_wakeword_idk), 1.0s
const char* kDefaultGiveUpAnim = "anim_wakeword_idk_01";
// the stock KnowledgeGraph "looking it up" face (ag_knowledgegraph_getin_01 / ag_knowledgegraph_searching_01)
const char* kDefaultThinkingGetInAnim = "anim_generic_look_up_01";
const char* kDefaultThinkingLoopAnim = "anim_generic_look_up_idle_01";
// the cortex's LLM can take a while; the answer or cortex_listening_end normally comes well before this
constexpr float kDefaultThinkingTimeout_s = 20.0f;
// the question mark, centered above the eyes. 18x26 sprite drawn in the eye color
constexpr int16_t kPromptSpriteX = 83;
constexpr int16_t kPromptSpriteY = 0;
constexpr int16_t kPromptSpriteWidth = 18;
constexpr int16_t kPromptSpriteHeight = 26;
// the give-up gesture never holds the behavior longer than this, even if its animation never reports done
constexpr float kMaxGiveUp_s = 3.0f;
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
    case BehaviorCortexListening::EndReason::StrayPrompt:  return "StrayPrompt";
    case BehaviorCortexListening::EndReason::StrayThinking: return "StrayThinking";
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

  _iConfig.eyeHue = kDefaultEyeHue;
  JsonTools::GetValueOptional(config, kEyeHueKey, _iConfig.eyeHue);
  _iConfig.eyeSaturation = kDefaultEyeSaturation;
  JsonTools::GetValueOptional(config, kEyeSaturationKey, _iConfig.eyeSaturation);

  // empty strings disable the question mark or the give-up gesture
  _iConfig.promptSpriteName = kDefaultPromptSprite;
  JsonTools::GetValueOptional(config, kPromptSpriteKey, _iConfig.promptSpriteName);
  _iConfig.promptDuration_s = kDefaultPromptDuration_s;
  JsonTools::GetValueOptional(config, kPromptDurationKey, _iConfig.promptDuration_s);
  _iConfig.giveUpAnimName = kDefaultGiveUpAnim;
  JsonTools::GetValueOptional(config, kGiveUpAnimKey, _iConfig.giveUpAnimName);

  // empty strings skip the thinking get-in, or hold the current face instead of a thinking loop
  _iConfig.thinkingGetInAnimName = kDefaultThinkingGetInAnim;
  JsonTools::GetValueOptional(config, kThinkingGetInAnimKey, _iConfig.thinkingGetInAnimName);
  _iConfig.thinkingLoopAnimName = kDefaultThinkingLoopAnim;
  JsonTools::GetValueOptional(config, kThinkingLoopAnimKey, _iConfig.thinkingLoopAnimName);
  _iConfig.thinkingTimeout_s = kDefaultThinkingTimeout_s;
  JsonTools::GetValueOptional(config, kThinkingTimeoutKey, _iConfig.thinkingTimeout_s);
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
  expectedKeys.insert(kEyeHueKey);
  expectedKeys.insert(kEyeSaturationKey);
  expectedKeys.insert(kPromptSpriteKey);
  expectedKeys.insert(kPromptDurationKey);
  expectedKeys.insert(kGiveUpAnimKey);
  expectedKeys.insert(kThinkingGetInAnimKey);
  expectedKeys.insert(kThinkingLoopAnimKey);
  expectedKeys.insert(kThinkingTimeoutKey);
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
  if( uic.IsUserIntentActive(USER_INTENT(cortex_listening_prompt)) ) {
    // same for a prompt about a capture we are not showing
    EndListening(EndReason::StrayPrompt);
    return;
  }
  if( uic.IsUserIntentActive(USER_INTENT(cortex_thinking)) ) {
    // and for the end of speech of a capture we are not showing; its answer is handled by its own behavior
    EndListening(EndReason::StrayThinking);
    return;
  }

  _dVars.listeningStart_s = GetCurrentTime_s();

  // listening eye color, until the behavior ends by any path (OnBehaviorDeactivated)
  GetBEI().GetSettingsManager().ShowTemporaryEyeColor(_iConfig.eyeHue, _iConfig.eyeSaturation);
  _showingListeningEyeColor = true;

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
void BehaviorCortexListening::OnBehaviorDeactivated()
{
  ClearPrompt();
  RestoreEyeColor();
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BehaviorCortexListening::RestoreEyeColor()
{
  if( _showingListeningEyeColor ) {
    GetBEI().GetSettingsManager().RestoreEyeColorSetting();
    _showingListeningEyeColor = false;
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
void BehaviorCortexListening::ShowPrompt()
{
  ++_dVars.numPrompts;
  if( _iConfig.promptSpriteName.empty() ) {
    return;
  }

  // the gesture should be long over; if not, cut it so the face shows the prompt
  CancelDelegates(false);

  // the listening eye pose with a question mark above the eyes (drawn in the eye color), cleared after a while.
  // Played straight on the animation component; the next animation (answer, give-up or idle) interrupts it
  const Vision::SpriteBox box( 100.0f, kPromptSpriteX, kPromptSpriteY, kPromptSpriteWidth, kPromptSpriteHeight,
                               Vision::SpriteBoxName::SpriteBox_1, Vision::LayerName::Layer_6,
                               Vision::SpriteRenderMethod::EyeColor, 0 );
  TimeStamp_t clearTime_ms = static_cast<TimeStamp_t>(std::max(0.0f, _iConfig.promptDuration_s) * 1000.0f);
  clearTime_ms -= clearTime_ms % ANIM_TIME_STEP_MS;
  const std::vector<Vision::SpriteBoxKeyFrame> keyFrames = {
    Vision::SpriteBoxKeyFrame( box, 0, Vision::SpritePathMap::GetAssetID(_iConfig.promptSpriteName),
                               Vision::SpriteSeqEndType::Hold, 0 ),
    Vision::SpriteBoxKeyFrame( box, clearTime_ms, Vision::SpritePathMap::kClearSpriteBoxID,
                               Vision::SpriteSeqEndType::Clear, 0 ),
  };
  const Result res = GetBEI().GetAnimationComponent().PlayAnimWithSpriteBoxKeyFrames(_iConfig.animName, keyFrames);
  _dVars.promptShowing = (res == RESULT_OK);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BehaviorCortexListening::ClearPrompt()
{
  if( !_dVars.promptShowing ) {
    return;
  }
  _dVars.promptShowing = false;

  // replace the prompt animation with an empty one so the question mark never outlives the listening
  const Vision::SpriteBox box( 100.0f, kPromptSpriteX, kPromptSpriteY, kPromptSpriteWidth, kPromptSpriteHeight,
                               Vision::SpriteBoxName::SpriteBox_1, Vision::LayerName::Layer_6,
                               Vision::SpriteRenderMethod::EyeColor, 0 );
  const std::vector<Vision::SpriteBoxKeyFrame> keyFrames = {
    Vision::SpriteBoxKeyFrame( box, 0, Vision::SpritePathMap::kClearSpriteBoxID,
                               Vision::SpriteSeqEndType::Clear, 0 ),
  };
  GetBEI().GetAnimationComponent().PlayAnimWithSpriteBoxKeyFrames("", keyFrames);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BehaviorCortexListening::TransitionToGiveUp()
{
  // the cortex stopped listening: back to the normal eye color, then the stock "never mind" gesture, then end
  _dVars.givingUp = true;
  _dVars.giveUpStart_s = GetCurrentTime_s();
  CancelDelegates(false);
  _dVars.promptShowing = false; // the gesture interrupts the prompt animation
  RestoreEyeColor();

  if( _iConfig.giveUpAnimName.empty() ) {
    EndListening(EndReason::ListeningEnd);
    return;
  }
  DelegateIfInControl(new PlayAnimationAction(_iConfig.giveUpAnimName), [this]() {
    EndListening(EndReason::ListeningEnd);
  });
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BehaviorCortexListening::TransitionToThinking()
{
  // the user stopped talking and the cortex is preparing the answer: the user's eye color again and the
  // "looking it up" face until the answer arrives (claimed by its own behavior), with a longer safety timeout
  _dVars.thinking = true;
  _dVars.listeningStart_s = GetCurrentTime_s();
  CancelDelegates(false);
  ClearPrompt();
  RestoreEyeColor();
  LOG_INFO("BehaviorCortexListening.Thinking", "the cortex is preparing the answer");

  if( _iConfig.thinkingGetInAnimName.empty() ) {
    TransitionToThinkingLoop();
    return;
  }
  DelegateIfInControl(new PlayAnimationAction(_iConfig.thinkingGetInAnimName),
                      &BehaviorCortexListening::TransitionToThinkingLoop);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BehaviorCortexListening::TransitionToThinkingLoop()
{
  if( _iConfig.thinkingLoopAnimName.empty() ) {
    return;
  }
  // loops until the behavior ends (answer, give-up or timeout cancel it)
  DelegateIfInControl(new PlayAnimationAction(_iConfig.thinkingLoopAnimName, 0, true,
                                              (u8)AnimTrackFlag::NO_TRACKS,
                                              PlayAnimationAction::GetInfiniteTimeoutInSeconds()));
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BehaviorCortexListening::BehaviorUpdate()
{
  if( !IsActivated() ) {
    return;
  }

  auto& uic = GetBehaviorComp<UserIntentComponent>();

  if( _dVars.givingUp ) {
    // ends when the gesture does. Late ends or prompts mean nothing now; the answer or a new wake cut it short
    // (left pending: a new wake starts listening again from scratch)
    if( uic.IsUserIntentPending(USER_INTENT(cortex_listening_end)) ) {
      uic.DropUserIntent(USER_INTENT(cortex_listening_end));
    }
    if( uic.IsUserIntentPending(USER_INTENT(cortex_listening_prompt)) ) {
      uic.DropUserIntent(USER_INTENT(cortex_listening_prompt));
    }
    if( uic.IsUserIntentPending(USER_INTENT(cortex_thinking)) ) {
      uic.DropUserIntent(USER_INTENT(cortex_thinking));
    }
    if( uic.IsAnyUserIntentPending() ) {
      EndListening(EndReason::NextIntent);
    } else if( GetCurrentTime_s() - _dVars.giveUpStart_s >= kMaxGiveUp_s ) {
      EndListening(EndReason::ListeningEnd);
    }
    return;
  }

  if( uic.IsUserIntentPending(USER_INTENT(cortex_listening_end)) ) {
    // while listening: no speech. While thinking: nothing to answer. Same short gesture either way
    uic.DropUserIntent(USER_INTENT(cortex_listening_end));
    TransitionToGiveUp();
    return;
  }

  if( uic.IsUserIntentPending(USER_INTENT(cortex_thinking)) ) {
    uic.DropUserIntent(USER_INTENT(cortex_thinking));
    if( !_dVars.thinking ) {
      TransitionToThinking();
    }
    return;
  }

  if( _dVars.thinking && uic.IsUserIntentPending(USER_INTENT(cortex_listening_prompt)) ) {
    // the user already spoke: a late prompt means nothing now
    uic.DropUserIntent(USER_INTENT(cortex_listening_prompt));
    return;
  }

  if( uic.IsUserIntentPending(USER_INTENT(cortex_listening_prompt)) ) {
    // no speech yet: ask with a question mark and keep listening, with a fresh safety timeout
    uic.DropUserIntent(USER_INTENT(cortex_listening_prompt));
    _dVars.listeningStart_s = GetCurrentTime_s();
    ShowPrompt();
    return;
  }

  if( uic.IsUserIntentPending(USER_INTENT(cortex_listening)) ) {
    // a new wake while already listening: keep the eyes and restart the safety timeout. While thinking, the
    // user starts over: back to the listening eyes
    uic.DropUserIntent(USER_INTENT(cortex_listening));
    _dVars.listeningStart_s = GetCurrentTime_s();
    if( _dVars.thinking ) {
      _dVars.thinking = false;
      CancelDelegates(false);
      GetBEI().GetSettingsManager().ShowTemporaryEyeColor(_iConfig.eyeHue, _iConfig.eyeSaturation);
      _showingListeningEyeColor = true;
      TransitionToHoldListening();
    }
    return;
  }

  if( uic.IsAnyUserIntentPending() ) {
    // the answer (or any other intent) arrived. Leave it pending so its own behavior claims it next
    EndListening(EndReason::NextIntent);
    return;
  }

  const float timeout_s = _dVars.thinking ? _iConfig.thinkingTimeout_s : _iConfig.timeout_s;
  if( GetCurrentTime_s() - _dVars.listeningStart_s >= timeout_s ) {
    EndListening(EndReason::Timeout);
  }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BehaviorCortexListening::EndListening(EndReason reason)
{
  _lastEndReason = reason;
  LOG_INFO("BehaviorCortexListening.EndListening", "%s after %.2fs",
           EndReasonToString(reason),
           ((reason == EndReason::StrayEnd) || (reason == EndReason::StrayPrompt) ||
            (reason == EndReason::StrayThinking)) ? 0.0f : GetCurrentTime_s() - _dVars.listeningStart_s);
  CancelSelf();
}

} // namespace Vector
} // namespace Anki
