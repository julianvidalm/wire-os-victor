/**
 * File: testBehaviorCortexListening.cpp
 *
 * Created: 2026-10-19
 *
 * Description: Unit tests for the PC cortex listening behavior: it starts on cortex_listening, holds while
 * active, and ends on the next intent (leaving it pending for its own behavior), on cortex_listening_end,
 * or on its timeout, without leaving a post-behavior suggestion behind.
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

  // the answer is claimed by its own behavior right after, not dropped
  for( int i=0; i<3 && UIC().IsUserIntentPending(USER_INTENT(imperative_lookatme)); ++i ) {
    Tick();
  }
  EXPECT_FALSE( UIC().IsUserIntentPending(USER_INTENT(imperative_lookatme)) );
  EXPECT_FALSE( UIC().WasUserIntentUnclaimed() );
  EXPECT_TRUE( InStack(BEHAVIOR_ID(LookAtMeVoiceCommand)) );
}

TEST_F(CortexListeningTest, EndsSilentlyOnListeningEnd)
{
  _tbf.GetAIComponent().GetComponent<AIWhiteboard>().ClearPostBehaviorSuggestions();
  StartListening();

  UIC().DevSetUserIntentPending( USER_INTENT(cortex_listening_end), UserIntentSource::Voice );
  Tick();
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

  // the default timeout is 10s; StartListening already spent about 1.1s
  Tick(1, 8.0f);
  EXPECT_TRUE( _behavior->IsActivated() );
  Tick(1, 1.5f);
  EXPECT_FALSE( _behavior->IsActivated() );
  EXPECT_EQ( _behavior->GetLastEndReason(), EndReason::Timeout );
  EXPECT_FALSE( HasAnyPostBehaviorSuggestion() );
}

TEST_F(CortexListeningTest, RepeatedWakeRestartsTimeout)
{
  StartListening();
  Tick(1, 8.0f);
  UIC().DevSetUserIntentPending( USER_INTENT(cortex_listening), UserIntentSource::Voice );
  Tick();
  EXPECT_TRUE( _behavior->IsActivated() );
  EXPECT_FALSE( UIC().IsAnyUserIntentPending() );
  Tick(1, 5.0f);
  EXPECT_TRUE( _behavior->IsActivated() );
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

} // namespace
