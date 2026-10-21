/**
* File: behaviorReactToVoiceCommand.cpp
*
* Author: Jarrod Hatfield
* Created: 2/16/2018
*
* Description: Simple behavior to immediately respond to the voice command keyphrase, while waiting for further commands.
*
* Copyright: Anki, Inc. 2017
*
**/

#include "engine/aiComponent/behaviorComponent/behaviors/reactions/behaviorReactToVoiceCommand.h"

#include "audioEngine/multiplexer/audioCladMessageHelper.h"
#include "clad/robotInterface/messageRobotToEngineTag.h"
#include "clad/types/animationTrigger.h"
#include "coretech/common/engine/jsonTools.h"
#include "coretech/common/engine/math/pose.h"
#include "engine/actions/animActions.h"
#include "engine/actions/basicActions.h"
#include "engine/actions/compoundActions.h"
#include "engine/aiComponent/aiComponent.h"
#include "engine/aiComponent/behaviorComponent/attentionTransferComponent.h"
#include "engine/aiComponent/behaviorComponent/behaviorComponent.h"
#include "engine/aiComponent/behaviorComponent/behaviorContainer.h"
#include "engine/aiComponent/behaviorComponent/behaviorExternalInterface/behaviorExternalInterface.h"
#include "engine/aiComponent/behaviorComponent/behaviorExternalInterface/beiRobotInfo.h"
#include "engine/aiComponent/behaviorComponent/behaviors/reactions/behaviorReactToMicDirection.h"
#include "engine/aiComponent/behaviorComponent/userIntentComponent.h"
#include "engine/aiComponent/behaviorComponent/userIntentData.h"
#include "engine/aiComponent/beiConditions/conditions/conditionUserIntentPending.h"
#include "engine/audio/engineRobotAudioClient.h"
#include "engine/components/backpackLights/engineBackpackLightComponent.h"
#include "engine/components/mics/micComponent.h"
#include "engine/components/mics/micDirectionHistory.h"
#include "engine/components/movementComponent.h"
#include "engine/components/settingsManager.h"
#include "engine/cozmoContext.h"
#include "engine/externalInterface/externalMessageRouter.h"
#include "engine/externalInterface/gatewayInterface.h"
#include "engine/faceWorld.h"
#include "engine/moodSystem/moodManager.h"
#include "engine/robotInterface/messageHandler.h"
#include "micDataTypes.h"
#include "osState/osState.h"
#include "util/console/consoleInterface.h"
#include "util/logging/DAS.h"

#include "clad/types/behaviorComponent/behaviorClasses.h"

#include "coretech/common/engine/utils/timer.h"

#include <array>
#include <vector>


#define DEBUG_TRIGGER_WORD_VERBOSE 0 // add some verbose debugging if trying to track down issues

#define LOG_CHANNEL "Microphones"
#define CONSOLE_GROUP "TriggerWord"

#if DEBUG_TRIGGER_WORD_VERBOSE
  // only print these debug if we're tracking down something so we don't spam the logs
  #define PRINT_TRIGGER_DEBUG( format, ... ) \
    LOG_DEBUG( "BehaviorReactToVoiceCommand.TriggerWord", format, ##__VA_ARGS__ )
#else
  #define PRINT_TRIGGER_DEBUG( format, ... )
#endif

#define PRINT_TRIGGER_INFO( format, ... ) \
  LOG_INFO( "BehaviorReactToVoiceCommand.TriggerWord", format, ##__VA_ARGS__ )


namespace Anki {
namespace Vector {

namespace {

  const char* kEarConBegin                         = "earConAudioEventBegin";
  const char* kEarConSuccess                       = "earConAudioEventSuccess";
  const char* kEarConFail                          = "earConAudioEventNeutral";
  const char* kIntentBehaviorKey                   = "behaviorOnIntent";
  const char* kProceduralBackpackLights            = "backpackLights";
  const char* kAnimListeningGetIn                  = "animListeningGetIn";
  const char* kAnimListeningLoop                   = "animListeningLoop";
  const char* kAnimListeningGetOut                 = "animListeningGetOut";
  const char* kExitAfterGetInKey                   = "exitAfterGetIn";
  const char* kExitAfterListeningIfNotStreamingKey = "exitAfterListeningIfNotStreaming";
  const char* kPushResponseKey                     = "pushResponse";
  const char* kNotifyOnWifiErrorsKey               = "notifyOnWifiErrors";
  const char* kNotifyOnCloudErrorsKey              = "notifyOnCloudErrors";

  // If supplied, any received intent not whitelisted will be consumed and the "unknown intent" outcome execized
  const char* kIntentWhitelistKey                  = "whiteListedIntents";

  // If supplied, the listening feedback after the wake (see the "listeningFeedback" keys below). Without it the
  // behavior is the stock one
  const char* kListeningFeedbackKey                = "listeningFeedback";
  const char* kFeedbackEyeHueKey                   = "eyeHue";
  const char* kFeedbackEyeSaturationKey            = "eyeSaturation";
  const char* kFeedbackTurnAtWakeKey               = "turnAtWake";
  const char* kFeedbackWakeDirectionWindowKey      = "wakeDirectionWindow_s";
  const char* kFeedbackMinDirectionConfidenceKey   = "minDirectionConfidence";
  const char* kFeedbackHeadAngleKey                = "headAngle_deg";
  const char* kFeedbackHoldAnimKey                 = "holdAnimName";
  const char* kFeedbackPromptAnimKey               = "promptAnimName";
  const char* kFeedbackPromptAfterKey              = "promptAfter_s";
  const char* kFeedbackGiveUpAnimKey               = "giveUpAnimName";
  const char* kFeedbackGiveUpAfterKey              = "giveUpAfter_s";
  const char* kFeedbackGiveUpWaitKey               = "giveUpWait_s";
  const char* kFeedbackMinSpeechKey                = "minSpeech_s";
  const char* kFeedbackSpeechWindowKey             = "speechWindow_s";
  const char* kFeedbackSettleAfterLookKey          = "settleAfterLook_s";
  const char* kFeedbackThinkingGetInAnimKey        = "thinkingGetInAnimName";
  const char* kFeedbackThinkingLoopAnimKey         = "thinkingLoopAnimName";
  const char* kFeedbackThinkingDelayKey            = "thinkingDelay_s";

  // UNCANNY_YELLOW in eye_color_config.json, the PC cortex listening color
  constexpr float kDefaultFeedbackEyeHue           = 0.11f;
  constexpr float kDefaultFeedbackEyeSaturation    = 1.0f;
  // the wake phrase ("hey vector", ~0.7-0.9s) plus the detection and mic history latency
  constexpr float kDefaultWakeDirectionWindow_s    = 1.5f;
  // same threshold as the stream vote below (kDirStreamingConfToIgnore)
  constexpr int   kDefaultFeedbackMinDirectionConf = 500;
  // VC_ListeningGetIn ends with the head at 33 degrees; the stock turns end lower (12-19)
  constexpr float kDefaultFeedbackHeadAngle_deg    = 33.0f;
  // from the end of the look gesture (pc-cortex S6: '?' after ~4s, then ~3s more, then give up)
  constexpr float kDefaultFeedbackPromptAfter_s    = 4.0f;
  constexpr float kDefaultFeedbackGiveUpAfter_s    = 7.0f;
  // after the give-up gesture, how long the behavior waits for the stream's late result (normally the cloud's
  // noaudio, which ends it at once) before handing the robot back
  constexpr float kDefaultFeedbackGiveUpWait_s     = 4.0f;
  // speech: at least this much voice activity (robot VAD, 10ms samples) within the speech window
  // (a sustained run: the VAD's own hangover and short noises stay under it)
  constexpr float kDefaultFeedbackMinSpeech_s      = 0.6f;
  constexpr float kDefaultFeedbackSpeechWindow_s   = 1.0f;
  // voice activity right after the look gesture is ignored for this long: the VAD still reports the gesture's
  // motor noise, the earcon tail and its own hangover (live: ~100% voice for 0.3s+ after every look, even silent)
  constexpr float kDefaultFeedbackSettleAfterLook_s = 1.0f;
  // while listening, one "vad_sample" event this often, so the levels of silent and spoken wakes can be compared
  constexpr double kFeedbackVadSamplePeriod_s      = 1.0;
  // the stream ended without robot-heard speech: wait this long for the result before the thinking face, so a
  // quick noaudio goes straight to the give-up gesture
  constexpr float kDefaultFeedbackThinkingDelay_s  = 0.3f;

  // a listening feedback step, as a log line and a DAS event (defined with the listening feedback below)
  void LogListeningFeedback(const char* step, int value, double sinceActivation_s, int total = 0);

  struct VoiceSamples {
    uint32_t voice = 0; // samples (10ms) with voice activity
    uint32_t total = 0; // all samples
  };

  // Mic samples (10ms each) heard in [begin_ms, end_ms], and how many of them had voice activity
  VoiceSamples CountVoiceSamples(const MicDirectionHistory& history, EngineTimeStamp_t begin_ms, EngineTimeStamp_t end_ms)
  {
    VoiceSamples result;
    if( end_ms <= begin_ms ) {
      return result;
    }
    const TimeStamp_t window_ms = TimeStamp_t(end_ms - begin_ms);
    const MicDirectionNodeList list = history.GetHistoryAtTime(end_ms, window_ms);
    double voice = 0.0;
    double total = 0.0;
    for( const auto& node : list ) {
      double share = 1.0;
      // only the part of a node that falls inside the window
      if( (node.timestampEnd > node.timestampBegin) && (node.timestampBegin < begin_ms) ) {
        const double nodeDuration = TimeStamp_t(node.timestampEnd - node.timestampBegin);
        const double timeInNode = (node.timestampEnd > begin_ms) ? TimeStamp_t(node.timestampEnd - begin_ms) : 0;
        share = timeInNode / nodeDuration;
      }
      voice += node.vadCount * share;
      total += node.count * share;
    }
    result.voice = static_cast<uint32_t>(voice);
    result.total = static_cast<uint32_t>(total);
    return result;
  }

  CONSOLE_VAR( bool, kRespondsToTriggerWord, CONSOLE_GROUP, true );

  // the behavior will always "listen" for at least this long once it hears the wakeword, even if we receive
  // an error sooner than this. Note that the behavior will also consider the intent to be an error if the
  // stream doesn't open within this amount of time, so don't lower this number too much
  CONSOLE_VAR_RANGED( float, kMinListeningTimeout_s, CONSOLE_GROUP, 5.0f, 0.0f, 30.0f );
  // this is the maximum duration we'll wait from streaming begin
  // MicData::kStreamingTimeout_ms (the robot's mic stream cap, 14s) plus the PC cortex's answer time (whisper and
  // the LLM, ~4-5s). vic-cloud gives up at 20s from the wake (voice.DefaultTimeout), which is before this, since
  // the stream opens after the wake
  CONSOLE_VAR_RANGED( float, kMaxStreamingDuration_s, CONSOLE_GROUP, 20.0f, 0.0f, 30.0f );


  // when our streaming begins/ends there is a high chance that we will record some non-intent sound, these
  // values allow us to chop off the front and back of the streaming window when determining the intent direction
  CONSOLE_VAR_RANGED( double, kDirStreamingTimeToIgnoreBegin,  CONSOLE_GROUP, 0.5, 0.0, 2.0 );
  CONSOLE_VAR_RANGED( double, kDirStreamingTimeToIgnoreEnd,    CONSOLE_GROUP, 1.25, 0.0, 2.0 );
  // ignore mic direction with confidence below this when trying to determine streaming direction
  CONSOLE_VAR_RANGED( MicDirectionConfidence, kDirStreamingConfToIgnore, CONSOLE_GROUP, 500, 0, 10000 );
  // if we cannot determine the mic direction, we fall back to the most recent direction
  // this allows you to specify how far back we sample for the most recent direction
  CONSOLE_VAR_RANGED( double, kRecentDirFallbackTime,          CONSOLE_GROUP, 1.0, 0.0, 10.0 );

// pretend all responses are errors: NOTE intents may still get processed with this set true, recommendation
// is to use silence or a known mismatch intent (my favorite happens to be "potatoes").
  CONSOLE_VAR( bool, kTriggerWord_FakeError, CONSOLE_GROUP, false );
  CONSOLE_VAR( bool, kTriggerWord_FakeError_HasWifi, CONSOLE_GROUP, false );

}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
BehaviorReactToVoiceCommand::InstanceConfig::InstanceConfig() :
  earConBegin( AudioMetaData::GameEvent::GenericEvent::Invalid ),
  earConSuccess( AudioMetaData::GameEvent::GenericEvent::Invalid ),
  earConFail( AudioMetaData::GameEvent::GenericEvent::Invalid ),
  animListeningGetIn( AnimationTrigger::VC_ListeningGetIn ),
  animListeningLoop( AnimationTrigger::VC_ListeningLoop ),
  animListeningGetOut( AnimationTrigger::VC_ListeningGetOut ),
  backpackLights( true ),
  exitAfterGetIn( false ),
  exitAfterListeningIfNotStreaming( false ),
  cloudErrorTracker( "VoiceCommandErrorTracker_nocloud" ),
  wifiErrorTracker( "VoiceCommandErrorTracker_nowifi" )
{

}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
BehaviorReactToVoiceCommand::DynamicVariables::DynamicVariables() :
  state( EState::GetIn ),
  reactionDirection( kMicDirectionUnknown ),
  streamingBeginTime( 0.0 ),
  streamingEndTime( 0.0 ),
  intentStatus( EIntentStatus::NoIntentHeard ),
  timestampToDisableTurnFor(0),
  expectingStream(false)
{

}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
BehaviorReactToVoiceCommand::DynamicVariables::Persistent::Persistent() :
  forcedAnimListeningLoop( AnimationTrigger::Count ),
  forcedAnimListeningGetOut( AnimationTrigger::Count ),
  listeningAnimsResetQueued( false )
{

}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
BehaviorReactToVoiceCommand::BehaviorReactToVoiceCommand( const Json::Value& config ) :
  ICozmoBehavior( config ),
  _triggerDirection( kMicDirectionUnknown )
{
  // do we play ear-con sounds to notify the user when Victor is listening
  {
    std::string earConString;
    if ( JsonTools::GetValueOptional( config, kEarConBegin, earConString ) )
    {
      _iVars.earConBegin = AudioMetaData::GameEvent::GenericEventFromString( earConString );
    }

    if ( JsonTools::GetValueOptional( config, kEarConSuccess, earConString ) )
    {
      _iVars.earConSuccess = AudioMetaData::GameEvent::GenericEventFromString( earConString );
    }

    if ( JsonTools::GetValueOptional( config, kEarConFail, earConString ) )
    {
      _iVars.earConFail = AudioMetaData::GameEvent::GenericEventFromString( earConString );
    }
  }

  // do we play the backpack lights from the behavior, else assume anims will handle it
  JsonTools::GetValueOptional( config, kProceduralBackpackLights, _iVars.backpackLights );

  // get the behavior to play after an intent comes in
  JsonTools::GetValueOptional( config, kIntentBehaviorKey, _iVars.reactionBehaviorString );

  std::string animGetIn;
  if( JsonTools::GetValueOptional( config, kAnimListeningGetIn, animGetIn ) && !animGetIn.empty() )
  {
    ANKI_VERIFY( AnimationTriggerFromString(animGetIn, _iVars.animListeningGetIn),
                 "BehaviorReactToVoiceCommand.Ctor.InvalidGetIn",
                 "Get-in %s is not a valid animation trigger",
                 animGetIn.c_str() );
  }

  std::string animLoop;
  if( JsonTools::GetValueOptional( config, kAnimListeningLoop, animLoop ) && !animLoop.empty() )
  {
    ANKI_VERIFY( AnimationTriggerFromString(animLoop, _iVars.animListeningLoop),
                 "BehaviorReactToVoiceCommand.Ctor.InvalidLoop",
                 "Get-in %s is not a valid animation trigger",
                 animLoop.c_str() );
  }

  std::string animGetOut;
  if( JsonTools::GetValueOptional( config, kAnimListeningGetOut, animGetOut ) && !animGetOut.empty() )
  {
    ANKI_VERIFY( AnimationTriggerFromString(animGetOut, _iVars.animListeningGetOut),
                 "BehaviorReactToVoiceCommand.Ctor.InvalidGetOut",
                 "Get-in %s is not a valid animation trigger",
                 animGetOut.c_str() );
  }

  JsonTools::GetValueOptional( config, kExitAfterGetInKey, _iVars.exitAfterGetIn );

  JsonTools::GetValueOptional( config, kExitAfterListeningIfNotStreamingKey, _iVars.exitAfterListeningIfNotStreaming );

  _iVars.pushResponse = config.get(kPushResponseKey, true).asBool();

  if( !config[kNotifyOnWifiErrorsKey].isNull() )
  {
    int numErrorsToTriggerAnim;
    float errorTrackingWindow_s;

    ANKI_VERIFY( RecentOccurrenceTracker::ParseConfig(config[kNotifyOnWifiErrorsKey],
                                                      numErrorsToTriggerAnim,
                                                      errorTrackingWindow_s),
                 "BehaviorReactToVoiceCommand.Constructor.InvalidConfig",
                 "Behavior '%s' specified invalid recent occurrence config for wifi",
                 GetDebugLabel().c_str() );
    _iVars.wifiErrorHandle = _iVars.wifiErrorTracker.GetHandle(numErrorsToTriggerAnim, errorTrackingWindow_s);
  }
  else {
    PRINT_NAMED_WARNING("BehaviorReactToVoiceCommand.NoErrorNotification.WifiErrors",
                        "Behavior '%s' is not configured to show wifi errors at all",
                        GetDebugLabel().c_str());
  }

  if( !config[kNotifyOnCloudErrorsKey].isNull() )
  {
    int numErrorsToTriggerAnim;
    float errorTrackingWindow_s;

    ANKI_VERIFY( RecentOccurrenceTracker::ParseConfig(config[kNotifyOnCloudErrorsKey],
                                                      numErrorsToTriggerAnim,
                                                      errorTrackingWindow_s),
                 "BehaviorReactToVoiceCommand.Constructor.InvalidConfig",
                 "Behavior '%s' specified invalid recent occurrence config for cloud",
                 GetDebugLabel().c_str() );
    _iVars.cloudErrorHandle = _iVars.cloudErrorTracker.GetHandle(numErrorsToTriggerAnim, errorTrackingWindow_s);
  }
  else {
    PRINT_NAMED_WARNING("BehaviorReactToVoiceCommand.NoErrorNotification.CloudErrors",
                        "Behavior '%s' is not configured to show cloud errors at all",
                        GetDebugLabel().c_str());
  }

  if( !config[kIntentWhitelistKey].isNull() )
  {
    const Json::Value& whiteListedIntents = ConditionUserIntentPending::GenerateConfig(config[kIntentWhitelistKey]);
    _iVars.intentWhitelistCondition = std::make_unique<ConditionUserIntentPending>(whiteListedIntents);
  }

  const Json::Value& feedbackConfig = config[kListeningFeedbackKey];
  if( feedbackConfig.isObject() )
  {
    auto& fb = _iVars.feedback;
    fb.enabled = true;
    fb.eyeHue = kDefaultFeedbackEyeHue;
    JsonTools::GetValueOptional( feedbackConfig, kFeedbackEyeHueKey, fb.eyeHue );
    fb.eyeSaturation = kDefaultFeedbackEyeSaturation;
    JsonTools::GetValueOptional( feedbackConfig, kFeedbackEyeSaturationKey, fb.eyeSaturation );
    // the turn is behaviorOnIntent's (the stock trigger word turn), done at the wake instead of after the intent
    fb.turnAtWake = true;
    JsonTools::GetValueOptional( feedbackConfig, kFeedbackTurnAtWakeKey, fb.turnAtWake );
    fb.wakeDirectionWindow_s = kDefaultWakeDirectionWindow_s;
    JsonTools::GetValueOptional( feedbackConfig, kFeedbackWakeDirectionWindowKey, fb.wakeDirectionWindow_s );
    int minDirectionConfidence = kDefaultFeedbackMinDirectionConf;
    JsonTools::GetValueOptional( feedbackConfig, kFeedbackMinDirectionConfidenceKey, minDirectionConfidence );
    fb.minDirectionConfidence = static_cast<MicDirectionConfidence>(minDirectionConfidence);
    fb.headAngle_deg = kDefaultFeedbackHeadAngle_deg;
    JsonTools::GetValueOptional( feedbackConfig, kFeedbackHeadAngleKey, fb.headAngle_deg );
    // empty animation names skip that clip
    JsonTools::GetValueOptional( feedbackConfig, kFeedbackHoldAnimKey, fb.holdAnimName );
    JsonTools::GetValueOptional( feedbackConfig, kFeedbackPromptAnimKey, fb.promptAnimName );
    fb.promptAfter_s = kDefaultFeedbackPromptAfter_s;
    JsonTools::GetValueOptional( feedbackConfig, kFeedbackPromptAfterKey, fb.promptAfter_s );
    JsonTools::GetValueOptional( feedbackConfig, kFeedbackGiveUpAnimKey, fb.giveUpAnimName );
    fb.giveUpAfter_s = kDefaultFeedbackGiveUpAfter_s;
    JsonTools::GetValueOptional( feedbackConfig, kFeedbackGiveUpAfterKey, fb.giveUpAfter_s );
    fb.giveUpWait_s = kDefaultFeedbackGiveUpWait_s;
    JsonTools::GetValueOptional( feedbackConfig, kFeedbackGiveUpWaitKey, fb.giveUpWait_s );
    fb.minSpeech_s = kDefaultFeedbackMinSpeech_s;
    JsonTools::GetValueOptional( feedbackConfig, kFeedbackMinSpeechKey, fb.minSpeech_s );
    fb.speechWindow_s = kDefaultFeedbackSpeechWindow_s;
    JsonTools::GetValueOptional( feedbackConfig, kFeedbackSpeechWindowKey, fb.speechWindow_s );
    fb.settleAfterLook_s = kDefaultFeedbackSettleAfterLook_s;
    JsonTools::GetValueOptional( feedbackConfig, kFeedbackSettleAfterLookKey, fb.settleAfterLook_s );
    JsonTools::GetValueOptional( feedbackConfig, kFeedbackThinkingGetInAnimKey, fb.thinkingGetInAnimName );
    JsonTools::GetValueOptional( feedbackConfig, kFeedbackThinkingLoopAnimKey, fb.thinkingLoopAnimName );
    fb.thinkingDelay_s = kDefaultFeedbackThinkingDelay_s;
    JsonTools::GetValueOptional( feedbackConfig, kFeedbackThinkingDelayKey, fb.thinkingDelay_s );
  }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
// Public destructor must be explicitly defined to allow std::unique_ptr<FwdDeclaredType> (ConditionUserIntentPending)
BehaviorReactToVoiceCommand::~BehaviorReactToVoiceCommand()
{
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BehaviorReactToVoiceCommand::GetBehaviorJsonKeys(std::set<const char*>& expectedKeys) const
{
  const char* list[] = {
    kEarConBegin,
    kEarConSuccess,
    kEarConFail,
    kProceduralBackpackLights,
    kIntentBehaviorKey,
    kAnimListeningGetIn,
    kAnimListeningLoop,
    kAnimListeningGetOut,
    kExitAfterGetInKey,
    kExitAfterListeningIfNotStreamingKey,
    kPushResponseKey,
    kNotifyOnWifiErrorsKey,
    kNotifyOnCloudErrorsKey,
    kIntentWhitelistKey,
    kListeningFeedbackKey
  };
  expectedKeys.insert( std::begin(list), std::end(list) );
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BehaviorReactToVoiceCommand::InitBehavior()
{
  // grab our reaction behavior ...
  if ( !_iVars.reactionBehaviorString.empty() )
  {
    ICozmoBehaviorPtr reactionBehavior = FindBehavior( _iVars.reactionBehaviorString );
    // downcast to a BehaviorReactToMicDirection since we're forcing all reactions to be of this behavior
    DEV_ASSERT_MSG( reactionBehavior->GetClass() == BehaviorClass::ReactToMicDirection,
                    "BehaviorReactToVoiceCommand.Init.IncorrectMicDirectionBehavior",
                    "Reaction behavior specified is not of valid class BehaviorClass::ReactToMicDirection");
    _iVars.reactionBehavior = std::static_pointer_cast<BehaviorReactToMicDirection>(reactionBehavior);
  }

  _iVars.unmatchedIntentBehavior = GetBEI().GetBehaviorContainer().FindBehaviorByID( BEHAVIOR_ID(IntentUnmatched) );
  DEV_ASSERT( _iVars.unmatchedIntentBehavior != nullptr,
              "BehaviorReactToVoiceCommand.Init.UnmatchedIntentBehaviorMissing");

  _iVars.silenceIntentBehavior = GetBEI().GetBehaviorContainer().FindBehaviorByID(BEHAVIOR_ID(TriggerWordWithoutIntent));
  DEV_ASSERT( _iVars.silenceIntentBehavior != nullptr,
              "BehaviorReactToVoiceCommand.Init.silenceIntentBehavior");

  _iVars.noCloudBehavior = GetBEI().GetBehaviorContainer().FindBehaviorByID( BEHAVIOR_ID(NoCloud) );
  DEV_ASSERT( _iVars.noCloudBehavior != nullptr,
              "BehaviorReactToVoiceCommand.Init.NoCloudBehaviorMissing");

  _iVars.noWifiBehavior = GetBEI().GetBehaviorContainer().FindBehaviorByID( BEHAVIOR_ID(NoWifi) );
  DEV_ASSERT( _iVars.noWifiBehavior != nullptr,
              "BehaviorReactToVoiceCommand.Init.NoWifiBehaviorMissing");

  SubscribeToTags(
  {
    RobotInterface::RobotToEngineTag::triggerWordDetected
  });

  if( nullptr != _iVars.intentWhitelistCondition )
  {
    _iVars.intentWhitelistCondition->SetOwnerDebugLabel(GetDebugLabel());
    _iVars.intentWhitelistCondition->Init(GetBEI());
  }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BehaviorReactToVoiceCommand::GetAllDelegates( std::set<IBehavior*>& delegates ) const
{
  if ( _iVars.reactionBehavior )
  {
    delegates.insert( _iVars.reactionBehavior.get() );
  }

  delegates.insert( _iVars.unmatchedIntentBehavior.get() );
  delegates.insert( _iVars.silenceIntentBehavior.get() );
  delegates.insert( _iVars.noCloudBehavior.get() );
  delegates.insert( _iVars.noWifiBehavior.get() );
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BehaviorReactToVoiceCommand::GetBehaviorOperationModifiers( BehaviorOperationModifiers& modifiers ) const
{
  modifiers.wantsToBeActivatedWhenCarryingObject  = true;
  modifiers.wantsToBeActivatedWhenOnCharger       = true;
  modifiers.wantsToBeActivatedWhenOffTreads       = true;
  modifiers.behaviorAlwaysDelegates               = false;

  // Since so many voice commands need faces, this helps improve the changes that a behavior following
  // this one will know about faces when the behavior starts
  modifiers.visionModesForActiveScope->insert({ VisionMode::Faces, EVisionUpdateFrequency::High });
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
bool BehaviorReactToVoiceCommand::WantsToBeActivatedBehavior() const
{
  const UserIntentComponent& uic = GetBehaviorComp<UserIntentComponent>();
  return kRespondsToTriggerWord && uic.IsTriggerWordPending();
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BehaviorReactToVoiceCommand::AlwaysHandleInScope( const RobotToEngineEvent& event )
{
  if ( event.GetData().GetTag() == RobotInterface::RobotToEngineTag::triggerWordDetected )
  {
    const auto& msg = event.GetData().Get_triggerWordDetected();
    _triggerDirection = msg.direction;

    #if DEBUG_TRIGGER_WORD_VERBOSE
    {
      LOG_DEBUG( "BehaviorReactToVoiceCommand.Debug",
                 "Received TriggerWordDetected event with direction [%d]", (int)_triggerDirection );
    }
    #endif
  }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BehaviorReactToVoiceCommand::OnBehaviorEnteredActivatableScope()
{
  // don't push a custom response if disabled via config, in case, e.g., a parent wants control of the response
  if( _iVars.pushResponse ) {
    namespace AECH = AudioEngine::Multiplexer::CladMessageHelper;
    auto postAudioEvent = AECH::CreatePostAudioEvent( _iVars.earConBegin, AudioMetaData::GameObjectType::Behavior, 0 );
    GetBehaviorComp<UserIntentComponent>().PushResponseToTriggerWord( GetDebugLabel(),
                                                                      _iVars.animListeningGetIn,
                                                                      postAudioEvent,
                                                                      StreamAndLightEffect::StreamingEnabled );
  }
}


// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BehaviorReactToVoiceCommand::OnBehaviorActivated()
{
  const auto persistent = _dVars.persistent;
  _dVars = DynamicVariables();
  _dVars.persistent = persistent;

  UserIntentComponent& uic = GetBehaviorComp<UserIntentComponent>();
  uic.ClearPendingTriggerWord();

  auto* gi = GetBEI().GetRobotInfo().GetGatewayInterface();
  if( gi != nullptr ) {
    auto* wakeWordBegin = new external_interface::WakeWordBegin;
    gi->Broadcast( ExternalMessageRouter::Wrap(wakeWordBegin) );
  }

  if ( GetBEI().HasMoodManager() )
  {
    auto& moodManager = GetBEI().GetMoodManager();
    moodManager.TriggerEmotionEvent( "ReactToTriggerWord", MoodManager::GetCurrentTimeInSeconds() );
  }

  _dVars.expectingStream = uic.WillPendingTriggerWordOpenStream();

  // Trigger word is heard (since we've been activated) ...
  LOG_DEBUG( "BehaviorReactToVoiceCommand.Activated",
             "Reacting to trigger word from direction [%d] (%s stream)",
             (int)GetReactionDirection(),
             _dVars.expectingStream ? "expecting" : "not expecting" );

  if( nullptr != _iVars.intentWhitelistCondition )
  {
    const bool setActive = true;
    _iVars.intentWhitelistCondition->SetActive(GetBEI(), setActive);
  }

  if( IsListeningFeedbackEnabled() )
  {
    // listening eye color from the wake until listening ends (RestoreListeningEyeColor on every exit)
    GetBEI().GetSettingsManager().ShowTemporaryEyeColor( _iVars.feedback.eyeHue, _iVars.feedback.eyeSaturation );
    _showingListeningEyeColor = true;
    // the applied color, to check live that the listening color was sent at the wake (hue and saturation x1000)
    LogListeningFeedback( "eye_color_applied", static_cast<int>( _iVars.feedback.eyeHue * 1000.0f ), 0.0,
                          static_cast<int>( _iVars.feedback.eyeSaturation * 1000.0f ) );
    // pick the direction before anything moves: the mic history is relative to the robot's current heading
    PickWakeDirection();
  }

  StartListening();
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BehaviorReactToVoiceCommand::OnBehaviorDeactivated()
{
  // in case we were interrupted before we had a chance to turn off backpack lights, do so now ...
  if ( _iVars.backpackLights && _dVars.lightsHandle.IsValid() )
  {
    BackpackLightComponent& blc = GetBEI().GetBackpackLightComponent();
    blc.StopLoopingBackpackAnimation( _dVars.lightsHandle );
  }

  // any exit (interrupted, timed out, gave up, handed over to the intent's behavior)
  RestoreListeningEyeColor();

  auto* gi = GetBEI().GetRobotInfo().GetGatewayInterface();
  if( gi != nullptr ) {
    auto* wakeWordEnd = new external_interface::WakeWordEnd;
    const bool intentHeard = ( (_dVars.intentStatus != EIntentStatus::NoIntentHeard) &&
                               (_dVars.intentStatus != EIntentStatus::SilenceTimeout) &&
                               (_dVars.intentStatus != EIntentStatus::Error) );
    wakeWordEnd->set_intent_heard( intentHeard );
    if( intentHeard ) {
      UserIntentComponent& uic = GetBehaviorComp<UserIntentComponent>();
      // we use this dirty dirty method here instead of sending this message directly from the uic
      // since we know whether the intent was heard here, and it's nice that the same behavior
      // on activation/deactivation sends the two messages. if the uic sent the end message, it
      // might be sent without an initial message.
      const UserIntentData* intentData = uic.GetPendingUserIntent();
      if( intentData != nullptr ) {
        // ideally we'd send a proto message structured the same as the intent, but this would mean
        // duplicating the entire userIntent.clad file for proto, or converting the engine handling
        // of intents to clad, neither of which I've got time for.
        std::stringstream ss;
        ss << intentData->intent.GetJSON();
        wakeWordEnd->set_intent_json( ss.str() );
      }
    }
    gi->Broadcast( ExternalMessageRouter::Wrap(wakeWordEnd) );
  }

  if( nullptr != _iVars.intentWhitelistCondition )
  {
    const bool setActive = false;
    _iVars.intentWhitelistCondition->SetActive(GetBEI(), setActive);
  }

  // we've done all we can, now it's up to the next behavior to consume the user intent
  GetBehaviorComp<UserIntentComponent>().SetUserIntentTimeoutEnabled( true );

  // reset this bad boy
  _triggerDirection = kMicDirectionUnknown;
  
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BehaviorReactToVoiceCommand::OnBehaviorLeftActivatableScope()
{
  if( _iVars.pushResponse ) {
    GetBehaviorComp<UserIntentComponent>().PopResponseToTriggerWord(GetDebugLabel());
  }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BehaviorReactToVoiceCommand::BehaviorUpdate()
{
  if(!IsActivated()){
    if (_dVars.persistent.listeningAnimsResetQueued) {
      ResetListeningAnimsToConfig();
    }
    return;
  }

  DEV_ASSERT( ( GetStreamingDuration() >= ( MicData::kStreamingTimeout_ms / 1000.0 ) ),
              "BehaviorReactToVoiceCommand: Behavior streaming timeout is less than mic streaming timeout" );

  const bool wasStreaming = ( _dVars.streamingBeginTime > 0.0 );
  const bool isStreaming = GetBehaviorComp<UserIntentComponent>().IsCloudStreamOpen();

  // track when our stream opens and closes; technically this is not synced with our states which is why we track
  // it independently.
  if ( !wasStreaming )
  {
    if ( isStreaming )
    {
      OnStreamingBegin();
    }
  }
  else
  {
    const bool notAlreadyRecordedEnd = ( _dVars.streamingEndTime <= 0.0 );
    if ( !isStreaming && notAlreadyRecordedEnd )
    {
      OnStreamingEnd();
    }
  }


  if ( _dVars.state == EState::ListeningGetIn )
  {
    // Once the animation process's GetIn animation has finished, queue the listening loop animation
    if(!GetBehaviorComp<UserIntentComponent>().WaitingForTriggerWordGetInToFinish()){

      // we don't want to enter EState::Listening until we're in our loop or else
      // we could end up exiting too soon and looking like garbage
      if( _iVars.exitAfterGetIn )
      {
        OnVictorListeningEnd();
        CancelSelf();
      }

      // we now loop indefinitely and wait for the timeout in the update function
      // this is because we don't know when the streaming will begin (if it hasn't already) so we can't time it accurately
      if ( IsListeningFeedbackEnabled() ) {
        // instead of the listening loop: turn toward the wake phrase, then hold the listening pose
        StartFeedbackLook();
      } else if ( _dVars.persistent.forcedAnimListeningLoop != AnimationTrigger::Count ) {
        DelegateIfInControl( new ReselectingLoopAnimationAction( _dVars.persistent.forcedAnimListeningLoop ) );
      } else {
        DelegateIfInControl( new ReselectingLoopAnimationAction( _iVars.animListeningLoop ) );
      }
      _dVars.state = EState::ListeningLoop;
    }
  }
  else if ( _dVars.state == EState::ListeningLoop )
  {
    const bool isIntentPending = GetBehaviorComp<UserIntentComponent>().IsAnyUserIntentPending();
    if ( IsListeningFeedbackEnabled() && UpdateFeedbackGiveUp() )
    {
      // the robot already gave up; the stream's late result was handled there
    }
    else if ( isIntentPending )
    {
      // kill delegates, we'll handle next steps with callbacks
      // note: passing true to CancelDelegates doesn't call the callback if we also delegate
      LOG_INFO( "BehaviorReactToVoiceCommand.StopListening.IntentPending",
                "Stopping listening because an intent is pending" );
      CancelDelegates( false );
      StopListening();
    }
    else
    {
      // there are a few ways we can timeout from the Listening state;
      // + error received
      // + streaming never started
      // + streaming started but no intent came back
      const double currTime_s = BaseStationTimer::getInstance()->GetCurrentTimeInSecondsDouble();
      const double listeningTimeout = GetListeningTimeout();
      if ( currTime_s >= listeningTimeout )
      {
        LOG_INFO( "BehaviorReactToVoiceCommand.StopListening.Error",
                  "Stopping listening because of a(n) %s",
                  GetBehaviorComp<UserIntentComponent>().WasUserIntentError() ? "error" : "timeout" );
        CancelDelegates( false );
        StopListening();
      }
      else if ( IsListeningFeedbackEnabled() )
      {
        UpdateListeningFeedback();
      }
    }
  }

  // with the listening feedback the listening pose is held without a delegate
  const bool holdingWithoutDelegate = IsListeningFeedbackEnabled() && ( _dVars.state == EState::ListeningLoop );
  if ( IsActivated() && ( _dVars.state != EState::ListeningGetIn ) && !holdingWithoutDelegate && !IsControlDelegated() )
  {
    CancelSelf();
  }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BehaviorReactToVoiceCommand::ComputeReactionDirectionFromStream()
{
  // what we are trying to do is figure out the direction that the user is located with respect to the robot.
  // we know the robot will not be moving while streaming, so we can assume all directions recorded during streaming
  // are in the same "coordinate space".
  // -> We will take the most common direction recorded during streaming as the most probable direction the user is located
  // we can expect a few spikes in noise causing the speaking direction to be false, but hopefully the user's actual
  // speaking direction will be the most consistent
  const MicDirectionHistory& micHistory = GetBEI().GetMicComponent().GetMicDirectionHistory();
  if ( _dVars.streamingBeginTime > 0.0f )
  {
    // take into account the "uknown" direction
    // we are going to ignore the unknown direction in our calculations since it is not helpful to us and very misleading
    // as any time the robot is moving or making noise will record the direction as unknown, skewing us to that direction
    constexpr uint16_t kNumHistoryIndices = ( kNumMicDirections + 1 );
    using DirectionHistoryCount = std::array<uint32_t, kNumHistoryIndices>;
    DirectionHistoryCount micDirectionCounts{};

    const TimeStamp_t streamBeginTime = ( _dVars.streamingBeginTime + kDirStreamingTimeToIgnoreBegin ) * 1000.0;
    const TimeStamp_t streamEndTime = ( _dVars.streamingEndTime - kDirStreamingTimeToIgnoreEnd ) * 1000.0;

    if ( streamEndTime >= streamBeginTime )
    {
      const TimeStamp_t elapsed = ( streamEndTime - streamBeginTime );
      MicDirectionNodeList list = micHistory.GetHistoryAtTime( streamEndTime, elapsed );

      // in the case where we have more than 1 sample, we want ignore partial samples outside of our time range, so
      // we'll just lop off the front and back portions of the list nodes
      if ( list.size() > 1 )
      {
        // case where the front extends beyond our streaming begin time ...
        MicDirectionNode& front = list.front();
        if ( front.timestampBegin < streamBeginTime )
        {
          DEV_ASSERT( ( front.timestampEnd >= streamBeginTime ), "Including node that is outside of streaming window" );
          const double nodeDuration = TimeStamp_t( front.timestampEnd - front.timestampBegin );
          const double timeInNode = TimeStamp_t( front.timestampEnd - streamBeginTime );
          front.count *= ( timeInNode / nodeDuration );
        }

        // case where the back extends beyond our streaming end time ...
        MicDirectionNode& back = list.back();
        if ( back.timestampEnd > streamEndTime )
        {
          DEV_ASSERT( ( back.timestampBegin <= streamEndTime ), "Including node that is outside of streaming window" );
          const double nodeDuration = TimeStamp_t( back.timestampEnd - back.timestampBegin );
          const double timeInNode = TimeStamp_t( streamEndTime - back.timestampBegin );
          back.count *= ( timeInNode / nodeDuration );
        }
      }

      // walk our list of heard directions and add up all of their counts
      // we're assuming a constant sample rate; we could always add up times as well
      for ( const auto& node : list )
      {
        // ignore directions that have too low of a confidence
        if ( node.confidenceAvg > kDirStreamingConfToIgnore )
        {
          PRINT_TRIGGER_DEBUG( "Heard valid direction [%d], with confidence [%d]", node.directionIndex, node.confidenceAvg );
          micDirectionCounts[node.directionIndex] += node.count;
        }
      }

      // now go through all of our valid directions and find the one with the highset count
      uint32_t highestCount = 0;
      for ( MicDirectionIndex i = 0; i < kNumHistoryIndices; ++i )
      {
        PRINT_TRIGGER_DEBUG( "Direction [%d], Count [%d]", i, micDirectionCounts[i] );
        if ( ( i != kMicDirectionUnknown ) && ( micDirectionCounts[i] > highestCount ) )
        {
          _dVars.reactionDirection = i;
          highestCount = micDirectionCounts[i];
        }
      }

      PRINT_TRIGGER_INFO( "Computed trigger reaction direction of %d", (int)_dVars.reactionDirection );
    }
    else
    {
      PRINT_TRIGGER_INFO( "Streaming duration was too short, falling back to most recent direction" );
      _dVars.reactionDirection = GetDirectionFromMicHistory();
    }
  }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
MicDirectionIndex BehaviorReactToVoiceCommand::GetReactionDirection() const
{
  MicDirectionIndex direction = _dVars.reactionDirection;
  if ( kMicDirectionUnknown == direction )
  {
    PRINT_TRIGGER_INFO( "Didn't have a reaction or trigger direction, so falling back to trigger direction" );

    // fallback to our trigger direction
    // accuracy is generally off by the amount the robot has turned
    // there's been some observed inaccuracy with this direction reported from trigger word event
    direction = _triggerDirection;
  }

  if ( kMicDirectionUnknown == direction )
  {
    PRINT_TRIGGER_INFO( "Didn't have a reaction or trigger direction, so falling back to latest selected direction" );

    // this is the least accurate if called post-intent
    // no difference if called pre-intent / post-trigger word
    direction = GetDirectionFromMicHistory();
  }

  return direction;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
MicDirectionIndex BehaviorReactToVoiceCommand::GetDirectionFromMicHistory() const
{
  const TimeStamp_t duration = ( kRecentDirFallbackTime * 1000.0 );

  const MicDirectionHistory& micHistory = GetBEI().GetMicComponent().GetMicDirectionHistory();
  MicDirectionIndex direction = micHistory.GetRecentDirection( duration );

  return direction;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BehaviorReactToVoiceCommand::OnStreamingBegin()
{
  LOG_INFO( "BehaviorReactToVoiceCommand.OnStreamingBegin",
            "Got notice that cloud stream is open" );
  _dVars.streamingBeginTime = BaseStationTimer::getInstance()->GetCurrentTimeInSecondsDouble();
  _dVars.streamingEndTime = 0.0; // reset this to we can match our start/ends
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BehaviorReactToVoiceCommand::OnStreamingEnd()
{
  // only record end time if we've begun streaming
  const bool hasBegunStreaming = ( _dVars.streamingBeginTime > 0.0 );
  const bool notAlreadyRecordedEnd = ( _dVars.streamingEndTime <= 0.0 );
  if ( hasBegunStreaming && notAlreadyRecordedEnd )
  {
    LOG_INFO( "BehaviorReactToVoiceCommand.OnStreamingEnd",
              "Got notice that cloud stream is closed" );

    _dVars.streamingEndTime = BaseStationTimer::getInstance()->GetCurrentTimeInSecondsDouble();

    // let's attempt to compute the reaction direction as soon as we know the stream is closed
    // note: this can be called outside of IsActivated(), but it doesn't matter to us
    ComputeReactionDirectionFromStream();
  }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BehaviorReactToVoiceCommand::StartListening()
{
  _dVars.state = EState::ListeningGetIn;

  // to get into our listening state, we need to play our get-in anim followed by our
  // looping animation.
  OnVictorListeningBegin();
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BehaviorReactToVoiceCommand::StopListening()
{
  ASSERT_NAMED_EVENT( _dVars.state == EState::ListeningLoop,
                      "BehaviorReactToVoiceCommand.State",
                      "Transitioning to EState::IntentReceived from invalid state [%hhu]", static_cast<uint8_t>(_dVars.state) );

  // force our model of the streaming to close, in the case that we timed out (etc) before the actual stream closed
  OnStreamingEnd();

  // listening is over: the user's eye color again before the answer, the give-up gesture or the error reaction
  RestoreListeningEyeColor();

  UpdateUserIntentStatus();
  OnVictorListeningEnd();
  TransitionToThinking();
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BehaviorReactToVoiceCommand::OnVictorListeningBegin()
{
  using namespace AudioMetaData::GameEvent;

  static const BackpackLightAnimation::BackpackAnimation kStreamingLights =
  {
    .onColors               = {{NamedColors::CYAN,NamedColors::CYAN,NamedColors::CYAN}},
    .offColors              = {{NamedColors::CYAN,NamedColors::CYAN,NamedColors::CYAN}},
    .onPeriod_ms            = {{0,0,0}},
    .offPeriod_ms           = {{0,0,0}},
    .transitionOnPeriod_ms  = {{0,0,0}},
    .transitionOffPeriod_ms = {{0,0,0}},
    .offset                 = {{0,0,0}}
  };

  if ( _iVars.backpackLights )
  {
    BackpackLightComponent& blc = GetBEI().GetBackpackLightComponent();
    blc.StartLoopingBackpackAnimation( kStreamingLights, _dVars.lightsHandle );
  }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BehaviorReactToVoiceCommand::OnVictorListeningEnd()
{
  using namespace AudioMetaData::GameEvent;

  if ( _iVars.backpackLights && _dVars.lightsHandle.IsValid() )
  {
    BackpackLightComponent& blc = GetBEI().GetBackpackLightComponent();
    blc.StopLoopingBackpackAnimation( _dVars.lightsHandle );
  }

  // play our "earcon end" audio, which depends on if our intent was successfully heard or not. The "understood"
  // chime is off by default (UserIntentComponent::PlaysVoiceRequestSuccessEarcon); "didn't understand" plays
  BehaviorExternalInterface& bei = GetBEI();
  if ( EIntentStatus::IntentHeard == _dVars.intentStatus )
  {
    if ( ( GenericEvent::Invalid != _iVars.earConSuccess ) && UserIntentComponent::PlaysVoiceRequestSuccessEarcon() )
    {
      // Play earcon end audio
      bei.GetRobotAudioClient().PostEvent( _iVars.earConSuccess, AudioMetaData::GameObjectType::Behavior );
    }
  }
  else
  {
    if ( GenericEvent::Invalid != _iVars.earConFail )
    {
      // Play earcon end audio
      bei.GetRobotAudioClient().PostEvent( _iVars.earConFail, AudioMetaData::GameObjectType::Behavior );
    }
  }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BehaviorReactToVoiceCommand::UpdateUserIntentStatus()
{
  UserIntentComponent& uic = GetBehaviorComp<UserIntentComponent>();
  if ( ( _dVars.intentStatus == EIntentStatus::NoIntentHeard ) && uic.IsAnyUserIntentPending() )
  {
    // next behavior is going to deal with the intent, but we still have more shit to do
    uic.SetUserIntentTimeoutEnabled( false );

    _dVars.intentStatus = EIntentStatus::IntentHeard;

    LOG_DEBUG( "BehaviorReactToVoiceCommand.UpdateUserIntentStatus.Heard",
               "Heard an intent" );

    bool shouldTransitionToIntentFeedback = true;

    if( nullptr != _iVars.intentWhitelistCondition && !_iVars.intentWhitelistCondition->AreConditionsMet(GetBEI()) )
    {
      LOG_DEBUG( "BehaviorReactToVoiceCommand.UpdateUserIntentStatus.NotOnWhitelist",
                 "Heard an intent, but it was not on the intent whitelist" );
      uic.DropAnyUserIntent();
      _dVars.intentStatus = EIntentStatus::IntentUnknown;
    }

    static const UserIntentTag unmatched = USER_INTENT(unmatched_intent);
    if ( uic.IsUserIntentPending( unmatched ) )
    {
      shouldTransitionToIntentFeedback = false;

      SmartActivateUserIntent( unmatched, false );
      _dVars.intentStatus = EIntentStatus::IntentUnknown;
      LOG_DEBUG( "BehaviorReactToVoiceCommand.UpdateUserIntentStatus.Unknown",
                 "Heard an intent, but it was unknown" );
    }

    static const UserIntentTag silence = USER_INTENT(silence);
    if ( uic.IsUserIntentPending( silence ) )
    {
      shouldTransitionToIntentFeedback = false;

      SmartActivateUserIntent( silence, false );
      _dVars.intentStatus = EIntentStatus::SilenceTimeout;
      LOG_DEBUG( "BehaviorReactToVoiceCommand.UpdateUserIntentStatus.Silence",
                 "Got response declaring silence timeout occurred" );
    }

    // so we're assuming that a behavior is going to come along and consume this pending intent, so we want to
    // transition our streaming lights into the intent feedback lights
    if ( shouldTransitionToIntentFeedback )
    {
      uic.StartTransitionIntoActiveUserIntentFeedback();
    }
  }
  else if( uic.WasUserIntentError() ) {
    uic.ResetUserIntentError();
    _dVars.intentStatus = EIntentStatus::Error;
    LOG_DEBUG( "BehaviorReactToVoiceCommand.UpdateUserIntentStatus.Error",
               "latest user intent was an error" );
  }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BehaviorReactToVoiceCommand::TransitionToThinking()
{
  _dVars.state = EState::Thinking;

  LOG_DEBUG( "BehaviorReactToVoiceCommand.TransitionToThinking", "Thinking state starting");

  auto callback = [this]()
  {
    const bool streamingToCloud = _dVars.expectingStream;
    if (!streamingToCloud && _iVars.exitAfterListeningIfNotStreaming) {
      PRINT_CH_INFO("Behaviors", "BehaviorReactToVoiceCommand.TransitionToThinkingCallback.NotStreaming",
                    "We are not streaming to the cloud currently, so no point in continuing with the behavior (since "
                    "we do not want to increment the error count, etc.). Playing the \"unheard\" anim then exiting");
      DelegateIfInControl( new TriggerLiftSafeAnimationAction( AnimationTrigger::VC_IntentNeutral ) );
      return;
    }

    // Play a reaction behavior if we were told to ...
    // ** only in the case that we've heard a valid intent **
    const bool heardValidIntent = ( _dVars.intentStatus == EIntentStatus::IntentHeard );
    const bool turnedAtWake = IsListeningFeedbackEnabled() && _iVars.feedback.turnAtWake;
    if ( heardValidIntent && _iVars.reactionBehavior && turnedAtWake )
    {
      LOG_INFO( "BehaviorReactToVoiceCommand.Thinking.TurnedAtWake",
                "Already turned toward the voice at the wake, no second turn after the intent" );
    }
    else if ( heardValidIntent && _iVars.reactionBehavior )
    {
      if( IsTurnEnabled() ) {
        const MicDirectionIndex triggerDirection = GetReactionDirection();
        PRINT_CH_INFO("Behaviors", "BehaviorReactToVoiceCommand.TransitionToThinking.ReactionDirection","%d",triggerDirection);
        _iVars.reactionBehavior->SetReactDirection( triggerDirection );

        LOG_DEBUG( "BehaviorReactToVoiceCommand.Thinking.SetReactionDirection",
                   "Setting reaction behavior direction to [%d]",
                   (int)triggerDirection );

        // allow the reaction to not want to run in certain directions/states
        if ( _iVars.reactionBehavior->WantsToBeActivated() )
        {
          DelegateIfInControl( _iVars.reactionBehavior.get(), &BehaviorReactToVoiceCommand::TransitionToIntentReceived );
        }
        else
        {
          LOG_DEBUG( "BehaviorReactToVoiceCommand.Thinking.ReactionDoesntWantToActivate",
                     "%s: intent reaction behavior '%s' doesn't want to activate",
                     GetDebugLabel().c_str(),
                     _iVars.reactionBehavior->GetDebugLabel().c_str() );
        }
      }
      else
      {
        LOG_DEBUG( "BehaviorReactToVoiceCommand.Thinking.TurnDisabled",
                   "Turn after intent is received has been disabled this tick, skipping" );
      }
    }

    if ( !IsControlDelegated() )
    {
      // handle intent now
      TransitionToIntentReceived();
    }
  };

  if ( IsListeningFeedbackEnabled() ) {
    // no listening loop to get out of: the listening pose, the '?' prompt or the thinking face were cancelled
    // with the delegates; the answer (or the give-up for silence) follows at once
    callback();
    return;
  }

  // we need to get out of our listening loop anim before we react
  if ( _dVars.persistent.forcedAnimListeningGetOut != AnimationTrigger::Count ) {
    DelegateIfInControl( new TriggerLiftSafeAnimationAction( _dVars.persistent.forcedAnimListeningGetOut ), callback );
  } else {
    DelegateIfInControl( new TriggerLiftSafeAnimationAction( _iVars.animListeningGetOut ), callback );
  }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BehaviorReactToVoiceCommand::TransitionToIntentReceived()
{
  _dVars.state = EState::IntentReceived;

  const auto status = kTriggerWord_FakeError ? EIntentStatus::Error : _dVars.intentStatus;

  switch ( status )
  {
    case EIntentStatus::IntentHeard: {
      // no animation for valid intent, go straight into the intent behavior
      LOG_DEBUG( "BehaviorReactToVoiceCommand.Intent.Heard", "Heard valid user intent, woot!" );

      // also reset the attention transfer counters since we got a valid intent
      GetBehaviorComp<AttentionTransferComponent>().ResetAttentionTransfer(AttentionTransferReason::UnmatchedIntent);
      GetBehaviorComp<AttentionTransferComponent>().ResetAttentionTransfer(AttentionTransferReason::NoWifi);
      GetBehaviorComp<AttentionTransferComponent>().ResetAttentionTransfer(AttentionTransferReason::NoCloudConnection);
      _iVars.cloudErrorTracker.Reset();
      _iVars.wifiErrorTracker.Reset();
      break;
    }

    case EIntentStatus::IntentUnknown: {
      LOG_DEBUG( "BehaviorReactToVoiceCommand.Intent.Unknown",
                 "Heard user intent but could not understand it" );
      GetBEI().GetMoodManager().TriggerEmotionEvent( "NoValidVoiceIntent" );
      if( _iVars.unmatchedIntentBehavior->WantsToBeActivated() ) {
        // this behavior will (should) interact directly with the attention transfer component
        DelegateIfInControl(_iVars.unmatchedIntentBehavior.get());
      }

      // even an unknown intent means we got something back from the cloud, so reset those transfer counters
      GetBehaviorComp<AttentionTransferComponent>().ResetAttentionTransfer(AttentionTransferReason::NoWifi);
      GetBehaviorComp<AttentionTransferComponent>().ResetAttentionTransfer(AttentionTransferReason::NoCloudConnection);
      break;
    }

    case EIntentStatus::SilenceTimeout: {
      LOG_DEBUG( "BehaviorReactToVoiceCommand.Intent.Silence",
                 "Heard silence from the user" );
      GetBEI().GetMoodManager().TriggerEmotionEvent( "NoValidVoiceIntent" );
      if( _iVars.silenceIntentBehavior->WantsToBeActivated() ) {
        DelegateIfInControl(_iVars.silenceIntentBehavior.get());
      }

      // even a silent intent means we got something back from the cloud, so reset those transfer
      // counters. This does not reset unmatched intent because the user may have difficulty speaking
      GetBehaviorComp<AttentionTransferComponent>().ResetAttentionTransfer(AttentionTransferReason::NoWifi);
      GetBehaviorComp<AttentionTransferComponent>().ResetAttentionTransfer(AttentionTransferReason::NoCloudConnection);
      break;
    }

    case EIntentStatus::NoIntentHeard:
    case EIntentStatus::Error:
      HandleStreamFailure();
      break;
  }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BehaviorReactToVoiceCommand::HandleStreamFailure()
{

  LOG_DEBUG( "BehaviorReactToVoiceCommand.Intent.Error",
             "Intent processing returned an error (or timeout)" );
  GetBEI().GetMoodManager().TriggerEmotionEvent( "NoValidVoiceIntent" );


  const bool updateNow = true;
  const bool osHasSSID = !OSState::getInstance()->GetSSID(updateNow).empty();

  // if console var faking is enabled, then check the console var to determine wifi rather than the true os
  // state
  const bool hasSSID = kTriggerWord_FakeError ? kTriggerWord_FakeError_HasWifi : osHasSSID;

  ICozmoBehaviorPtr errorBehavior;

  if( hasSSID ) {
    LOG_DEBUG( "BehaviorReactToVoiceCommand.Intent.Error.NoCloud",
               "has wifi, so error must be on the internet or cloud side" );

    _iVars.cloudErrorTracker.AddOccurrence();
    if( _iVars.cloudErrorHandle && _iVars.cloudErrorHandle->AreConditionsMet() )
    {
      if( ANKI_VERIFY(_iVars.noCloudBehavior != nullptr,
                      "BehaviorReactToVoiceCommand.Intent.Error.MissingNoCloudBehavior",
                      "Bad config, no behavior specified") ) {
        errorBehavior = _iVars.noCloudBehavior;
      }
    }
  }
  else {
    LOG_DEBUG( "BehaviorReactToVoiceCommand.Intent.Error.NoWifi",
               "no wifi SSID, error is local" );

    _iVars.wifiErrorTracker.AddOccurrence();
    if( _iVars.wifiErrorHandle && _iVars.wifiErrorHandle->AreConditionsMet() )
    {
      if( ANKI_VERIFY(_iVars.noWifiBehavior != nullptr,
                      "BehaviorReactToVoiceCommand.Intent.Error.noWifiBehavior",
                      "Bad config, no behavior specified") ) {
        errorBehavior = _iVars.noWifiBehavior;
      }
    }
  }

  if( errorBehavior ) {
    const MicDirectionIndex triggerDirection = GetReactionDirection();
    _iVars.reactionBehavior->SetReactDirection( triggerDirection );

    LOG_DEBUG( "BehaviorReactToVoiceCommand.Intent.Error.SetReactionDirection",
               "Setting reaction behavior direction to [%d]",
               (int)triggerDirection );

    auto errorBehaviorCallback = [this,errorBehavior](){
      if( ANKI_VERIFY( errorBehavior->WantsToBeActivated(),
                       "BehaviorReactToVoiceCommand.Intent.Error.BehaviorWontActivate",
                       "behavior '%s' doesn't want to be activated",
                       errorBehavior->GetDebugLabel().c_str()) ) {
        DelegateIfInControl(errorBehavior.get());
      }
    };

    // allow the reaction to not want to run in certain directions/states
    if ( _iVars.reactionBehavior->WantsToBeActivated() )
    {
      DelegateIfInControl( _iVars.reactionBehavior.get(), errorBehaviorCallback );
    }
    else
    {
      LOG_DEBUG( "BehaviorReactToVoiceCommand.Intent.Error",
                 "%s: intent reaction behavior '%s' doesn't want to activate (in case of intent error)",
                 GetDebugLabel().c_str(),
                 _iVars.reactionBehavior->GetDebugLabel().c_str() );
      errorBehaviorCallback();
    }
  }
  else {
    // not time to tell the user, just play the normal unheard animation
    DelegateIfInControl( new TriggerLiftSafeAnimationAction( AnimationTrigger::VC_IntentNeutral ) );
  }
}
  
// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BehaviorReactToVoiceCommand::SetListeningAnims(const AnimationTrigger& listeningLoop,
                                                    const AnimationTrigger& listeningGetOut)
{
  if ( IsActivated() ) {
    LOG_ERROR("BehaviorReactToVoiceCommand.CannotSetListeningAnimsWhileActivated",
              "Behavior %s is activated, cannot currently set new listening animations!",
              GetDebugLabel().c_str());
    return;
  }
  
  if (listeningLoop == AnimationTrigger::Count && listeningGetOut == AnimationTrigger::Count) {
    LOG_ERROR("BehaviorReactToVoiceCommand.CannotSetInvalidListeningAnims",
              "Behavior %s cannot set invalid listening animations!",
              GetDebugLabel().c_str());
  } else {
    _dVars.persistent.listeningAnimsResetQueued = false;
    _dVars.persistent.forcedAnimListeningLoop = listeningLoop;
    _dVars.persistent.forcedAnimListeningGetOut = listeningGetOut;
    if (listeningLoop != AnimationTrigger::Count) {
      LOG_DEBUG("BehaviorReactToVoiceCommand.SetListeningLoopAnim",
                "Behavior %s set listening loop animation to %s",
                GetDebugLabel().c_str(), AnimationTriggerToString(listeningLoop));
    }
    if (listeningGetOut != AnimationTrigger::Count) {
      LOG_DEBUG("BehaviorReactToVoiceCommand.SetListeningGetOutAnim",
                "Behavior %s set listening get-out animation to %s",
                GetDebugLabel().c_str(), AnimationTriggerToString(listeningGetOut));
    }
  }
}
  
// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BehaviorReactToVoiceCommand::ResetListeningAnimsToConfig()
{
  if ( IsActivated() ) {
    _dVars.persistent.listeningAnimsResetQueued = true;
    LOG_WARNING("BehaviorReactToVoiceCommand.CannotResetListeningAnimsWhileActivated",
                "Behavior %s is activated, cannot currently reset listening animations, but a"
                "reset is queued to happen after deactivation!",
                GetDebugLabel().c_str());
    return;
  }
  _dVars.persistent.listeningAnimsResetQueued = false;
  _dVars.persistent.forcedAnimListeningLoop = AnimationTrigger::Count;
  _dVars.persistent.forcedAnimListeningGetOut = AnimationTrigger::Count;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
bool BehaviorReactToVoiceCommand::IsTurnEnabled() const
{
  const EngineTimeStamp_t ts = BaseStationTimer::getInstance()->GetCurrentTimeStamp();
  const bool extnerallyDisabled = (ts == _dVars.timestampToDisableTurnFor);

  if( extnerallyDisabled ) {
    return false;
  }

  // special case for simple voice intents, don't turn if the flag is set
  UserIntentComponent& uic = GetBehaviorComp<UserIntentComponent>();
  UserIntent pendingIntent;
  if( uic.IsUserIntentPending(USER_INTENT(simple_voice_response), pendingIntent) ) {
    const MetaUserIntent_SimpleVoiceResponse& response = pendingIntent.Get_simple_voice_response();
    if( response.disable_wakeword_turn ) {
      return false;
    }
  }

  // otherwise, we're ok to turn
  return true;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
double BehaviorReactToVoiceCommand::GetStreamingDuration() const
{
  // our streaming duration is how long after streaming begins do we wait for an intent
  return kMaxStreamingDuration_s;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
double BehaviorReactToVoiceCommand::GetListeningTimeout() const
{
  double timeout = 0.0;

  const bool errorPending = GetBehaviorComp<UserIntentComponent>().WasUserIntentError();
  const bool streamingHasBegun = ( _dVars.streamingBeginTime > 0.0 );

  if ( errorPending || !streamingHasBegun || !_dVars.expectingStream )
  {
    // we haven't started streaming (or we won't), so timeout this much after we've been active
    timeout = ( GetTimeActivated_s() + kMinListeningTimeout_s );
  }
  else
  {
    // we're currently streaming, so timeout when we hit our streaming duration
    timeout = ( _dVars.streamingBeginTime + GetStreamingDuration() );
  }

  return timeout;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
// Listening feedback
// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

namespace {
  // The Microphones log channel is off on the robot: every feedback step also goes out as a DAS event, which
  // reaches its syslog ("@behavior.voice_command.listening_feedback"), like the cortex listening reaction's
  void LogListeningFeedback(const char* step, int value, double sinceActivation_s, int total)
  {
    LOG_INFO( "BehaviorReactToVoiceCommand.ListeningFeedback", "%s (%d/%d) at %.2fs",
              step, value, total, sinceActivation_s );
    DASMSG( behavior_voice_command_listening_feedback, "behavior.voice_command.listening_feedback",
            "A listening feedback step after the robot's own wake word" );
    DASMSG_SET( s1, step, "eye_color_applied, wake_turn, wake_turn_skipped, look_end, speech_heard, vad_sample, prompt, give_up, "
                          "give_up_result, thinking or eye_color_restored" );
    DASMSG_SET( i1, value, "The wake direction (0 in front, clockwise, 12 unknown) for the turn steps; voice "
                           "samples (10ms) in the window for speech_heard and vad_sample; hue x1000 for eye_color_applied; else 0" );
    DASMSG_SET( i2, static_cast<int>(sinceActivation_s * 1000.0), "Milliseconds since the wake reaction started" );
    DASMSG_SET( i3, total, "All samples (10ms) in the window for speech_heard and vad_sample; saturation x1000 for "
                       "eye_color_applied; else 0" );
    DASMSG_SEND();
  }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
BehaviorReactToVoiceCommand::VoiceDirectionVote
BehaviorReactToVoiceCommand::VoteVoiceDirection(const MicDirectionHistory& history,
                                                EngineTimeStamp_t end_ms,
                                                TimeStamp_t window_ms,
                                                MicDirectionConfidence minConfidence,
                                                bool voiceOnly)
{
  VoiceDirectionVote vote;
  if( window_ms == 0 ) {
    return vote;
  }

  const EngineTimeStamp_t begin_ms = ( (TimeStamp_t)end_ms > window_ms ) ? EngineTimeStamp_t((TimeStamp_t)end_ms - window_ms)
                                                                              : EngineTimeStamp_t(0);
  MicDirectionNodeList list = history.GetHistoryAtTime(end_ms, window_ms);

  // only count the part of the first and last nodes that falls inside the window
  std::vector<double> share(list.size(), 1.0);
  if( list.size() > 1 ) {
    const MicDirectionNode& front = list.front();
    if( (front.timestampBegin < begin_ms) && (front.timestampEnd > front.timestampBegin) ) {
      const double nodeDuration = TimeStamp_t(front.timestampEnd - front.timestampBegin);
      const double timeInNode = (front.timestampEnd > begin_ms) ? TimeStamp_t(front.timestampEnd - begin_ms) : 0;
      share.front() = timeInNode / nodeDuration;
    }

    const MicDirectionNode& back = list.back();
    if( (back.timestampEnd > end_ms) && (back.timestampEnd > back.timestampBegin) ) {
      const double nodeDuration = TimeStamp_t(back.timestampEnd - back.timestampBegin);
      const double timeInNode = (end_ms > back.timestampBegin) ? TimeStamp_t(end_ms - back.timestampBegin) : 0;
      share.back() = timeInNode / nodeDuration;
    }
  }

  std::array<uint32_t, kNumMicDirections + 1> counts{};
  std::array<double, kNumMicDirections + 1> confidenceSums{};
  size_t i = 0;
  for( const auto& node : list ) {
    const double nodeShare = share[i++];
    if( (node.directionIndex <= kMicDirectionUnknown) && (node.confidenceAvg > minConfidence) ) {
      // a direction run can mix voice and other sound; its voice samples are spread evenly over it
      const uint32_t counted = static_cast<uint32_t>( (voiceOnly ? node.vadCount : node.count) * nodeShare );
      counts[node.directionIndex] += counted;
      confidenceSums[node.directionIndex] += static_cast<double>(node.confidenceAvg) * counted;
    }
  }

  uint32_t bestCount = 0;
  for( MicDirectionIndex dir = kFirstMicDirectionIndex; dir < kNumMicDirections; ++dir ) {
    if( counts[dir] > bestCount ) {
      vote.direction = dir;
      bestCount = counts[dir];
    }
  }
  if( vote.direction != kMicDirectionUnknown ) {
    vote.samples = bestCount;
    vote.confidence = static_cast<MicDirectionConfidence>( confidenceSums[vote.direction] / bestCount );
  }
  return vote;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BehaviorReactToVoiceCommand::PickWakeDirection()
{
  const auto& fb = _iVars.feedback;
  const MicDirectionHistory& micHistory = GetBEI().GetMicComponent().GetMicDirectionHistory();
  const EngineTimeStamp_t now_ms = BaseStationTimer::getInstance()->GetCurrentTimeStamp();
  const TimeStamp_t window_ms = static_cast<TimeStamp_t>( std::max(0.0f, fb.wakeDirectionWindow_s) * 1000.0f );

  // the dominant direction over the wake phrase, counting only samples with voice activity.
  // Fallbacks: the trigger word's own direction, then the latest direction
  const auto vote = VoteVoiceDirection( micHistory, now_ms, window_ms, fb.minDirectionConfidence, true );
  const char* source = "wake_vote";
  MicDirectionIndex direction = vote.direction;
  if( kMicDirectionUnknown == direction ) {
    direction = _triggerDirection;
    source = "trigger";
  }
  if( kMicDirectionUnknown == direction ) {
    direction = GetDirectionFromMicHistory();
    source = "recent";
  }
  _dVars.feedback.wakeDirection = direction;

  LOG_INFO( "BehaviorReactToVoiceCommand.ListeningFeedback.WakeDirection",
            "direction %d from %s (vote %d confidence %d samples %u, trigger %d)",
            (int)direction, source, (int)vote.direction, (int)vote.confidence, vote.samples, (int)_triggerDirection );
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BehaviorReactToVoiceCommand::StartFeedbackLook()
{
  // the stock turn after "hey vector" (behaviorOnIntent, ReactToTriggerDirectionAwake), now before the user
  // speaks. It decides by itself not to turn on the charger, when picked up, or for voices from 11, 12 and 1 o'clock
  auto& turn = _iVars.reactionBehavior;
  const MicDirectionIndex direction = _dVars.feedback.wakeDirection;
  if( _iVars.feedback.turnAtWake && ( turn != nullptr ) && ( direction != kMicDirectionUnknown ) )
  {
    _dVars.feedback.phase = DynamicVariables::EFeedbackPhase::Turning;
    turn->SetReactDirection( direction );
    if( turn->WantsToBeActivated() &&
        DelegateIfInControl( turn.get(), &BehaviorReactToVoiceCommand::TransitionToFeedbackHold ) )
    {
      LogListeningFeedback( "wake_turn", (int)direction, GetActivatedDuration() );
      return;
    }
    turn->ClearReactDirection();
  }

  LogListeningFeedback( "wake_turn_skipped", (int)direction, GetActivatedDuration() );
  TransitionToFeedbackHold();
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BehaviorReactToVoiceCommand::TransitionToFeedbackHold()
{
  // end looking up at the user (the stock turns end with the head at 12-19 degrees), then hold the silent eyes
  _dVars.feedback.phase = DynamicVariables::EFeedbackPhase::Holding;
  IActionRunner* action = nullptr;
  if( _iVars.feedback.holdAnimName.empty() ) {
    action = new MoveHeadToAngleAction( DEG_TO_RAD(_iVars.feedback.headAngle_deg) );
  } else {
    action = new CompoundActionParallel({
      new MoveHeadToAngleAction( DEG_TO_RAD(_iVars.feedback.headAngle_deg) ),
      new PlayAnimationAction( _iVars.feedback.holdAnimName ),
    });
  }
  if( !DelegateIfInControl( action, &BehaviorReactToVoiceCommand::OnFeedbackLookEnd ) ) {
    OnFeedbackLookEnd();
  }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BehaviorReactToVoiceCommand::OnFeedbackLookEnd()
{
  // the look gesture is over (the user talks once the robot looks at them): the prompt and give-up timers start,
  // and only voice heard from now on counts as speech (the earcon, the get-in and the turn are behind)
  _dVars.feedback.phase = DynamicVariables::EFeedbackPhase::Listening;
  _dVars.feedback.lookEnd_s = BaseStationTimer::getInstance()->GetCurrentTimeInSecondsDouble();
  _dVars.feedback.lookEnd_ms = BaseStationTimer::getInstance()->GetCurrentTimeStamp();
  _dVars.feedback.lastVadSample_s = _dVars.feedback.lookEnd_s;
  LogListeningFeedback( "look_end", 0, GetActivatedDuration() );
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BehaviorReactToVoiceCommand::UpdateListeningFeedback()
{
  using EFeedbackPhase = DynamicVariables::EFeedbackPhase;
  auto& dfb = _dVars.feedback;
  const auto& fb = _iVars.feedback;
  if( dfb.phase != EFeedbackPhase::Listening ) {
    // turning or holding (the timers have not started), or thinking (waiting for the intent)
    return;
  }

  const double now_s = BaseStationTimer::getInstance()->GetCurrentTimeInSecondsDouble();

  // the stream closed (the cloud heard the end of speech) and no intent yet: the answer is on its way
  if( _dVars.streamingEndTime > 0.0 ) {
    if( dfb.streamEndSeen_s <= 0.0 ) {
      dfb.streamEndSeen_s = now_s;
    }
    if( dfb.speechHeard || ( now_s - dfb.streamEndSeen_s >= fb.thinkingDelay_s ) ) {
      TransitionToFeedbackThinking();
    }
    return;
  }

  const EngineTimeStamp_t now_ms = BaseStationTimer::getInstance()->GetCurrentTimeStamp();
  const MicDirectionHistory& micHistory = GetBEI().GetMicComponent().GetMicDirectionHistory();
  const TimeStamp_t window_ms = static_cast<TimeStamp_t>( std::max(0.0f, fb.speechWindow_s) * 1000.0f );
  const EngineTimeStamp_t windowBegin_ms = ( (TimeStamp_t)now_ms > window_ms )
                                           ? EngineTimeStamp_t((TimeStamp_t)now_ms - window_ms) : EngineTimeStamp_t(0);

  // the raw levels once a second while listening (the settle time included), to tune the rule live
  if( now_s - dfb.lastVadSample_s >= kFeedbackVadSamplePeriod_s ) {
    dfb.lastVadSample_s = now_s;
    const EngineTimeStamp_t sampleBegin_ms = std::max( windowBegin_ms, dfb.lookEnd_ms );
    const VoiceSamples levels = CountVoiceSamples( micHistory, sampleBegin_ms, now_ms );
    LogListeningFeedback( "vad_sample", (int)levels.voice, GetActivatedDuration(), (int)levels.total );
  }

  if( !dfb.speechHeard ) {
    // only voice after the settle time counts, and it must be a sustained run: at least minSpeech_s of voice
    // within the speech window (a window still shorter than minSpeech_s cannot reach it)
    const EngineTimeStamp_t settled_ms = EngineTimeStamp_t( (TimeStamp_t)dfb.lookEnd_ms +
      static_cast<TimeStamp_t>( std::max(0.0f, fb.settleAfterLook_s) * 1000.0f ) );
    if( now_ms > settled_ms ) {
      const EngineTimeStamp_t begin_ms = std::max( windowBegin_ms, settled_ms );
      const VoiceSamples samples = CountVoiceSamples( micHistory, begin_ms, now_ms );
      const uint32_t minVoice = static_cast<uint32_t>( std::max(0.0f, fb.minSpeech_s) * 1000.0f /
                                                       MicDirectionHistory::kTimePerDirectionUpdate_ms );
      if( ( samples.voice > 0 ) && ( samples.voice >= minVoice ) ) {
        // speech cancels the prompt and the give-up; the cloud ends listening from here (stream close, intent)
        dfb.speechHeard = true;
        LogListeningFeedback( "speech_heard", (int)samples.voice, GetActivatedDuration(), (int)samples.total );
        return;
      }
    }
  }

  if( dfb.speechHeard ) {
    return;
  }

  const double sinceLookEnd_s = now_s - dfb.lookEnd_s;
  if( sinceLookEnd_s >= fb.giveUpAfter_s ) {
    TransitionToFeedbackGiveUp();
  } else if( !dfb.promptShown && ( sinceLookEnd_s >= fb.promptAfter_s ) ) {
    ShowFeedbackPrompt();
  }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BehaviorReactToVoiceCommand::ShowFeedbackPrompt()
{
  // no speech yet: ask with the question mark clip and keep listening. The clip ends where the listening pose
  // started, so the eyes simply hold after it; the intent, the give-up or the thinking face cancel it
  _dVars.feedback.promptShown = true;
  LogListeningFeedback( "prompt", 0, GetActivatedDuration() );
  if( _iVars.feedback.promptAnimName.empty() ) {
    return;
  }
  CancelDelegates( false );
  DelegateIfInControl( new PlayAnimationAction( _iVars.feedback.promptAnimName ) );
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BehaviorReactToVoiceCommand::TransitionToFeedbackGiveUp()
{
  // still no speech: the robot stops listening. The user's eye color again, then the stock "never mind" gesture.
  // The stream may still be open: its late result (normally the cloud's noaudio) is consumed silently after this
  using EFeedbackPhase = DynamicVariables::EFeedbackPhase;
  _dVars.feedback.phase = EFeedbackPhase::GivingUp;
  CancelDelegates( false );
  RestoreListeningEyeColor();
  LogListeningFeedback( "give_up", 0, GetActivatedDuration() );
  if( GetBEI().HasMoodManager() ) {
    GetBEI().GetMoodManager().TriggerEmotionEvent( "NoValidVoiceIntent" );
  }

  auto onGestureEnd = [this]() {
    _dVars.feedback.phase = DynamicVariables::EFeedbackPhase::GaveUp;
    _dVars.feedback.giveUpEnd_s = BaseStationTimer::getInstance()->GetCurrentTimeInSecondsDouble();
  };
  if( _iVars.feedback.giveUpAnimName.empty() ||
      !DelegateIfInControl( new PlayAnimationAction( _iVars.feedback.giveUpAnimName ), onGestureEnd ) ) {
    onGestureEnd();
  }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
bool BehaviorReactToVoiceCommand::UpdateFeedbackGiveUp()
{
  using EFeedbackPhase = DynamicVariables::EFeedbackPhase;
  auto& dfb = _dVars.feedback;
  const bool givingUp = ( dfb.phase == EFeedbackPhase::GivingUp );
  if( !givingUp && ( dfb.phase != EFeedbackPhase::GaveUp ) ) {
    return false;
  }

  UserIntentComponent& uic = GetBehaviorComp<UserIntentComponent>();
  static const UserIntentTag silence = USER_INTENT(silence);
  if( uic.IsUserIntentPending( silence ) ) {
    // the cloud agrees there was no speech: already shown, consume it so it never reads as unclaimed
    uic.DropUserIntent( silence );
    dfb.resultAfterGiveUp = true;
    LogListeningFeedback( "give_up_result", 0, GetActivatedDuration() );
  } else if( uic.IsAnyUserIntentPending() ) {
    // the cloud heard something the robot's VAD missed: handle it like any intent (cuts the gesture)
    LogListeningFeedback( "give_up_result", 1, GetActivatedDuration() );
    CancelDelegates( false );
    StopListening();
    return true;
  } else if( uic.WasUserIntentError() ) {
    uic.ResetUserIntentError();
    dfb.resultAfterGiveUp = true;
    LogListeningFeedback( "give_up_result", 2, GetActivatedDuration() );
  }

  if( !givingUp ) {
    const double now_s = BaseStationTimer::getInstance()->GetCurrentTimeInSecondsDouble();
    if( dfb.resultAfterGiveUp || ( now_s - dfb.giveUpEnd_s >= _iVars.feedback.giveUpWait_s ) ) {
      CancelSelf();
    }
  }
  return true;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BehaviorReactToVoiceCommand::TransitionToFeedbackThinking()
{
  // the user stopped talking and the cloud is preparing the answer: the user's eye color again and the stock
  // "looking it up" face until the intent arrives (or the stock listening timeout)
  _dVars.feedback.phase = DynamicVariables::EFeedbackPhase::Thinking;
  CancelDelegates( false );
  RestoreListeningEyeColor();
  LogListeningFeedback( "thinking", _dVars.feedback.speechHeard ? 1 : 0, GetActivatedDuration() );

  if( _iVars.feedback.thinkingGetInAnimName.empty() ||
      !DelegateIfInControl( new PlayAnimationAction( _iVars.feedback.thinkingGetInAnimName ),
                            &BehaviorReactToVoiceCommand::TransitionToFeedbackThinkingLoop ) ) {
    TransitionToFeedbackThinkingLoop();
  }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BehaviorReactToVoiceCommand::TransitionToFeedbackThinkingLoop()
{
  if( _iVars.feedback.thinkingLoopAnimName.empty() ) {
    return;
  }
  // loops until the intent (or a timeout) cancels it
  DelegateIfInControl( new PlayAnimationAction( _iVars.feedback.thinkingLoopAnimName, 0, true,
                                                (u8)AnimTrackFlag::NO_TRACKS,
                                                PlayAnimationAction::GetInfiniteTimeoutInSeconds() ) );
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BehaviorReactToVoiceCommand::RestoreListeningEyeColor()
{
  if( _showingListeningEyeColor ) {
    GetBEI().GetSettingsManager().RestoreEyeColorSetting();
    _showingListeningEyeColor = false;
    LogListeningFeedback( "eye_color_restored", 0, IsActivated() ? GetActivatedDuration() : 0.0 );
  }
}

} // namespace Vector
} // namespace Anki
