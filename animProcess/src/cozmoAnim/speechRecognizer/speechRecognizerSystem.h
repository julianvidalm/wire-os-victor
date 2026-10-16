/**
* File: speechRecognizerSystem.h
*
* Author: Jordan Rivas
* Created: 10/23/2018
*
* Description: Speech Recognizer System handles high level speech features, such as the "Hey Vector" trigger
*
* Copyright: Anki, Inc. 2018
*
*/

#ifndef __AnimProcess_VictorAnim_SpeechRecognizerSystem_H_
#define __AnimProcess_VictorAnim_SpeechRecognizerSystem_H_

#include "audioUtil/audioDataTypes.h"
#include <functional>
#include <memory>

namespace Anki {
  namespace AudioUtil {
    struct SpeechRecognizerCallbackInfo;
  }
  namespace Vector {
    class SpeechRecognizerPicovoice;
  }
}

namespace Anki {
namespace Vector {

class SpeechRecognizerSystem
{
public:

  SpeechRecognizerSystem();
  
  ~SpeechRecognizerSystem();
  
  SpeechRecognizerSystem(const SpeechRecognizerSystem& other) = delete;
  SpeechRecognizerSystem& operator=(const SpeechRecognizerSystem& other) = delete;
  
  using TriggerWordDetectedCallback = std::function<void(const AudioUtil::SpeechRecognizerCallbackInfo& info)>;
  
  // Init Vector trigger detector
  // Note: This always happens at boot
  void InitVector(TriggerWordDetectedCallback callback);

  // Update recognizer audio
  // NOTE: Always call from the same thread
  void Update(const AudioUtil::AudioSample * audioData, unsigned int audioDataLen, bool vadActive);

private:

  std::unique_ptr<SpeechRecognizerPicovoice>  _victorTrigger;
};


} // end namespace Vector
} // end namespace Anki

#endif // __AnimProcess_VictorAnim_SpeechRecognizerSystem_H_
