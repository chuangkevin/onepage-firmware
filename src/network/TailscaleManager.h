#pragma once

#include <string>

// Brings up a Tailscale (WireGuard) overlay via MicroLink when a config file
// exists at /.crosspoint/tailscale.json:
//   {"enabled":true,"authKey":"tskey-auth-...","hostname":"onepage"}
//
// Without that file (or with "enabled":false) every method is a no-op, so the
// rest of the firmware behaves exactly as before. No key is ever hardcoded.
//
// MicroLink's init/start is dispatched to a dedicated FreeRTOS task so the UI
// loop never blocks on key generation, NVS access, or control-plane setup.
class TailscaleManager {
 public:
  static TailscaleManager& instance();

  TailscaleManager(const TailscaleManager&) = delete;
  TailscaleManager& operator=(const TailscaleManager&) = delete;

  // Reads /.crosspoint/tailscale.json once. Returns true when a usable config
  // (enabled == true and a non-empty authKey) was loaded. Safe to call
  // repeatedly while the manager has not started.
  bool loadConfig();

  // True once a usable config has been loaded, regardless of link state.
  bool isConfigured() const { return configured_; }

  // Idempotent: loads the config if needed and, when configured, starts the
  // tailnet on its own task. A no-op when unconfigured or already started.
  // Call this only after Wi-Fi is connected.
  void ensureUp();

  // True while MicroLink reports the tailnet is connected.
  bool isUp() const;

  // True once a bring-up attempt failed (e.g. OOM / task create failure).
  bool isFailed() const { return failed_; }

  // The Tailscale interactive login URL, when the control plane has issued one.
  // Only happens in QR/interactive mode (no authKey configured). Empty string
  // when none is available yet or the device is already authorised.
  std::string loginUrl() const;

 private:
  TailscaleManager() = default;

  static void taskEntry(void* arg);
  void run();

  bool configured_ = false;
  bool starting_ = false;
  bool started_ = false;
  bool failed_ = false;

  std::string authKey_;
  std::string hostname_;

  // Opaque MicroLink handle; kept as void* so the C header stays out of this
  // header. Cast back to microlink_t* inside the .cpp.
  void* ml_ = nullptr;
};
