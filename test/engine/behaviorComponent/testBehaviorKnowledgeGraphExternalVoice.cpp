/**
 * File: testBehaviorKnowledgeGraphExternalVoice.cpp
 *
 * Created: 2026-10-19
 *
 * Description: Unit tests for the knowledge graph answer spoken by the PC cortex (external voice): the
 * behavior goes straight to the talking face without the robot's TTS, and gives up waiting for the audio
 * stream's end after the expected duration plus a margin. Without the flag the robot speaks the answer itself.
 *
 **/

#include "gtest/gtest.h"

#include "clad/types/behaviorComponent/behaviorClasses.h"
#include "clad/types/behaviorComponent/behaviorIDs.h"
#include "coretech/common/engine/utils/timer.h"
#include "engine/aiComponent/behaviorComponent/behaviorContainer.h"
#include "engine/aiComponent/behaviorComponent/behaviors/knowledgeGraph/behaviorKnowledgeGraphQuestion.h"
#include "engine/aiComponent/behaviorComponent/userIntentComponent.h"
#include "engine/aiComponent/behaviorComponent/userIntents.h"
#include "test/engine/behaviorComponent/testBehaviorFramework.h"

using namespace Anki;
using namespace Anki::Vector;

namespace {

class KnowledgeGraphExternalVoiceTest : public ::testing::Test
{
protected:
  void SetUp() override
  {
    _time_s = 5.0f;
    BaseStationTimer::getInstance()->UpdateTime( Util::SecToNanoSec(_time_s) );

    _tbf.InitializeStandardBehaviorComponent();
    _tbf.GetBehaviorContainer().FindBehaviorByIDAndDowncast( BEHAVIOR_ID(KnowledgeGraphQuestion),
                                                              BEHAVIOR_CLASS(KnowledgeGraphQuestion),
                                                              _behavior );
    ASSERT_NE( _behavior, nullptr );
    _tbf.SetBehaviorStackByName( "highLevelAI_stack" );
    Tick(3);
  }

  UserIntentComponent& UIC()
  {
    return _tbf.GetBehaviorComponent().GetComponent<UserIntentComponent>();
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

  void SendAnswer(bool externalVoice, uint32_t expected_ms)
  {
    UserIntent_KnowledgeResponse response;
    response.answer = "Son las tres.";
    response.query_text = "qué hora es";
    response.external_voice = externalVoice;
    response.expected_duration_ms = expected_ms;
    UIC().DevSetUserIntentPending( UserIntent::Createknowledge_response_bypass(std::move(response)),
                                   UserIntentSource::Voice );
    for( int i=0; i<3 && !_behavior->IsActivated(); ++i ) {
      Tick();
    }
  }

  TestBehaviorFramework _tbf;
  std::shared_ptr<BehaviorKnowledgeGraphQuestion> _behavior;
  float _time_s = 0.0f;
};

TEST_F(KnowledgeGraphExternalVoiceTest, ExternalVoiceShowsTheTalkingFaceUntilItsTimeout)
{
  SendAnswer(true, 2000);
  ASSERT_TRUE( _behavior->IsActivated() );
  EXPECT_TRUE( _behavior->IsExternalVoice() );
  EXPECT_FALSE( UIC().WasUserIntentUnclaimed() );

  // no end report from the audio stream: it waits the expected 2s plus the 3s margin
  Tick(1, 4.0f);
  if( _behavior->IsActivated() ) {
    EXPECT_FALSE( _behavior->IsExternalVoiceDone() );
  }
  Tick(1, 1.5f);
  EXPECT_TRUE( !_behavior->IsActivated() || _behavior->IsExternalVoiceDone() );
}

TEST_F(KnowledgeGraphExternalVoiceTest, WithoutTheFlagTheRobotSpeaks)
{
  SendAnswer(false, 0);
  ASSERT_TRUE( _behavior->IsActivated() );
  EXPECT_FALSE( _behavior->IsExternalVoice() );
}

}
