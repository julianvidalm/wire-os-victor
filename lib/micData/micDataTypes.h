/**
* File: micDataTypes.h
*
* Author: Lee Crippen
* Created: 10/25/2017
*
* Description: Holds types associated with mic data processing.
*
* Copyright: Anki, Inc. 2017
*
*/

#ifndef __MicData_MicDataTypes_H_
#define __MicData_MicDataTypes_H_

#include <array>
#include <cstdint>

namespace Anki {
namespace Vector {
namespace MicData {

  enum class MicDataType {
    Raw,
    Processed
  };

  static constexpr uint32_t kNumInputChannels               = 4;
  static constexpr uint32_t kSamplesPerBlockPerChannel      = 160;
  static constexpr uint32_t kSampleRateIncoming_hz          = 16000;
  static constexpr uint32_t kTimePerChunk_ms                = 10;
  static constexpr uint32_t kIncomingAudioChunkSize         = kSamplesPerBlockPerChannel * kNumInputChannels;
  static constexpr uint32_t kTriggerOverlapSize_ms          = 50;
  static constexpr uint32_t kTriggerLessOverlapSize_ms      = 0;
  static constexpr uint32_t kTriggerAudioLengthDebug_ms     = 1500; // We only hold onto more audio for FP testing during development
  static constexpr uint32_t kTriggerAudioLengthShipping_ms  = 750;
  // the robot's mic stream cap after the wake (the cloud normally ends the stream sooner, at the end of speech).
  // 14s: the listening feedback gives up ~9s after the wake (7s after the look gesture, which ends ~1.5-2s
  // after the wake), and a user who starts talking just before that still has ~5s to speak
  static constexpr uint32_t kStreamingTimeout_ms            = 14000 + kTriggerOverlapSize_ms;
  static constexpr uint32_t kStreamingDefaultMinDuration_ms = 4000;
  static constexpr uint32_t kRawAudioPerBuffer_ms           = 1000;

  using DirectionIndex = uint16_t;
  using DirectionConfidence = int16_t;
  
  static constexpr DirectionIndex kFirstIndex = 0;
  static constexpr DirectionIndex kLastValidIndex = 11;
  static constexpr DirectionIndex kDirectionUnknown = 12;
  static constexpr DirectionIndex kLastIndex = kDirectionUnknown;
  static constexpr size_t kNumDirections = kLastIndex - kFirstIndex + 1;
  using DirectionConfidences = std::array<float, kNumDirections>;

  struct MicDirectionData
  {
    DirectionIndex        winningDirection = 0;
    DirectionConfidence   winningConfidence = 0;
    DirectionIndex        selectedDirection = 0;
    DirectionConfidence   selectedConfidence = 0;
    DirectionConfidences  confidenceList{};
    int                   activeState = 0;
    float                 latestPowerValue = 0.f;
    float                 latestNoiseFloor = 0.f;
  };
} // namespace MicData
} // namespace Vector
} // namespace Anki

#endif // __MicData_MicDataTypes_H_
