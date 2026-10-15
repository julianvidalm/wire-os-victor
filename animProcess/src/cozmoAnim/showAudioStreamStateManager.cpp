/**
* File: cozmoAnim/showAudioStreamStateManager.cpp
*
* Author: Kevin M. Karol
* Created: 8/3/2018
*
* Description: Communicates the current state of cloud audio streaming to the user
* and ensures expectations of related animation components are met (e.g. motion/lack there of when streaming)
*
* Copyright: Anki, Inc. 2018
**/

#include "cozmoAnim/showAudioStreamStateManager.h"

#include "micDataTypes.h"
#include "cozmoAnim/animation/animationStreamer.h"
#include "cozmoAnim/audio/engineRobotAudioInput.h"
#include "cozmoAnim/audio/cozmoAudioController.h"
#include "cozmoAnim/robotDataLoader.h"

#include "audioEngine/audioTypeTranslator.h"

namespace{
const int32_t kUseDefaultStreamingDuration = -1;
}

namespace Anki {
namespace Vector {

ShowAudioStreamStateManager::ShowAudioStreamStateManager(const Anim::AnimContext* context)
: _context(context)
, _minStreamingDuration_ms(kUseDefaultStreamingDuration)
{
  // Initialize this value to prevent errors before the TriggerResponse is first set
  _postAudioEvent.audioEvent = AudioMetaData::GameEvent::GenericEvent::Invalid;
}


ShowAudioStreamStateManager::~ShowAudioStreamStateManager()
{

}

void ShowAudioStreamStateManager::Update()
{
  {
    std::lock_guard<std::recursive_mutex> lock(_triggerResponseMutex);
    if(_havePendingTriggerResponse)
    {
      if(_pendingTriggerResponseHasGetIn)
      {
        StartTriggerResponseWithGetIn(_responseCallback);
      }
      else
      {
        StartTriggerResponseWithoutGetIn(_responseCallback);
      }

      _havePendingTriggerResponse = false;
      _responseCallback = nullptr;
    }
  }
}

void ShowAudioStreamStateManager::SetTriggerWordResponse(const RobotInterface::SetTriggerWordResponse& msg)
{
  std::lock_guard<std::recursive_mutex> lock(_triggerResponseMutex);
  _postAudioEvent = msg.postAudioEvent;
  _minStreamingDuration_ms = msg.minStreamingDuration_ms;
  _shouldTriggerWordStartStream = msg.shouldTriggerWordStartStream;
  _shouldTriggerWordSimulateStream = msg.shouldTriggerWordSimulateStream;
  _getInAnimationTag = msg.getInAnimationTag;
  _getInAnimName = std::string(msg.getInAnimationName, msg.getInAnimationName_length);
}

void ShowAudioStreamStateManager::SetPendingTriggerResponseWithGetIn(OnTriggerAudioCompleteCallback callback)
{
  std::lock_guard<std::recursive_mutex> lock(_triggerResponseMutex);
  if(_havePendingTriggerResponse)
  {
    PRINT_NAMED_WARNING("ShowAudioStreamStateManager.SetPendingTriggerResponseWithGetIn.ExistingResponse",
                        "Already have pending trigger response, overriding");
  }
  _havePendingTriggerResponse = true;
  _pendingTriggerResponseHasGetIn = true;
  _responseCallback = callback;
}

void ShowAudioStreamStateManager::SetPendingTriggerResponseWithoutGetIn(OnTriggerAudioCompleteCallback callback)
{
  std::lock_guard<std::recursive_mutex> lock(_triggerResponseMutex);
  if(_havePendingTriggerResponse)
  {
    PRINT_NAMED_WARNING("ShowAudioStreamStateManager.SetPendingTriggerResponseWithoutGetIn.ExistingResponse",
                        "Already have pending trigger response, overriding");
  }
  _havePendingTriggerResponse = true;
  _pendingTriggerResponseHasGetIn = false;
  _responseCallback = callback;
}


void ShowAudioStreamStateManager::StartTriggerResponseWithGetIn(OnTriggerAudioCompleteCallback callback)
{
  if(!HasValidTriggerResponse()){
    if(callback){
      callback(false);
    }
    return;
  }

  auto* anim = _context->GetDataLoader()->GetCannedAnimation(_getInAnimName);
  if((_streamer != nullptr) && (anim != nullptr)){
    _streamer->SetStreamingAnimation(_getInAnimName, _getInAnimationTag);
  }else{
    PRINT_NAMED_ERROR("ShowAudioStreamStateManager.StartTriggerResponseWithGetIn.NoValidGetInAnimation",
                      "Animation not found for get in %s", _getInAnimName.c_str());
  }
  StartTriggerResponseWithoutGetIn(std::move(callback));
}


void ShowAudioStreamStateManager::StartTriggerResponseWithoutGetIn(OnTriggerAudioCompleteCallback callback)
{
  using namespace AudioEngine;

  if(!HasValidTriggerResponse()){
    if(callback){
      callback(false);
    }
    return;
  }

  Audio::CozmoAudioController* controller = _context->GetAudioController();
  if (nullptr != controller) {
    AudioCallbackContext* audioCallbackContext = nullptr;
    if (callback) {
      audioCallbackContext = new AudioCallbackContext();
      audioCallbackContext->SetCallbackFlags( AudioCallbackFlag::Complete );
      audioCallbackContext->SetExecuteAsync( false ); // Execute callbacks synchronously (on main thread)
      audioCallbackContext->SetEventCallbackFunc([callbackFunc = callback]
                                                 (const AudioCallbackContext* thisContext, const AudioCallbackInfo& callbackInfo)
      {
        callbackFunc(true);
      });
    }

    AudioPlayingId result = controller->PostAudioEvent(ToAudioEventId(_postAudioEvent.audioEvent),
                                                       ToAudioGameObject(_postAudioEvent.gameObject),
                                                       audioCallbackContext);

    // if we failed to post the earcon, we still want the callback to be called successfully since we've still
    // completed the get-in process.  the unsuccessful callback is for when no valid response exists ... in this
    // case, it DOES exists, the audio engine is just being an ass right now
    if ( AudioEngine::kInvalidAudioPlayingId == result )
    {
      callback(true);
    }
  }
  else
  {
    // even though we don't have a valid audio controller, we still had a valid trigger response so return true
    if (callback) {
      callback(true);
    }
  }
}


bool ShowAudioStreamStateManager::HasValidTriggerResponse()
{
  std::lock_guard<std::recursive_mutex> lock(_triggerResponseMutex);
  return _postAudioEvent.audioEvent != AudioMetaData::GameEvent::GenericEvent::Invalid;
}


bool ShowAudioStreamStateManager::ShouldStreamAfterTriggerWordResponse()
{
  std::lock_guard<std::recursive_mutex> lock(_triggerResponseMutex);
  return HasValidTriggerResponse() && _shouldTriggerWordStartStream;
}

bool ShowAudioStreamStateManager::ShouldSimulateStreamAfterTriggerWord()
{
  std::lock_guard<std::recursive_mutex> lock(_triggerResponseMutex);
  return HasValidTriggerResponse() && _shouldTriggerWordSimulateStream;
}

uint32_t ShowAudioStreamStateManager::GetMinStreamingDuration()
{
  if( _minStreamingDuration_ms > kUseDefaultStreamingDuration ){
    return _minStreamingDuration_ms;
  }
  else{
    return MicData::kStreamingDefaultMinDuration_ms;
  }
}

} // namespace Vector
} // namespace Anki
