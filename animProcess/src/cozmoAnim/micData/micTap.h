/**
 * File: micTap.h
 *
 * Description: Hands what Vector hears and plays to vic-cloud, which uploads it to the cortex (wire-pod) over
 *              TLS, so wake word detection, speech recognition and echo cancellation can run off the robot. The
 *              robot opens no network port: frames go to the local unix datagram socket /dev/socket/mictap_sock,
 *              served here; vic-cloud is its only client and announces itself with "ANKICONN" (every 5 s).
 *
 *              Off by default. Turn it on at runtime with the console var MicTap_Enable (vic-anim web server,
 *              port 8889: /consolevarset?key=MicTap_Enable&value=1) or at boot by creating the file
 *              "micTapEnabled" in the persistent data folder.
 *
 *              Raw microphone frames (type 4) have their own switch, off by default and independent of
 *              MicTap_Enable: console var MicTap_RawEnable, or the file "micTapRawEnabled" in the persistent data
 *              folder at boot. They are sent only while MicTap is enabled and the raw switch is on.
 *
 *              Wire format v1, little-endian; the hello travels alone in one datagram, frames are batched
 *              (several frames per datagram, never split):
 *                hello  : "VMT1" | u16 version (1) | u32 mic rate | u32 speaker rate | u16 reserved
 *                         (sent when the client appears and then once per second, so uploads can start any time)
 *                frame  : u8 type | u8 flags | u16 reserved | u32 seq | u64 timestamp_ms | u32 payload bytes | payload
 *                  type 1 mic     : 160 x s16 mono @16 kHz, flags bit0 speaker active, bit1 robot moving
 *                  type 2 speaker : 1024 x s16 mono @32 kHz, flags bit0 buffer has audio (not silence)
 *                  type 3 stats   : u32 mic frames dropped | u32 speaker frames dropped (totals, once per second)
 *                  type 4 raw mic : 4 x 160 x s16 interleaved (ch0 ch1 ch2 ch3 ch0 ...) at the native mic rate
 *                                   (~15662 Hz), the same 10 ms block as the type 1 frame with the same timestamp,
 *                                   taken before SignalEssence; flags as type 1. Its seq advances with every
 *                                   block while streaming, so time with the raw switch off shows as a seq gap.
 *                                   Raw drops are not in the stats frame (its layout is fixed); seq gaps show them.
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
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class LocalUdpServer;

namespace Anki {
namespace Vector {
namespace MicData {

class MicTap {
public:
  // sink may be null (no speaker frames then). persistentFolder ends with a separator.
  MicTap(AudioEngine::PlugIns::AkAlsaSinkPlugIn* sink, const std::string& persistentFolder);
  ~MicTap();
  MicTap(const MicTap&) = delete;
  MicTap& operator=(const MicTap&) = delete;

  // Called on the mic processing thread for every processed 10 ms block. Never blocks on the network.
  // rawChunk is the same block before SignalEssence: kIncomingAudioChunkSize samples, channel-planar
  // (channel c, sample i at rawChunk[c * kSamplesPerBlockPerChannel + i]).
  void OnMicBlock(RobotTimeStamp_t timestamp, const AudioUtil::AudioSample* samples,
                  const AudioUtil::AudioSample* rawChunk, bool speakerActive, bool robotMoving);

private:
  using SinkTypes = AudioEngine::PlugIns::AkAlsaSinkPluginTypes;

  struct MicFrame {
    std::array<AudioUtil::AudioSample, kSamplesPerBlockPerChannel> samples;
    uint64_t timestamp_ms;
    uint32_t seq;
    uint8_t flags;
  };
  struct RawMicFrame {
    std::array<AudioUtil::AudioSample, kIncomingAudioChunkSize> samples;  // channel-planar, as received
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
  static constexpr size_t kRawMicQueueFrames = 200;

  void OnSpeakerBuffer(const SinkTypes::AudioChunk& chunk, bool hasData);
  void SenderLoop();
  bool StartServer();
  void StopServer();
  void PollClient();
  void StartStreaming();
  void StopStreaming();
  void SendDatagram();
  void SendHello();
  void SendQueuedFrames();
  void SendStats();

  AudioEngine::PlugIns::AkAlsaSinkPlugIn* _sink = nullptr;
  AudioEngine::PlugIns::AkAlsaSinkPlugIn::PlaybackObserverId _sinkObserverId =
    AudioEngine::PlugIns::AkAlsaSinkPlugIn::kInvalidPlaybackObserverId;

  // True only while vic-cloud is connected: producers skip all work otherwise
  std::atomic<bool> _streaming{false};

  std::mutex _queueMutex;
  std::condition_variable _queueCondition;
  Util::FixedCircularBuffer<MicFrame, kMicQueueFrames> _micQueue;
  Util::FixedCircularBuffer<SpeakerFrame, kSpeakerQueueFrames> _speakerQueue;
  Util::FixedCircularBuffer<RawMicFrame, kRawMicQueueFrames> _rawMicQueue;
  uint32_t _micSeq = 0;
  uint32_t _speakerSeq = 0;
  uint32_t _rawMicSeq = 0;
  uint32_t _micDropped = 0;
  uint32_t _speakerDropped = 0;
  uint32_t _rawMicDropped = 0;

  // Owned by the sender thread
  std::unique_ptr<LocalUdpServer> _server;
  uint32_t _sendFailures = 0;  // consecutive; the client is dropped after a few
  std::vector<uint8_t> _sendBuffer;
  std::vector<MicFrame> _micBatch;
  std::vector<SpeakerFrame> _speakerBatch;
  std::vector<RawMicFrame> _rawMicBatch;
  uint32_t _rawMicDroppedLogged = 0;

  std::atomic<bool> _stop{false};
  std::thread _senderThread;
};

} // namespace MicData
} // namespace Vector
} // namespace Anki

#endif // __AnimProcess_CozmoAnim_MicTap_H_
