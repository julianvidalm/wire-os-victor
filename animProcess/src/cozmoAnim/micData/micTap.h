/**
 * File: micTap.h
 *
 * Description: Streams what Vector hears and plays to one TCP client on the LAN (the PC "cortex"), so wake word
 *              detection, speech recognition and echo cancellation can run off the robot.
 *
 *              Off by default. Turn it on at runtime with the console var MicTap_Enable (vic-anim web server,
 *              port 8889: /consolevarset?key=MicTap_Enable&value=1) or at boot by creating the file
 *              "micTapEnabled" in the persistent data folder. An optional file "micTapAllowedPeer" in the same
 *              folder holds the only IPv4 address allowed to connect.
 *
 *              Wire format v1, little-endian, robot listens on TCP 8899:
 *                hello  : "VMT1" | u16 version (1) | u32 mic rate | u32 speaker rate | u16 reserved
 *                frame  : u8 type | u8 flags | u16 reserved | u32 seq | u64 timestamp_ms | u32 payload bytes | payload
 *                  type 1 mic     : 160 x s16 mono @16 kHz, flags bit0 speaker active, bit1 robot moving
 *                  type 2 speaker : 1024 x s16 mono @32 kHz, flags bit0 buffer has audio (not silence)
 *                  type 3 stats   : u32 mic frames dropped | u32 speaker frames dropped (totals, once per second)
 *              Timestamps of mic and speaker frames share one clock (steady_clock ms), so the client can align
 *              them. seq counts per type and skips when frames are dropped.
 */

#ifndef __AnimProcess_CozmoAnim_MicTap_H_
#define __AnimProcess_CozmoAnim_MicTap_H_

#include "audioEngine/plugins/akAlsaSinkPlugIn.h"
#include "audioEngine/plugins/akAlsaSinkPluginTypes.h"
#include "audioUtil/audioDataTypes.h"
#include "coretech/common/engine/robotTimeStamp.h"
#include "micDataTypes.h"
#include "util/container/fixedCircularBuffer.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace Anki {
namespace Vector {
namespace MicData {

class MicTap {
public:
  static constexpr uint16_t kPort = 8899;

  // sink may be null (no speaker frames then). persistentFolder ends with a separator.
  MicTap(AudioEngine::PlugIns::AkAlsaSinkPlugIn* sink, const std::string& persistentFolder);
  ~MicTap();
  MicTap(const MicTap&) = delete;
  MicTap& operator=(const MicTap&) = delete;

  // Called on the mic processing thread for every processed 10 ms block. Never blocks on the network.
  void OnMicBlock(RobotTimeStamp_t timestamp, const AudioUtil::AudioSample* samples,
                  bool speakerActive, bool robotMoving);

private:
  using SinkTypes = AudioEngine::PlugIns::AkAlsaSinkPluginTypes;

  struct MicFrame {
    std::array<AudioUtil::AudioSample, kSamplesPerBlockPerChannel> samples;
    uint64_t timestamp_ms;
    uint32_t seq;
    uint8_t flags;
  };
  struct SpeakerFrame {
    std::array<SinkTypes::AkSinkAudioSample_t, SinkTypes::kAkAlsaSinkBufferSize> samples;
    uint64_t timestamp_ms;
    uint32_t seq;
    uint8_t flags;
  };

  // About 2 s of each stream; when the client falls behind the oldest frames are dropped and counted
  static constexpr size_t kMicQueueFrames = 200;
  static constexpr size_t kSpeakerQueueFrames = 64;

  void OnSpeakerBuffer(const SinkTypes::AudioChunk& chunk, bool hasData);
  void SenderLoop();
  bool OpenListener();
  void CloseListener();
  void AcceptClient();
  void CloseClient();
  bool SendAll(const uint8_t* data, size_t size);
  bool SendHello();
  bool SendQueuedFrames();
  bool SendStats();
  bool ClientHungUp();

  AudioEngine::PlugIns::AkAlsaSinkPlugIn* _sink = nullptr;
  AudioEngine::PlugIns::AkAlsaSinkPlugIn::PlaybackObserverId _sinkObserverId =
    AudioEngine::PlugIns::AkAlsaSinkPlugIn::kInvalidPlaybackObserverId;
  uint32_t _allowedPeer = 0;  // IPv4 in network byte order; 0 accepts any peer

  // True only while a client is connected: producers skip all work otherwise
  std::atomic<bool> _streaming{false};

  std::mutex _queueMutex;
  std::condition_variable _queueCondition;
  Util::FixedCircularBuffer<MicFrame, kMicQueueFrames> _micQueue;
  Util::FixedCircularBuffer<SpeakerFrame, kSpeakerQueueFrames> _speakerQueue;
  uint32_t _micSeq = 0;
  uint32_t _speakerSeq = 0;
  uint32_t _micDropped = 0;
  uint32_t _speakerDropped = 0;

  // Owned by the sender thread
  int _listenFd = -1;
  int _clientFd = -1;
  std::vector<uint8_t> _sendBuffer;
  std::vector<MicFrame> _micBatch;
  std::vector<SpeakerFrame> _speakerBatch;

  std::atomic<bool> _stop{false};
  std::thread _senderThread;
};

} // namespace MicData
} // namespace Vector
} // namespace Anki

#endif // __AnimProcess_CozmoAnim_MicTap_H_
