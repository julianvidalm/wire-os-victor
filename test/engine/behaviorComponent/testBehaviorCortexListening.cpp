/**
 * File: testBehaviorCortexListening.cpp
 *
 * Created: 2026-10-19
 *
 * Description: Unit tests for the PC cortex listening behavior: it starts on cortex_listening, holds while
 * active, and ends on the next intent (leaving it pending for its own behavior), on cortex_listening_end,
 * after a give-up gesture, or on its timeout, without leaving a post-behavior suggestion behind; a prompt shows a
 * question mark and keeps listening; cortex_thinking switches to a thinking face with the user's eye color until
 * the answer, cortex_listening_end or a longer timeout; the listening eye color is undone on every exit. It picks the voice direction to turn
 * toward from the mic direction history over the wake phrase.
 *
 **/

#include "gtest/gtest.h"

#include "clad/types/behaviorComponent/behaviorClasses.h"
#include "clad/types/behaviorComponent/behaviorIDs.h"
#include "coretech/common/engine/utils/timer.h"
#include "engine/aiComponent/aiWhiteboard.h"
#include "engine/aiComponent/behaviorComponent/behaviorContainer.h"
#include "engine/aiComponent/behaviorComponent/behaviors/iCozmoBehavior.h"
#include "engine/aiComponent/behaviorComponent/behaviors/reactions/behaviorCortexListening.h"
#include "engine/aiComponent/behaviorComponent/userIntentComponent.h"
#include "engine/aiComponent/behaviorComponent/userIntents.h"
#include "engine/components/mics/micComponent.h"
#include "engine/components/mics/micDirectionHistory.h"
#include "engine/robot.h"
#include "test/engine/behaviorComponent/testBehaviorFramework.h"

using namespace Anki;
using namespace Anki::Vector;

namespace {

using EndReason = BehaviorCortexListening::EndReason;

class CortexListeningTest : public ::testing::Test
{
protected:
  void SetUp() override
  {
    _time_s = 5.0f;
    BaseStationTimer::getInstance()->UpdateTime( Util::SecToNanoSec(_time_s) );

    _tbf.InitializeStandardBehaviorComponent();
    _tbf.GetBehaviorContainer().FindBehaviorByIDAndDowncast( BEHAVIOR_ID(CortexListening),
                                                              BEHAVIOR_CLASS(CortexListening),
                                                              _behavior );
    ASSERT_NE( _behavior, nullptr );

    _tbf.SetBehaviorStackByName( "highLevelAI_stack" );
    Tick(3);
  }

  UserIntentComponent& UIC()
  {
    return _tbf.GetBehaviorExternalInterface().GetAIComponent()
               .GetComponent<BehaviorComponent>().GetComponent<UserIntentComponent>();
  }

  void Tick(unsigned int numTimes = 1, float dt_s = 0.05f)
  {
    AICompMap emptyMap;
    for( unsigned int i=0; i<numTimes; ++i ) {
      _time_s += dt_s;
      BaseStationTimer::getInstance()->UpdateTime( Util::SecToNanoSec(_time_s) );
      _tbf.GetBehaviorComponent().UpdateDependent(emptyMap);
      IncrementBaseStationTimerTicks();
    }
  }

  bool InStack(BehaviorID id)
  {
    for( const auto* elem : _tbf.GetCurrentBehaviorStack() ) {
      const auto* beh = dynamic_cast<const ICozmoBehavior*>(elem);
      if( beh != nullptr && beh->GetID() == id ) {
        return true;
      }
    }
    return false;
  }

  void StartListening()
  {
    UIC().DevSetUserIntentPending( USER_INTENT(cortex_listening), UserIntentSource::Voice );
    Tick(2);
    ASSERT_TRUE( _behavior->IsActivated() );
    ASSERT_FALSE( UIC().IsAnyUserIntentPending() );
    ASSERT_TRUE( _behavior->IsShowingListeningEyeColor() );
    // no other intent arriving: it keeps holding
    Tick(20);
    ASSERT_TRUE( _behavior->IsActivated() );
    ASSERT_TRUE( InStack(BEHAVIOR_ID(CortexListening)) );
  }

  bool HasAnyPostBehaviorSuggestion()
  {
    auto& whiteboard = _tbf.GetAIComponent().GetComponent<AIWhiteboard>();
    size_t tick = 0;
    for( uint8_t i=0; i<PostBehaviorSuggestionsNumEntries; ++i ) {
      if( whiteboard.GetPostBehaviorSuggestion( static_cast<PostBehaviorSuggestions>(i), tick ) ) {
        return true;
      }
    }
    return false;
  }

  // one mic direction sample every 10ms over [begin_ms, end_ms)
  static void AddSamples(MicDirectionHistory& history, TimeStamp_t begin_ms, TimeStamp_t end_ms,
                         MicDirectionIndex dir, MicDirectionConfidence conf)
  {
    for( TimeStamp_t t = begin_ms; t < end_ms; t += 10 ) {
      history.AddDirectionSample(t, true, dir, conf, dir);
    }
  }

  TestBehaviorFramework _tbf;
  std::shared_ptr<BehaviorCortexListening> _behavior;
  float _time_s = 0.0f;
};

TEST_F(CortexListeningTest, EndsOnNextIntentAndLeavesItPending)
{
  _tbf.GetAIComponent().GetComponent<AIWhiteboard>().ClearPostBehaviorSuggestions();
  StartListening();

  UIC().DevSetUserIntentPending( USER_INTENT(imperative_lookatme), UserIntentSource::Voice );
  Tick();
  EXPECT_FALSE( _behavior->IsActivated() );
  EXPECT_EQ( _behavior->GetLastEndReason(), EndReason::NextIntent );
  EXPECT_FALSE( _behavior->IsShowingListeningEyeColor() );

  // the answer is claimed by its own behavior right after, not dropped
  for( int i=0; i<3 && UIC().IsUserIntentPending(USER_INTENT(imperative_lookatme)); ++i ) {
    Tick();
  }
  EXPECT_FALSE( UIC().IsUserIntentPending(USER_INTENT(imperative_lookatme)) );
  EXPECT_FALSE( UIC().WasUserIntentUnclaimed() );
  EXPECT_TRUE( InStack(BEHAVIOR_ID(LookAtMeVoiceCommand)) );
}

TEST_F(CortexListeningTest, ListeningEndPlaysTheGiveUpGestureThenEnds)
{
  _tbf.GetAIComponent().GetComponent<AIWhiteboard>().ClearPostBehaviorSuggestions();
  StartListening();

  UIC().DevSetUserIntentPending( USER_INTENT(cortex_listening_end), UserIntentSource::Voice );
  Tick();
  // consumed; the gesture plays with the user's eye color back
  EXPECT_FALSE( UIC().IsAnyUserIntentPending() );
  EXPECT_FALSE( _behavior->IsShowingListeningEyeColor() );
  if( _behavior->IsActivated() ) {
    EXPECT_TRUE( _behavior->IsGivingUp() );
    // a late prompt or end during the gesture changes nothing
    UIC().DevSetUserIntentPending( USER_INTENT(cortex_listening_prompt), UserIntentSource::Voice );
    Tick();
    EXPECT_EQ( _behavior->GetNumPromptsShown(), 0 );
    EXPECT_FALSE( UIC().IsAnyUserIntentPending() );
    // the gesture never holds listening for more than 3s
    for( int i=0; i<40 && _behavior->IsActivated(); ++i ) {
      Tick(1, 0.1f);
    }
  }
  EXPECT_FALSE( _behavior->IsActivated() );
  EXPECT_EQ( _behavior->GetLastEndReason(), EndReason::ListeningEnd );
  EXPECT_FALSE( UIC().IsAnyUserIntentPending() );
  EXPECT_FALSE( HasAnyPostBehaviorSuggestion() );

  Tick(5);
  EXPECT_FALSE( UIC().WasUserIntentUnclaimed() );
  EXPECT_TRUE( InStack(BEHAVIOR_ID(HighLevelAI)) );
}

TEST_F(CortexListeningTest, EndsOnTimeout)
{
  _tbf.GetAIComponent().GetComponent<AIWhiteboard>().ClearPostBehaviorSuggestions();
  StartListening();

  // the default timeout is 15s; StartListening already spent about 1.1s
  Tick(1, 13.0f);
  EXPECT_TRUE( _behavior->IsActivated() );
  Tick(1, 1.5f);
  EXPECT_FALSE( _behavior->IsActivated() );
  EXPECT_EQ( _behavior->GetLastEndReason(), EndReason::Timeout );
  EXPECT_FALSE( _behavior->IsShowingListeningEyeColor() );
  EXPECT_FALSE( HasAnyPostBehaviorSuggestion() );
}

TEST_F(CortexListeningTest, RepeatedWakeRestartsTimeout)
{
  StartListening();
  Tick(1, 13.0f);
  UIC().DevSetUserIntentPending( USER_INTENT(cortex_listening), UserIntentSource::Voice );
  Tick();
  EXPECT_TRUE( _behavior->IsActivated() );
  EXPECT_FALSE( UIC().IsAnyUserIntentPending() );
  Tick(1, 5.0f);
  EXPECT_TRUE( _behavior->IsActivated() );
}

TEST_F(CortexListeningTest, PromptKeepsListeningAndRestartsTimeout)
{
  StartListening();
  Tick(1, 13.0f);
  UIC().DevSetUserIntentPending( USER_INTENT(cortex_listening_prompt), UserIntentSource::Voice );
  Tick();
  EXPECT_TRUE( _behavior->IsActivated() );
  EXPECT_FALSE( UIC().IsAnyUserIntentPending() );
  EXPECT_FALSE( UIC().WasUserIntentUnclaimed() );
  EXPECT_EQ( _behavior->GetNumPromptsShown(), 1 );
  EXPECT_FALSE( _behavior->IsGivingUp() );
  EXPECT_TRUE( _behavior->IsShowingListeningEyeColor() );

  // fresh safety timeout from the prompt
  Tick(1, 5.0f);
  EXPECT_TRUE( _behavior->IsActivated() );
  Tick(1, 11.0f);
  EXPECT_FALSE( _behavior->IsActivated() );
  EXPECT_EQ( _behavior->GetLastEndReason(), EndReason::Timeout );
  EXPECT_FALSE( _behavior->IsShowingListeningEyeColor() );
}

TEST_F(CortexListeningTest, AnswerDuringPromptEndsAndIsClaimedByItsBehavior)
{
  StartListening();
  UIC().DevSetUserIntentPending( USER_INTENT(cortex_listening_prompt), UserIntentSource::Voice );
  Tick();
  ASSERT_EQ( _behavior->GetNumPromptsShown(), 1 );

  UIC().DevSetUserIntentPending( USER_INTENT(imperative_lookatme), UserIntentSource::Voice );
  Tick();
  EXPECT_FALSE( _behavior->IsActivated() );
  EXPECT_EQ( _behavior->GetLastEndReason(), EndReason::NextIntent );
  EXPECT_FALSE( _behavior->IsShowingListeningEyeColor() );
  for( int i=0; i<3 && UIC().IsUserIntentPending(USER_INTENT(imperative_lookatme)); ++i ) {
    Tick();
  }
  EXPECT_FALSE( UIC().WasUserIntentUnclaimed() );
  EXPECT_TRUE( InStack(BEHAVIOR_ID(LookAtMeVoiceCommand)) );
}

TEST_F(CortexListeningTest, AnswerDuringGiveUpEndsAndIsClaimedByItsBehavior)
{
  StartListening();
  UIC().DevSetUserIntentPending( USER_INTENT(cortex_listening_end), UserIntentSource::Voice );
  Tick();
  if( !_behavior->IsActivated() ) {
    // the gesture already finished in this environment; nothing to interrupt
    return;
  }
  ASSERT_TRUE( _behavior->IsGivingUp() );

  UIC().DevSetUserIntentPending( USER_INTENT(imperative_lookatme), UserIntentSource::Voice );
  Tick();
  EXPECT_FALSE( _behavior->IsActivated() );
  EXPECT_EQ( _behavior->GetLastEndReason(), EndReason::NextIntent );
  for( int i=0; i<3 && UIC().IsUserIntentPending(USER_INTENT(imperative_lookatme)); ++i ) {
    Tick();
  }
  EXPECT_FALSE( UIC().WasUserIntentUnclaimed() );
}

TEST_F(CortexListeningTest, InterruptionRestoresTheEyeColor)
{
  StartListening();
  // a higher priority behavior takes over the stack
  _tbf.SetBehaviorStackByName( "highLevelAI_stack" );
  Tick();
  EXPECT_FALSE( _behavior->IsActivated() );
  EXPECT_FALSE( _behavior->IsShowingListeningEyeColor() );
}

TEST_F(CortexListeningTest, StrayPromptIsConsumedSilently)
{
  ASSERT_FALSE( _behavior->IsActivated() );
  UIC().DevSetUserIntentPending( USER_INTENT(cortex_listening_prompt), UserIntentSource::Voice );
  Tick(3);
  EXPECT_FALSE( UIC().IsAnyUserIntentPending() );
  EXPECT_FALSE( UIC().WasUserIntentUnclaimed() );
  EXPECT_FALSE( _behavior->IsActivated() );
  EXPECT_EQ( _behavior->GetLastEndReason(), EndReason::StrayPrompt );
  EXPECT_FALSE( _behavior->IsShowingListeningEyeColor() );
}

TEST_F(CortexListeningTest, StrayListeningEndIsConsumedSilently)
{
  ASSERT_FALSE( _behavior->IsActivated() );
  UIC().DevSetUserIntentPending( USER_INTENT(cortex_listening_end), UserIntentSource::Voice );
  Tick(3);
  EXPECT_FALSE( UIC().IsAnyUserIntentPending() );
  EXPECT_FALSE( UIC().WasUserIntentUnclaimed() );
  EXPECT_FALSE( _behavior->IsActivated() );
  EXPECT_EQ( _behavior->GetLastEndReason(), EndReason::StrayEnd );
}

TEST_F(CortexListeningTest, ThinkingRestoresTheEyeColorAndHoldsUntilTheAnswer)
{
  _tbf.GetAIComponent().GetComponent<AIWhiteboard>().ClearPostBehaviorSuggestions();
  StartListening();

  UIC().DevSetUserIntentPending( USER_INTENT(cortex_thinking), UserIntentSource::Voice );
  Tick();
  EXPECT_TRUE( _behavior->IsActivated() );
  EXPECT_TRUE( _behavior->IsThinking() );
  EXPECT_FALSE( UIC().IsAnyUserIntentPending() );
  EXPECT_FALSE( _behavior->IsShowingListeningEyeColor() );

  // a late prompt changes nothing; thinking outlasts the listening timeout (15s) but not its own (20s)
  UIC().DevSetUserIntentPending( USER_INTENT(cortex_listening_prompt), UserIntentSource::Voice );
  Tick();
  EXPECT_EQ( _behavior->GetNumPromptsShown(), 0 );
  EXPECT_FALSE( UIC().IsAnyUserIntentPending() );
  Tick(1, 17.0f);
  EXPECT_TRUE( _behavior->IsActivated() );
  EXPECT_TRUE( _behavior->IsThinking() );

  // the answer ends it and is claimed by its own behavior
  UIC().DevSetUserIntentPending( USER_INTENT(imperative_lookatme), UserIntentSource::Voice );
  Tick();
  EXPECT_FALSE( _behavior->IsActivated() );
  EXPECT_EQ( _behavior->GetLastEndReason(), EndReason::NextIntent );
  for( int i=0; i<3 && UIC().IsUserIntentPending(USER_INTENT(imperative_lookatme)); ++i ) {
    Tick();
  }
  EXPECT_FALSE( UIC().WasUserIntentUnclaimed() );
  EXPECT_TRUE( InStack(BEHAVIOR_ID(LookAtMeVoiceCommand)) );
  EXPECT_FALSE( HasAnyPostBehaviorSuggestion() );
}

TEST_F(CortexListeningTest, ThinkingEndsOnItsTimeout)
{
  StartListening();
  UIC().DevSetUserIntentPending( USER_INTENT(cortex_thinking), UserIntentSource::Voice );
  Tick();
  ASSERT_TRUE( _behavior->IsThinking() );
  Tick(1, 19.0f);
  EXPECT_TRUE( _behavior->IsActivated() );
  Tick(1, 1.5f);
  EXPECT_FALSE( _behavior->IsActivated() );
  EXPECT_EQ( _behavior->GetLastEndReason(), EndReason::Timeout );
  EXPECT_FALSE( _behavior->IsShowingListeningEyeColor() );
}

TEST_F(CortexListeningTest, ListeningEndWhileThinkingPlaysTheGiveUpGesture)
{
  StartListening();
  UIC().DevSetUserIntentPending( USER_INTENT(cortex_thinking), UserIntentSource::Voice );
  Tick();
  ASSERT_TRUE( _behavior->IsThinking() );

  UIC().DevSetUserIntentPending( USER_INTENT(cortex_listening_end), UserIntentSource::Voice );
  Tick();
  EXPECT_FALSE( UIC().IsAnyUserIntentPending() );
  if( _behavior->IsActivated() ) {
    EXPECT_TRUE( _behavior->IsGivingUp() );
    for( int i=0; i<40 && _behavior->IsActivated(); ++i ) {
      Tick(1, 0.1f);
    }
  }
  EXPECT_FALSE( _behavior->IsActivated() );
  EXPECT_EQ( _behavior->GetLastEndReason(), EndReason::ListeningEnd );
  Tick(5);
  EXPECT_FALSE( UIC().WasUserIntentUnclaimed() );
}

TEST_F(CortexListeningTest, NewWakeWhileThinkingListensAgain)
{
  StartListening();
  UIC().DevSetUserIntentPending( USER_INTENT(cortex_thinking), UserIntentSource::Voice );
  Tick();
  ASSERT_TRUE( _behavior->IsThinking() );

  UIC().DevSetUserIntentPending( USER_INTENT(cortex_listening), UserIntentSource::Voice );
  Tick();
  EXPECT_TRUE( _behavior->IsActivated() );
  EXPECT_FALSE( _behavior->IsThinking() );
  EXPECT_TRUE( _behavior->IsShowingListeningEyeColor() );
  EXPECT_FALSE( UIC().IsAnyUserIntentPending() );
}

TEST_F(CortexListeningTest, StrayThinkingIsConsumedSilently)
{
  ASSERT_FALSE( _behavior->IsActivated() );
  UIC().DevSetUserIntentPending( USER_INTENT(cortex_thinking), UserIntentSource::Voice );
  Tick(3);
  EXPECT_FALSE( UIC().IsAnyUserIntentPending() );
  EXPECT_FALSE( UIC().WasUserIntentUnclaimed() );
  EXPECT_FALSE( _behavior->IsActivated() );
  EXPECT_EQ( _behavior->GetLastEndReason(), EndReason::StrayThinking );
  EXPECT_FALSE( _behavior->IsShowingListeningEyeColor() );
}

TEST(CortexListeningVoiceDirection, PicksTheDominantConfidentDirection)
{
  MicDirectionHistory history{};
  // the wake phrase mostly from 3 o'clock, a shorter burst from 9 o'clock
  for( TimeStamp_t t = 1000; t < 1800; t += 10 ) {
    const MicDirectionIndex dir = ( t < 1200 ) ? 9 : 3;
    history.AddDirectionSample(t, true, dir, 2000, dir);
  }
  EXPECT_EQ( BehaviorCortexListening::PickVoiceDirection(history, 2000, 2000, 500), 3 );
}

TEST(CortexListeningVoiceDirection, IgnoresLowConfidenceAndUnknown)
{
  MicDirectionHistory history{};
  for( TimeStamp_t t = 1000; t < 1800; t += 10 ) {
    // a lot of low confidence noise from 6 o'clock and unknown direction, a little confident voice from 2
    const MicDirectionIndex dir = ( t < 1600 ) ? 6 : ( t < 1700 ? kMicDirectionUnknown : 2 );
    const MicDirectionConfidence conf = ( t < 1600 ) ? 100 : 2000;
    history.AddDirectionSample(t, true, dir, conf, dir);
  }
  EXPECT_EQ( BehaviorCortexListening::PickVoiceDirection(history, 2000, 2000, 500), 2 );
}

TEST(CortexListeningVoiceDirection, UnknownWhenNothingIsConfident)
{
  MicDirectionHistory history{};
  for( TimeStamp_t t = 1000; t < 1800; t += 10 ) {
    history.AddDirectionSample(t, true, 4, 100, 4);
  }
  EXPECT_EQ( BehaviorCortexListening::PickVoiceDirection(history, 2000, 2000, 500), kMicDirectionUnknown );
  EXPECT_EQ( BehaviorCortexListening::PickVoiceDirection(MicDirectionHistory{}, 2000, 2000, 500),
             kMicDirectionUnknown );
}

TEST(CortexListeningVoiceDirection, OnlyCountsTheWindow)
{
  MicDirectionHistory history{};
  // a long confident sound from 8 o'clock well before the wake phrase, then the phrase from 4 o'clock
  for( TimeStamp_t t = 1000; t < 6000; t += 10 ) {
    const MicDirectionIndex dir = ( t < 5000 ) ? 8 : 4;
    history.AddDirectionSample(t, true, dir, 2000, dir);
  }
  EXPECT_EQ( BehaviorCortexListening::PickVoiceDirection(history, 6000, 1500, 500), 4 );
}

TEST_F(CortexListeningTest, PicksTheWakePhraseDirectionOnActivation)
{
  // the wake phrase came from 3 o'clock during the last second
  const TimeStamp_t now_ms = BaseStationTimer::getInstance()->GetCurrentTimeStamp();
  auto& history = _tbf.GetRobot().GetMicComponent().GetMicDirectionHistory();
  AddSamples( history, now_ms - 1000, now_ms - 200, 3, 2000 );
  AddSamples( history, now_ms - 200, now_ms, kMicDirectionUnknown, 0 );

  StartListening();
  EXPECT_EQ( _behavior->GetLastVoiceDirection(), 3 );

  // the gesture does not change how listening ends
  UIC().DevSetUserIntentPending( USER_INTENT(imperative_lookatme), UserIntentSource::Voice );
  Tick();
  EXPECT_FALSE( _behavior->IsActivated() );
  EXPECT_EQ( _behavior->GetLastEndReason(), EndReason::NextIntent );
}

TEST_F(CortexListeningTest, NoConfidentDirectionStillListens)
{
  StartListening();
  EXPECT_EQ( _behavior->GetLastVoiceDirection(), kMicDirectionUnknown );

  UIC().DevSetUserIntentPending( USER_INTENT(imperative_lookatme), UserIntentSource::Voice );
  Tick();
  EXPECT_FALSE( _behavior->IsActivated() );
  EXPECT_EQ( _behavior->GetLastEndReason(), EndReason::NextIntent );
}

} // namespace
