#pragma once

#include <string>

#include "activities/Activity.h"

// Shows the Tailscale interactive-login QR code.
//
// Flow: pick Wi-Fi -> bring the tailnet up via TailscaleManager -> MicroLink
// registers without an auth key -> Tailscale returns a login URL -> we render it
// as a QR code. Scanning it with a phone authorises this device on the tailnet.
// Once the link is up the screen switches to a "connected" message.
class TailscaleLoginActivity final : public Activity {
 public:
  explicit TailscaleLoginActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("TailscaleLogin", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  // The QR must stay on screen until the user scans it.
  bool preventAutoSleep() override { return true; }

 private:
  void onWifiSelected(bool ok);

  unsigned long lastRefresh_ = 0;
  std::string shownPayload_;
};
