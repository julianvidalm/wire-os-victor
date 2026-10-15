/**
 * File: micTap.cpp
 *
 * Description: See micTap.h. Producers (mic processing thread, audio sink thread) only copy a frame into a bounded
 *              queue; one sender thread owns the sockets, so the audio threads never wait on the network.
 */

#include "cozmoAnim/micData/micTap.h"

#include "util/console/consoleInterface.h"
#include "util/fileUtils/fileUtils.h"
#include "util/helpers/ankiDefines.h"
#include "util/logging/logging.h"
#include "util/threading/threadPriority.h"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <sys/socket.h>
#include <unistd.h>

#define LOG_CHANNEL "MicTap"
#define CONSOLE_GROUP "MicTap"

namespace Anki {
namespace Vector {
namespace MicData {

namespace {
  CONSOLE_VAR(bool, kMicTap_Enable, CONSOLE_GROUP, false);

  const char* const kEnabledFile = "micTapEnabled";
  const char* const kAllowedPeerFile = "micTapAllowedPeer";

  constexpr uint16_t kProtocolVersion = 1;
  constexpr uint8_t kFrameMic = 1;
  constexpr uint8_t kFrameSpeaker = 2;
  constexpr uint8_t kFrameStats = 3;
  constexpr uint8_t kMicFlagSpeakerActive = 1 << 0;
  constexpr uint8_t kMicFlagRobotMoving = 1 << 1;
  constexpr uint8_t kSpeakerFlagHasData = 1 << 0;

  constexpr int kSenderCpu = 3;  // MicProcRaw uses CPU 2, MicProcTrigger CPU 1
  constexpr auto kIdlePoll = std::chrono::milliseconds(100);
  constexpr auto kSendPeriod = std::chrono::milliseconds(20);  // adds at most 20 ms of latency
  constexpr auto kStatsPeriod = std::chrono::seconds(1);
  constexpr int kSendTimeout_ms = 500;

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

  uint32_t ReadAllowedPeer(const std::string& path)
  {
    if (!Util::FileUtils::FileExists(path)) {
      return 0;
    }
    std::string text = Util::FileUtils::ReadFile(path);
    text.erase(text.find_last_not_of(" \t\r\n") + 1);
    in_addr addr{};
    if (inet_pton(AF_INET, text.c_str(), &addr) != 1) {
      LOG_WARNING("MicTap.AllowedPeer.Invalid", "Ignoring '%s'; any peer may connect", text.c_str());
      return 0;
    }
    return addr.s_addr;
  }
}

MicTap::MicTap(AudioEngine::PlugIns::AkAlsaSinkPlugIn* sink, const std::string& persistentFolder)
: _sink(sink)
{
  _allowedPeer = ReadAllowedPeer(persistentFolder + kAllowedPeerFile);
  if (Util::FileUtils::FileExists(persistentFolder + kEnabledFile)) {
    kMicTap_Enable = true;
  }
  if (_sink != nullptr) {
    _sinkObserverId = _sink->AddPlaybackObserver(
      [this](const SinkTypes::AudioChunk& chunk, bool hasData) { OnSpeakerBuffer(chunk, hasData); });
  }
  _micBatch.reserve(kMicQueueFrames);
  _speakerBatch.reserve(kSpeakerQueueFrames);
  _senderThread = std::thread(&MicTap::SenderLoop, this);
  LOG_INFO("MicTap.Init", "port %u, enabled %d, allowed peer %s", kPort, kMicTap_Enable ? 1 : 0,
           _allowedPeer == 0 ? "any" : "restricted");
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
                        bool speakerActive, bool robotMoving)
{
  if (!_streaming.load(std::memory_order_relaxed)) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(_queueMutex);
    if (_micQueue.size() == _micQueue.capacity()) {
      ++_micDropped;  // push_back() overwrites the oldest frame
    }
    MicFrame& frame = _micQueue.push_back();
    std::memcpy(frame.samples.data(), samples, sizeof(frame.samples));
    frame.timestamp_ms = static_cast<TimeStamp_t>(timestamp);
    frame.seq = _micSeq++;
    frame.flags = (speakerActive ? kMicFlagSpeakerActive : 0) | (robotMoving ? kMicFlagRobotMoving : 0);
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
      CloseClient();
      CloseListener();
      std::this_thread::sleep_for(kIdlePoll);
      continue;
    }
    if (_listenFd < 0 && !OpenListener()) {
      std::this_thread::sleep_for(kIdlePoll * 10);
      continue;
    }
    if (_clientFd < 0) {
      AcceptClient();
      nextStats = std::chrono::steady_clock::now() + kStatsPeriod;
      continue;
    }
    {
      std::unique_lock<std::mutex> lock(_queueMutex);
      // Producers do not signal: waking once per batch instead of once per frame keeps the cost of the tap low
      _queueCondition.wait_for(lock, kSendPeriod, [this] { return _stop.load(); });
    }
    bool ok = SendQueuedFrames();
    const auto now = std::chrono::steady_clock::now();
    if (ok && now >= nextStats) {
      ok = SendStats();
      nextStats = now + kStatsPeriod;  // never "catch up" with a burst after a stall
    }
    if (!ok || ClientHungUp()) {
      CloseClient();
    }
  }
  CloseClient();
  CloseListener();
}

bool MicTap::OpenListener()
{
  const int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    LOG_ERROR("MicTap.OpenListener.Socket", "%s", strerror(errno));
    return false;
  }
  const int yes = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(kPort);
  if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(fd, 1) != 0) {
    LOG_ERROR("MicTap.OpenListener.Bind", "port %u: %s", kPort, strerror(errno));
    close(fd);
    return false;
  }
  _listenFd = fd;
  LOG_INFO("MicTap.OpenListener", "listening on %u", kPort);
  return true;
}

void MicTap::CloseListener()
{
  if (_listenFd >= 0) {
    close(_listenFd);
    _listenFd = -1;
    LOG_INFO("MicTap.CloseListener", "");
  }
}

void MicTap::AcceptClient()
{
  pollfd pfd{_listenFd, POLLIN, 0};
  const int timeout_ms = static_cast<int>(kIdlePoll.count());
  if (poll(&pfd, 1, timeout_ms) <= 0 || !(pfd.revents & POLLIN)) {
    return;
  }
  sockaddr_in peer{};
  socklen_t peerLen = sizeof(peer);
  const int fd = accept4(_listenFd, reinterpret_cast<sockaddr*>(&peer), &peerLen, SOCK_CLOEXEC);
  if (fd < 0) {
    return;
  }
  char peerText[INET_ADDRSTRLEN] = {};
  inet_ntop(AF_INET, &peer.sin_addr, peerText, sizeof(peerText));
  if (_allowedPeer != 0 && peer.sin_addr.s_addr != _allowedPeer) {
    LOG_WARNING("MicTap.AcceptClient.Rejected", "%s", peerText);
    close(fd);
    return;
  }
  const int yes = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
  timeval sendTimeout{0, kSendTimeout_ms * 1000};
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &sendTimeout, sizeof(sendTimeout));
  _clientFd = fd;

  // Start from fresh audio, not whatever was queued for a previous client
  {
    std::lock_guard<std::mutex> lock(_queueMutex);
    _micQueue.clear();
    _speakerQueue.clear();
  }
  if (!SendHello()) {
    CloseClient();
    return;
  }
  _streaming = true;
  LOG_INFO("MicTap.AcceptClient", "streaming to %s", peerText);
}

void MicTap::CloseClient()
{
  if (_clientFd >= 0) {
    _streaming = false;
    close(_clientFd);
    _clientFd = -1;
    LOG_INFO("MicTap.CloseClient", "");
  }
}

bool MicTap::SendAll(const uint8_t* data, size_t size)
{
  while (size > 0) {
    // MSG_NOSIGNAL: a client that went away must not raise SIGPIPE and kill vic-anim
    const ssize_t sent = send(_clientFd, data, size, MSG_NOSIGNAL);
    if (sent < 0) {
      if (errno == EINTR) {
        continue;
      }
      LOG_INFO("MicTap.Send", "%s", strerror(errno));
      return false;
    }
    data += sent;
    size -= static_cast<size_t>(sent);
  }
  return true;
}

bool MicTap::SendHello()
{
  _sendBuffer.clear();
  _sendBuffer.insert(_sendBuffer.end(), {'V', 'M', 'T', '1'});
  Append<uint16_t>(_sendBuffer, kProtocolVersion);
  Append<uint32_t>(_sendBuffer, kSampleRateIncoming_hz);
  Append<uint32_t>(_sendBuffer, static_cast<uint32_t>(SinkTypes::kAkAlsaSinkSampleRate));
  Append<uint16_t>(_sendBuffer, 0);
  return SendAll(_sendBuffer.data(), _sendBuffer.size());
}

bool MicTap::SendQueuedFrames()
{
  _micBatch.clear();
  _speakerBatch.clear();
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
  }
  if (_micBatch.empty() && _speakerBatch.empty()) {
    return true;
  }
  _sendBuffer.clear();
  for (const auto& frame : _micBatch) {
    AppendFrame(_sendBuffer, kFrameMic, frame);
  }
  for (const auto& frame : _speakerBatch) {
    AppendFrame(_sendBuffer, kFrameSpeaker, frame);
  }
  return SendAll(_sendBuffer.data(), _sendBuffer.size());
}

bool MicTap::SendStats()
{
  uint32_t micDropped = 0;
  uint32_t speakerDropped = 0;
  {
    std::lock_guard<std::mutex> lock(_queueMutex);
    micDropped = _micDropped;
    speakerDropped = _speakerDropped;
  }
  _sendBuffer.clear();
  AppendFrameHeader(_sendBuffer, kFrameStats, 0, 0, NowMs(), 2 * sizeof(uint32_t));
  Append<uint32_t>(_sendBuffer, micDropped);
  Append<uint32_t>(_sendBuffer, speakerDropped);
  return SendAll(_sendBuffer.data(), _sendBuffer.size());
}

bool MicTap::ClientHungUp()
{
  // The client never sends anything; readable means it closed (or sent junk, which we discard)
  pollfd pfd{_clientFd, POLLIN, 0};
  if (poll(&pfd, 1, 0) <= 0) {
    return false;
  }
  if (pfd.revents & (POLLHUP | POLLERR)) {
    return true;
  }
  uint8_t discard[256];
  const ssize_t got = recv(_clientFd, discard, sizeof(discard), MSG_DONTWAIT);
  return got == 0 || (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR);
}

} // namespace MicData
} // namespace Vector
} // namespace Anki
