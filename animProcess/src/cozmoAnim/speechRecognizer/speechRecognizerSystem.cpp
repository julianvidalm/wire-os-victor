/**
 * File: speechRecognizerSystem.cpp
 *
 * Author: Jordan Rivas
 * Created: 10/23/2018
 *
 * Description: Speech Recognizer System handles high level speech features, such as the "Hey Vector" trigger
 *
 * Copyright: Anki, Inc. 2018
 *
 */

#include "cozmoAnim/speechRecognizer/speechRecognizerSystem.h"

#include "audioUtil/speechRecognizer.h"
#include "speechRecognizerPicovoice.h"                        // swapped in Picovoice
#include "util/logging/logging.h"


namespace Anki {
namespace Vector {

#define LOG_CHANNEL "SpeechRecognizer"

SpeechRecognizerSystem::SpeechRecognizerSystem() = default;

SpeechRecognizerSystem::~SpeechRecognizerSystem()
{
  if (_victorTrigger) {
    _victorTrigger->Stop();
  }
}

void SpeechRecognizerSystem::InitVector(TriggerWordDetectedCallback callback)
{
  if (_victorTrigger) {
    LOG_WARNING("SpeechRecognizerSystem.InitVector", "Victor Recognizer is already running");
    return;
  }

  _victorTrigger = std::make_unique<SpeechRecognizerPicovoice>();
  _victorTrigger->Init();
  _victorTrigger->SetCallback(std::move(callback));
}

void SpeechRecognizerSystem::Update(const AudioUtil::AudioSample * audioData, unsigned int audioDataLen, bool vadActive)
{
  if (_victorTrigger && vadActive) {
    _victorTrigger->Update(audioData, audioDataLen);
  }
}

} // end namespace Vector
} // end namespace Anki
