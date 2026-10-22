/***********************************************************************************************************************
 *
 *  BehaviorKnowledgeGraphQuestion
 *  Victor / Engine
 *
 *  Created by Jarrod Hatfield on 5/09/2018
 *
 *  Description
 *  + Parent behavior requesting a knowledge graph inquiry.
 *  + Streams a users voice (assumed to be a question for Victor) to the cloud which returns a response/answer string
 *
 **********************************************************************************************************************/

#ifndef __Cozmo_Basestation_Behaviors_BehaviorKnowledgeGraphQuestion_H__
#define __Cozmo_Basestation_Behaviors_BehaviorKnowledgeGraphQuestion_H__

#include "engine/aiComponent/behaviorComponent/behaviors/iCozmoBehavior.h"
#include "engine/components/mics/voiceMessageTypes.h"
#include "engine/components/backpackLights/engineBackpackLightComponentTypes.h"
#include "components/textToSpeech/textToSpeechWrapper.h"
#include "clad/audio/audioEventTypes.h"

namespace Anki
{
  namespace Vector
  {

    class BehaviorTextToSpeechLoop;

    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    class BehaviorKnowledgeGraphQuestion : public ICozmoBehavior
    {
      friend class BehaviorFactory;
      BehaviorKnowledgeGraphQuestion(const Json::Value &config);

    public:
      // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
      // Overrides from ICozmoBehavior

      virtual bool WantsToBeActivatedBehavior() const override;
      virtual void GetBehaviorOperationModifiers(BehaviorOperationModifiers &modifiers) const override;
      virtual void GetBehaviorJsonKeys(std::set<const char *> &expectedKeys) const override;

      // Exposed for tests: this activation shows the talking face for the cortex's own voice, and has ended it
      bool IsExternalVoice() const { return _dVars.externalVoice; }
      bool IsExternalVoiceDone() const { return _dVars.externalVoiceDone; }

    protected:
      // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
      // Overrides from ICozmoBehavior

      virtual void InitBehavior() override;
      virtual void GetAllDelegates(std::set<IBehavior *> &delegates) const override;

      virtual void OnBehaviorActivated() override;
      virtual void OnBehaviorDeactivated() override;
      virtual void BehaviorUpdate() override;

      virtual void HandleWhileActivated(const EngineToGameEvent &event) override;
      virtual void HandleWhileActivated(const RobotToEngineEvent &event) override;

      // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
      // State Transitions

      void TransitionToSearchingLoop();
      void TransitionToBeginResponse();
      void TransitionToNoResponse();

      // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
      // Helpers

      // start streaming mic data to knowledge graph
      void BeginStreamingQuestion();
      // speak back the response we got from knowledge graph
      void BeginResponseTTS();

      // we have a response intent pending from knowledgeGraph
      bool IsResponsePending() const;
      // translate that response intent into and answer string and store it for later
      void ConsumeResponse();

      // translate that response intent into an answer string and store it for later
      // if it was a knowledge graph response
      void ConsumeIntentGraphResponse();

      // we're done listening to the question
      void OnStreamingComplete(bool);
      // we allow the response audio to be interrupted under certain conditions
      void OnResponseInterrupted();

      // streaming cues are backpack lights and audio cues to let the user know when to speak
      void PlayEarconEnd();

      // external voice: the PC cortex streams the spoken answer through the SDK external audio stream, so
      // we only show the talking face (TTS get-in, loop, get-out) until the stream ends or times out
      bool IsExternalVoiceAnswer() const;
      void TransitionToExternalVoice();
      void TransitionToExternalVoiceLoop();
      void UpdateExternalVoice();
      void FinishExternalVoice();
      double GetExternalVoiceDeadline() const;

    private:
      // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

      enum class EState : uint8_t
      {
        GettingIn,
        WaitingToStream,
        Listening,
        Searching,
        Responding,
        NoResponse,
        NoConnection,
        Interrupted
      };

      enum class EGenerationStatus
      {
        None,
        Success,
        Fail
      };

      // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
      // Instance Vars are members that last the lifetime of the behavior and generally do not change (config vars)

      struct InstanceConfig
      {
        InstanceConfig();

        double streamingDuration;  // how long before we give up on streaming and bail
        std::string readyStringID; // audio tts to let the user know they can begin speaking
        std::shared_ptr<BehaviorTextToSpeechLoop> ttsBehavior;

        AudioMetaData::GameEvent::GenericEvent earConEnd;

        // external voice timeouts: margin after the expected end or the last chunk; first chunk wait when the
        // length is unknown; hard cap
        double externalVoiceMargin_s;
        double externalVoiceFirstAudio_s;
        double externalVoiceMax_s;

      } _iVars;

      // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
      // Dynamic Vars are members that change over the lifetime of the behavior and are generally reset every activation

      struct DynamicVariables
      {
        DynamicVariables();

        EState state;
        double streamingBeginTime;             // the time we begun actually streaming the mic data
        std::string responseString;            // the text we got back from knowledge graph
        EGenerationStatus ttsGenerationStatus; // track the status of the response tts
        BackpackLightDataLocator lightsHandle; // lights, camera, action!
        bool wasPickedUp;

        bool externalVoice;          // this answer is spoken by the cortex's audio stream
        bool externalVoiceDone;      // the talking face is getting out
        bool externalAudioOwned;     // the stream may still play and is ours to cancel on interruption
        double externalVoiceStart_s;
        double lastChunk_s;          // last chunk the robot reported adding to the stream, 0 if none
        uint32_t expectedDuration_ms;

      } _dVars;

      TextToSpeechWrapper _readyTTSWrapper;

    }; // class BehaviorKnowledgeGraphQuestion

  } // namespace Vector
} // namespace Anki

#endif // __Cozmo_Basestation_Behaviors_BehaviorKnowledgeGraphQuestion_H__
