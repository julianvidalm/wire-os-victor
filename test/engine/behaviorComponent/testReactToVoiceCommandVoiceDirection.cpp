/**
 * File: testReactToVoiceCommandVoiceDirection.cpp
 *
 * Description: Unit tests for the voice direction vote the listening feedback of ReactToVoiceCommand turns
 * toward at the wake (BehaviorReactToVoiceCommand::VoteVoiceDirection), and for the robot time mapping of the
 * mic direction history.
 *
 **/

#include "gtest/gtest.h"

#include "coretech/common/engine/utils/timer.h"
#include "engine/aiComponent/behaviorComponent/behaviors/reactions/behaviorReactToVoiceCommand.h"
#include "engine/components/mics/micDirectionHistory.h"
#include "engine/robot.h"
#include "util/helpers/templateHelpers.h"

using namespace Anki;
using namespace Anki::Vector;

namespace {

// the stock vote (every sample counted)
MicDirectionIndex PickVoiceDirection(const MicDirectionHistory& history, EngineTimeStamp_t end_ms,
                                     TimeStamp_t window_ms, MicDirectionConfidence minConfidence)
{
  return BehaviorReactToVoiceCommand::VoteVoiceDirection(history, end_ms, window_ms, minConfidence, false).direction;
}

TEST(ReactToVoiceCommandVoiceDirection, PicksTheDominantConfidentDirection)
{
  MicDirectionHistory history{};
  // the wake phrase mostly from 3 o'clock, a shorter burst from 9 o'clock
  for( TimeStamp_t t = 1000; t < 1800; t += 10 ) {
    const MicDirectionIndex dir = ( t < 1200 ) ? 9 : 3;
    history.AddDirectionSample(t, true, dir, 2000, dir);
  }
  EXPECT_EQ( PickVoiceDirection(history, 2000, 2000, 500), 3 );
}

TEST(ReactToVoiceCommandVoiceDirection, IgnoresLowConfidenceAndUnknown)
{
  MicDirectionHistory history{};
  for( TimeStamp_t t = 1000; t < 1800; t += 10 ) {
    // a lot of low confidence noise from 6 o'clock and unknown direction, a little confident voice from 2
    const MicDirectionIndex dir = ( t < 1600 ) ? 6 : ( t < 1700 ? kMicDirectionUnknown : 2 );
    const MicDirectionConfidence conf = ( t < 1600 ) ? 100 : 2000;
    history.AddDirectionSample(t, true, dir, conf, dir);
  }
  EXPECT_EQ( PickVoiceDirection(history, 2000, 2000, 500), 2 );
}

TEST(ReactToVoiceCommandVoiceDirection, UnknownWhenNothingIsConfident)
{
  MicDirectionHistory history{};
  for( TimeStamp_t t = 1000; t < 1800; t += 10 ) {
    history.AddDirectionSample(t, true, 4, 100, 4);
  }
  EXPECT_EQ( PickVoiceDirection(history, 2000, 2000, 500), kMicDirectionUnknown );
  EXPECT_EQ( PickVoiceDirection(MicDirectionHistory{}, 2000, 2000, 500),
             kMicDirectionUnknown );
}

TEST(ReactToVoiceCommandVoiceDirection, OnlyCountsTheWindow)
{
  MicDirectionHistory history{};
  // a long confident sound from 8 o'clock well before the wake phrase, then the phrase from 4 o'clock
  for( TimeStamp_t t = 1000; t < 6000; t += 10 ) {
    const MicDirectionIndex dir = ( t < 5000 ) ? 8 : 4;
    history.AddDirectionSample(t, true, dir, 2000, dir);
  }
  EXPECT_EQ( PickVoiceDirection(history, 6000, 1500, 500), 4 );
}

TEST(ReactToVoiceCommandVoiceDirection, VoiceOnlyIgnoresSamplesWithoutVoice)
{
  MicDirectionHistory history{};
  // loud confident noise from 9 o'clock without voice, a shorter voice from 3 o'clock
  for( TimeStamp_t t = 1000; t < 2000; t += 10 ) {
    const bool isVoice = ( t >= 1600 );
    const MicDirectionIndex dir = isVoice ? 3 : 9;
    history.AddDirectionSample(t, isVoice, dir, 5000, dir);
  }
  const auto vote = BehaviorReactToVoiceCommand::VoteVoiceDirection(history, 2000, 1000, 500, true);
  EXPECT_EQ( vote.direction, 3 );
  EXPECT_EQ( vote.confidence, 5000 );
  EXPECT_GT( vote.samples, 0u );
  // the stock vote counts every sample
  EXPECT_EQ( BehaviorReactToVoiceCommand::VoteVoiceDirection(history, 2000, 1000, 500, false).direction, 9 );
}

TEST(ReactToVoiceCommandVoiceDirection, VoiceOnlyCountsTheVoicePartOfAMixedNode)
{
  MicDirectionHistory history{};
  // one direction run from 5 o'clock that is mostly not voice, and a voice run from 7 o'clock
  for( TimeStamp_t t = 1000; t < 1600; t += 10 ) {
    history.AddDirectionSample(t, (t >= 1500), 5, 2000, 5);
  }
  for( TimeStamp_t t = 1600; t < 1900; t += 10 ) {
    history.AddDirectionSample(t, true, 7, 2000, 7);
  }
  EXPECT_EQ( BehaviorReactToVoiceCommand::VoteVoiceDirection(history, 2000, 1000, 500, true).direction, 7 );
  EXPECT_EQ( BehaviorReactToVoiceCommand::VoteVoiceDirection(history, 2000, 1000, 500, false).direction, 5 );
}

TEST(ReactToVoiceCommandVoiceDirection, VoiceOnlyUnknownWithoutVoice)
{
  MicDirectionHistory history{};
  for( TimeStamp_t t = 1000; t < 2000; t += 10 ) {
    history.AddDirectionSample(t, false, 4, 5000, 4);
  }
  const auto vote = BehaviorReactToVoiceCommand::VoteVoiceDirection(history, 2000, 1000, 500, true);
  EXPECT_EQ( vote.direction, kMicDirectionUnknown );
  EXPECT_EQ( vote.samples, 0u );
}

TEST(MicDirectionHistoryRobotTime, MapsRobotTimeOntoTheEngineClock)
{
  MicDirectionHistory history{};
  EngineTimeStamp_t engine_ms = 0;
  // nothing to map with before the first mic sample
  EXPECT_FALSE( history.GetEngineTimeAtRobotTime(1000, engine_ms) );

  // the latest mic sample was stamped 50000 by the robot and arrived at engine time 80000
  history.AddRobotTimeSample(50000, 80000);
  ASSERT_TRUE( history.GetEngineTimeAtRobotTime(49400, engine_ms) );
  EXPECT_EQ( (TimeStamp_t)engine_ms, 79400u );
  // a robot time after the latest sample is clamped to it
  ASSERT_TRUE( history.GetEngineTimeAtRobotTime(50500, engine_ms) );
  EXPECT_EQ( (TimeStamp_t)engine_ms, 80000u );
  // older than the engine clock: not mappable
  EXPECT_FALSE( history.GetEngineTimeAtRobotTime(50000 - 90000, engine_ms) );
}

TEST(MicDirectionHistoryRobotTime, AddMicSampleRecordsTheRobotTime)
{
  BaseStationTimer::getInstance()->UpdateTime( Util::SecToNanoSec(70.0) );
  MicDirectionHistory history{};
  RobotInterface::MicDirection msg;
  msg.timestamp = 20000;
  msg.direction = 2;
  msg.confidence = 3000;
  msg.selectedDirection = 2;
  msg.activeState = 1;
  history.AddMicSample(msg);

  EngineTimeStamp_t engine_ms = 0;
  ASSERT_TRUE( history.GetEngineTimeAtRobotTime(19000, engine_ms) );
  EXPECT_EQ( (TimeStamp_t)engine_ms, 69000u );
  // and the VAD flag of the sample
  const auto nodes = history.GetRecentHistory(100);
  ASSERT_FALSE( nodes.empty() );
  EXPECT_EQ( nodes.back().vadCount, 1u );
}


} // namespace
