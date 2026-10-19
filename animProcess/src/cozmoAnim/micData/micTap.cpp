/**
 * File: micTap.cpp
 *
 * Description: See micTap.h. Producers (mic processing thread, audio sink thread) only copy a frame into a bounded
 *              queue; one sender thread owns the local socket, so the audio threads never wait on the client.
 */

#include "cozmoAnim/micData/micTap.h"

#include "coretech/messaging/shared/LocalUdpServer.h"
#include "coretech/messaging/shared/socketConstants.h"
#include "util/console/consoleInterface.h"
#include "util/fileUtils/fileUtils.h"
#include "util/helpers/ankiDefines.h"
#include "util/logging/logging.h"
#include "util/threading/threadPriority.h"

#include <chrono>
#include <cstring>
#include <pthread.h>
#include <sched.h>

#define LOG_CHANNEL "MicTap"
#define CONSOLE_GROUP "MicTap"

namespace Anki {
namespace Vector {
namespace MicData {

namespace {
  CONSOLE_VAR(bool, kMicTap_Enable, CONSOLE_GROUP, false);
  // Raw 4-channel mic frames (type 4, ~128 kB/s), sent only while MicTap_Enable is on as well
  CONSOLE_VAR(bool, kMicTap_RawEnable, CONSOLE_GROUP, false);

  const char* const kEnabledFile = "micTapEnabled";
  const char* const kRawEnabledFile = "micTapRawEnabled";
  constexpr char kSocketPath[] = LOCAL_SOCKET_PATH "mictap_sock";

  // About 2.5 s of mic and speaker audio, so vic-cloud can stall briefly without losing frames
  constexpr int kSendBufferBytes = 256 * 1024;
  constexpr int kRecvBufferBytes = 4 * 1024;  // only "ANKICONN" packets arrive
  // Consecutive failed sends (vic-cloud gone) before the client is dropped until it announces itself again
  constexpr uint32_t kMaxSendFailures = 20;
  // Frames of one send period travel in one datagram (the stream is a byte stream, so frames may share one):
  // about 50 datagrams per second instead of 130, each well under the socket buffer.
  constexpr size_t kMaxDatagramBytes = 16 * 1024;

  constexpr uint16_t kProtocolVersion = 1;
  constexpr uint8_t kFrameMic = 1;
  constexpr uint8_t kFrameSpeaker = 2;
  constexpr uint8_t kFrameStats = 3;
  constexpr uint8_t kFrameRawMic = 4;
  constexpr uint8_t kMicFlagSpeakerActive = 1 << 0;
  constexpr uint8_t kMicFlagRobotMoving = 1 << 1;
  constexpr uint8_t kSpeakerFlagHasData = 1 << 0;

  constexpr int kSenderCpu = 3;  // MicProcRaw uses CPU 2, MicProcTrigger CPU 1
  constexpr auto kIdlePoll = std::chrono::milliseconds(100);
  constexpr auto kSendPeriod = std::chrono::milliseconds(20);  // adds at most 20 ms of latency
  constexpr auto kStatsPeriod = std::chrono::seconds(1);

  // Same clock as vic-robot's HAL::GetTimeStamp(), which stamps the mic blocks
  uint64_t NowMs()
  {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    return static_cast<uint64_t>(static_cast<TimeStamp_t>(ms));
  }

  template <typename T>
  void Append(std::vector<uint8_t>& out, T value)
  {
    const auto* bytes = reinterpret_cast<const uint8_t*>(&value);  // little-endian host (ARM, x86)
    out.insert(out.end(), bytes, bytes + sizeof(T));
  }

  void AppendFrameHeader(std::vector<uint8_t>& out, uint8_t type, uint8_t flags, uint32_t seq,
                         uint64_t timestamp_ms, uint32_t payloadBytes)
  {
    Append<uint8_t>(out, type);
    Append<uint8_t>(out, flags);
    Append<uint16_t>(out, 0);
    Append<uint32_t>(out, seq);
    Append<uint64_t>(out, timestamp_ms);
    Append<uint32_t>(out, payloadBytes);
  }

  template <typename Frame>
  void AppendFrame(std::vector<uint8_t>& out, uint8_t type, const Frame& frame)
  {
    const uint32_t payloadBytes = static_cast<uint32_t>(sizeof(frame.samples));
    AppendFrameHeader(out, type, frame.flags, frame.seq, frame.timestamp_ms, payloadBytes);
    const auto* bytes = reinterpret_cast<const uint8_t*>(frame.samples.data());
    out.insert(out.end(), bytes, bytes + payloadBytes);
  }

  // The robot delivers the raw block channel-planar; the wire carries it interleaved, like a 4-channel .wav
  template <typename Frame>
  void AppendInterleavedFrame(std::vector<uint8_t>& out, uint8_t type, const Frame& frame)
  {
    const uint32_t payloadBytes = static_cast<uint32_t>(sizeof(frame.samples));
    AppendFrameHeader(out, type, frame.flags, frame.seq, frame.timestamp_ms, payloadBytes);
    const size_t start = out.size();
    out.resize(start + payloadBytes);
    auto* dest = reinterpret_cast<AudioUtil::AudioSample*>(out.data() + start);
    for (size_t sample = 0; sample < kSamplesPerBlockPerChannel; ++sample) {
      for (size_t channel = 0; channel < kNumInputChannels; ++channel) {
        dest[kNumInputChannels * sample + channel] = frame.samples[channel * kSamplesPerBlockPerChannel + sample];
      }
    }
  }
}

MicTap::MicTap(AudioEngine::PlugIns::AkAlsaSinkPlugIn* sink, const std::string& persistentFolder)
: _sink(sink)
{
  if (Util::FileUtils::FileExists(persistentFolder + kEnabledFile)) {
    kMicTap_Enable = true;
  }
  if (Util::FileUtils::FileExists(persistentFolder + kRawEnabledFile)) {
    kMicTap_RawEnable = true;
  }
  if (_sink != nullptr) {
    _sinkObserverId = _sink->AddPlaybackObserver(
      [this](const SinkTypes::AudioChunk& chunk, bool hasData) { OnSpeakerBuffer(chunk, hasData); });
  }
  _micBatch.reserve(kMicQueueFrames);
  _speakerBatch.reserve(kSpeakerQueueFrames);
  _rawMicBatch.reserve(kRawMicQueueFrames);
  _senderThread = std::thread(&MicTap::SenderLoop, this);
  LOG_INFO("MicTap.Init", "socket %s, enabled %d, raw %d", kSocketPath, kMicTap_Enable ? 1 : 0,
           kMicTap_RawEnable ? 1 : 0);
}

MicTap::~MicTap()
{
  if (_sink != nullptr) {
    _sink->RemovePlaybackObserver(_sinkObserverId);
  }
  _stop = true;
  _queueCondition.notify_all();
  if (_senderThread.joinable()) {
    _senderThread.join();
  }
}

void MicTap::OnMicBlock(RobotTimeStamp_t timestamp, const AudioUtil::AudioSample* samples,
                        const AudioUtil::AudioSample* rawChunk, bool speakerActive, bool robotMoving)
{
  if (!_streaming.load(std::memory_order_relaxed)) {
    return;
  }
  const uint64_t timestamp_ms = static_cast<TimeStamp_t>(timestamp);
  const uint8_t flags = (speakerActive ? kMicFlagSpeakerActive : 0) | (robotMoving ? kMicFlagRobotMoving : 0);
  const bool sendRaw = kMicTap_RawEnable && (rawChunk != nullptr);
  {
    std::lock_guard<std::mutex> lock(_queueMutex);
    if (_micQueue.size() == _micQueue.capacity()) {
      ++_micDropped;  // push_back() overwrites the oldest frame
    }
    MicFrame& frame = _micQueue.push_back();
    std::memcpy(frame.samples.data(), samples, sizeof(frame.samples));
    frame.timestamp_ms = timestamp_ms;
    frame.seq = _micSeq++;
    frame.flags = flags;

    // Advances even with the raw switch off, so raw seq tracks time like the mic seq
    const uint32_t rawSeq = _rawMicSeq++;
    if (sendRaw) {
      if (_rawMicQueue.size() == _rawMicQueue.capacity()) {
        ++_rawMicDropped;
      }
      RawMicFrame& rawFrame = _rawMicQueue.push_back();
      std::memcpy(rawFrame.samples.data(), rawChunk, sizeof(rawFrame.samples));  // interleaved by the sender
      rawFrame.timestamp_ms = timestamp_ms;
      rawFrame.seq = rawSeq;
      rawFrame.flags = flags;
    }
  }
}

void MicTap::OnSpeakerBuffer(const SinkTypes::AudioChunk& chunk, bool hasData)
{
  // Runs on the audio sink thread under its buffer lock: copy and return
  if (!_streaming.load(std::memory_order_relaxed)) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(_queueMutex);
    if (_speakerQueue.size() == _speakerQueue.capacity()) {
      ++_speakerDropped;
    }
    SpeakerFrame& frame = _speakerQueue.push_back();
    std::memcpy(frame.samples.data(), chunk, sizeof(frame.samples));
    frame.timestamp_ms = NowMs();
    frame.seq = _speakerSeq++;
    frame.flags = hasData ? kSpeakerFlagHasData : 0;
  }
}

void MicTap::SenderLoop()
{
  Util::SetThreadName(pthread_self(), "MicTapSender");
#if defined(ANKI_PLATFORM_VICOS)
  cpu_set_t cpuSet;
  CPU_ZERO(&cpuSet);
  CPU_SET(kSenderCpu, &cpuSet);
  const int error = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuSet);
  if (error != 0) {
    LOG_WARNING("MicTap.SenderLoop.Affinity", "error %d", error);
  }
#endif

  auto nextStats = std::chrono::steady_clock::now() + kStatsPeriod;
  while (!_stop) {
    if (!kMicTap_Enable) {
      StopStreaming();
      StopServer();
      std::this_thread::sleep_for(kIdlePoll);
      continue;
    }
    if (_server == nullptr && !StartServer()) {
      std::this_thread::sleep_for(kIdlePoll * 10);
      continue;
    }
    PollClient();
    if (!_server->HasClient()) {
      StopStreaming();
      std::this_thread::sleep_for(kIdlePoll);
      continue;
    }
    if (!_streaming) {
      StartStreaming();
      nextStats = std::chrono::steady_clock::now() + kStatsPeriod;
    }
    {
      std::unique_lock<std::mutex> lock(_queueMutex);
      // Producers do not signal: waking once per batch instead of once per frame keeps the cost of the tap low
      _queueCondition.wait_for(lock, kSendPeriod, [this] { return _stop.load(); });
    }
    SendQueuedFrames();
    const auto now = std::chrono::steady_clock::now();
    if (now >= nextStats) {
      // The hello is repeated so vic-cloud can start an upload at any time with the current parameters
      SendHello();
      SendStats();
      nextStats = now + kStatsPeriod;  // never "catch up" with a burst after a stall
    }
  }
  StopStreaming();
  StopServer();
}

bool MicTap::StartServer()
{
  auto server = std::make_unique<LocalUdpServer>(kSendBufferBytes, kRecvBufferBytes);
  server->SetBindClients(false);
  if (!server->StartListening(kSocketPath)) {
    LOG_ERROR("MicTap.StartServer", "cannot listen on %s", kSocketPath);
    return false;
  }
  _server = std::move(server);
  LOG_INFO("MicTap.StartServer", "listening on %s", kSocketPath);
  return true;
}

void MicTap::StopServer()
{
  if (_server != nullptr) {
    _server->StopListening();
    _server.reset();
    LOG_INFO("MicTap.StopServer", "");
  }
}

void MicTap::PollClient()
{
  // Recv() is what registers the client: vic-cloud sends "ANKICONN" when it connects and every 5 s after
  char packet[64];
  while (_server->Recv(packet, sizeof(packet)) > 0) {
  }
}

void MicTap::StartStreaming()
{
  // Start from fresh audio, not whatever was queued before
  {
    std::lock_guard<std::mutex> lock(_queueMutex);
    _micQueue.clear();
    _speakerQueue.clear();
    _rawMicQueue.clear();
  }
  _sendFailures = 0;
  _streaming = true;
  SendHello();
  LOG_INFO("MicTap.StartStreaming", "");
}

void MicTap::StopStreaming()
{
  if (_streaming) {
    _streaming = false;
    LOG_INFO("MicTap.StopStreaming", "");
  }
}

void MicTap::SendDatagram()
{
  if (!_streaming) {
    return;
  }
  const ssize_t sent = _server->Send(reinterpret_cast<const char*>(_sendBuffer.data()), _sendBuffer.size());
  if (sent == static_cast<ssize_t>(_sendBuffer.size())) {
    _sendFailures = 0;
    return;
  }
  // Not delivered (vic-cloud gone, or its socket buffer full): the frame is lost and its seq leaves a gap
  if (++_sendFailures >= kMaxSendFailures) {
    LOG_WARNING("MicTap.SendDatagram", "%u sends failed in a row; waiting for vic-cloud to reconnect", _sendFailures);
    _server->Disconnect();
    StopStreaming();
  }
}

void MicTap::SendHello()
{
  _sendBuffer.clear();
  _sendBuffer.insert(_sendBuffer.end(), {'V', 'M', 'T', '1'});
  Append<uint16_t>(_sendBuffer, kProtocolVersion);
  Append<uint32_t>(_sendBuffer, kSampleRateIncoming_hz);
  Append<uint32_t>(_sendBuffer, static_cast<uint32_t>(SinkTypes::kAkAlsaSinkSampleRate));
  Append<uint16_t>(_sendBuffer, 0);
  SendDatagram();
}

void MicTap::SendQueuedFrames()
{
  _micBatch.clear();
  _speakerBatch.clear();
  _rawMicBatch.clear();
  {
    std::lock_guard<std::mutex> lock(_queueMutex);
    while (_micQueue.size() > 0) {
      _micBatch.push_back(_micQueue.front());
      _micQueue.pop_front();
    }
    while (_speakerQueue.size() > 0) {
      _speakerBatch.push_back(_speakerQueue.front());
      _speakerQueue.pop_front();
    }
    while (_rawMicQueue.size() > 0) {
      _rawMicBatch.push_back(_rawMicQueue.front());
      _rawMicQueue.pop_front();
    }
  }
  _sendBuffer.clear();
  for (const auto& frame : _micBatch) {
    AppendFrame(_sendBuffer, kFrameMic, frame);
    if (_sendBuffer.size() >= kMaxDatagramBytes) {
      SendDatagram();
      _sendBuffer.clear();
    }
  }
  for (const auto& frame : _speakerBatch) {
    AppendFrame(_sendBuffer, kFrameSpeaker, frame);
    if (_sendBuffer.size() >= kMaxDatagramBytes) {
      SendDatagram();
      _sendBuffer.clear();
    }
  }
  for (const auto& frame : _rawMicBatch) {
    AppendInterleavedFrame(_sendBuffer, kFrameRawMic, frame);
    if (_sendBuffer.size() >= kMaxDatagramBytes) {
      SendDatagram();
      _sendBuffer.clear();
    }
  }
  if (!_sendBuffer.empty()) {
    SendDatagram();
  }
}

void MicTap::SendStats()
{
  uint32_t micDropped = 0;
  uint32_t speakerDropped = 0;
  uint32_t rawMicDropped = 0;
  {
    std::lock_guard<std::mutex> lock(_queueMutex);
    micDropped = _micDropped;
    speakerDropped = _speakerDropped;
    rawMicDropped = _rawMicDropped;
  }
  // The stats frame layout is fixed (two totals), so raw drops are logged here and visible as raw seq gaps
  if (rawMicDropped != _rawMicDroppedLogged) {
    LOG_WARNING("MicTap.SendStats.RawMicDropped", "%u raw mic frames dropped in total", rawMicDropped);
    _rawMicDroppedLogged = rawMicDropped;
  }
  _sendBuffer.clear();
  AppendFrameHeader(_sendBuffer, kFrameStats, 0, 0, NowMs(), 2 * sizeof(uint32_t));
  Append<uint32_t>(_sendBuffer, micDropped);
  Append<uint32_t>(_sendBuffer, speakerDropped);
  SendDatagram();
}

} // namespace MicData
} // namespace Vector
} // namespace Anki
