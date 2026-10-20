/**
 * File: behaviorCortexListening.h
 *
 * Created: 2026-10-19
 *
 * Description: Silent "I'm listening" reaction for the PC cortex. Responds to the cortex_listening user
 * intent (sent by the cortex when it hears the wake word), plays a silent eye pose and holds it while the
 * cortex captures the user's speech. It ends, without any post-behavior suggestion, when:
 *   - any other user intent becomes pending (e.g. the cortex's answer). That intent is left pending so the
 *     behavior that handles it can claim it right after.
 *   - the cortex_listening_end user intent arrives (consumed silently).
 *   - a safety timeout expires.
 * A stray cortex_listening_end (e.g. after this behavior was interrupted) is also consumed silently, so it
 * never counts as an unclaimed intent.
 *
 **/

#ifndef __Engine_AiComponent_BehaviorComponent_Behaviors_BehaviorCortexListening__
#define __Engine_AiComponent_BehaviorComponent_Behaviors_BehaviorCortexListening__
#pragma once

#include "engine/aiComponent/behaviorComponent/behaviors/iCozmoBehavior.h"

namespace Anki {
namespace Vector {

class BehaviorCortexListening : public ICozmoBehavior
{
public:
  virtual ~BehaviorCortexListening();

  // Why the behavior last ended (exposed for tests and logs)
  enum class EndReason {
    None,
    NextIntent,    // another user intent became pending
    ListeningEnd,  // cortex_listening_end arrived while listening
    StrayEnd,      // cortex_listening_end arrived while not listening
    Timeout,       // the safety timeout expired
  };

  EndReason GetLastEndReason() const { return _lastEndReason; }

protected:

  // Enforce creation through BehaviorFactory
  friend class BehaviorFactory;
  explicit BehaviorCortexListening(const Json::Value& config);

  virtual void GetBehaviorOperationModifiers(BehaviorOperationModifiers& modifiers) const override;
  virtual void GetBehaviorJsonKeys(std::set<const char*>& expectedKeys) const override;

  virtual bool WantsToBeActivatedBehavior() const override { return true; }
  virtual void OnBehaviorActivated() override;
  virtual void BehaviorUpdate() override;

private:

  struct InstanceConfig {
    std::string animName;
    float timeout_s;
  };

  struct DynamicVariables {
    float listeningStart_s = 0.0f;
  };

  void EndListening(EndReason reason);

  InstanceConfig _iConfig;
  DynamicVariables _dVars;
  EndReason _lastEndReason = EndReason::None;
};

} // namespace Vector
} // namespace Anki

#endif // __Engine_AiComponent_BehaviorComponent_Behaviors_BehaviorCortexListening__
