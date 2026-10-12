#include "network/TailscaleManager.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <esp_heap_caps.h>

#include <cstdint>

#include "microlink.h"

namespace {
constexpr const char* TS_CONFIG_PATH = "/.crosspoint/tailscale.json";

const char* stateName(microlink_state_t state) {
  static const char* const names[] = {"IDLE",    "WIFI_WAIT",    "CONNECTING", "REGISTERING",
                                      "CONNECTED", "RECONNECTING", "ERROR"};
  const auto idx = static_cast<size_t>(state);
  return idx < (sizeof(names) / sizeof(names[0])) ? names[idx] : "UNKNOWN";
}

void logHeap(const char* when) {
  LOG_INF("TS", "[MEM] %s: Free: %d bytes, Min Free: %d bytes, MaxAlloc: %d bytes", when, ESP.getFreeHeap(),
          ESP.getMinFreeHeap(), ESP.getMaxAllocHeap());
}

void onStateChange(microlink_t* ml, microlink_state_t state, void* /*userData*/) {
  LOG_DBG("TS", "tailnet state: %s", stateName(state));

  if (state == ML_STATE_CONNECTED) {
    char ip[16];
    microlink_ip_to_str(microlink_get_vpn_ip(ml), ip);
    LOG_INF("TS", "tailnet up, VPN IP: %s", ip);
    logHeap("after tailnet up");
  } else if (state == ML_STATE_ERROR) {
    LOG_ERR("TS", "tailnet reported ERROR");
  }
}

void onPeerUpdate(microlink_t* /*ml*/, const microlink_peer_info_t* peer, void* /*userData*/) {
  char ip[16];
  microlink_ip_to_str(peer->vpn_ip, ip);
  LOG_DBG("TS", "peer %s (%s) online=%d direct=%d", peer->hostname, ip, peer->online, peer->direct_path);
}
}  // namespace

TailscaleManager& TailscaleManager::instance() {
  static TailscaleManager instance;
  return instance;
}

bool TailscaleManager::loadConfig() {
  if (configured_) return true;

  if (!Storage.exists(TS_CONFIG_PATH)) {
    // No config file: default to ENABLED in interactive QR-login mode so the
    // login screen works out of the box (no need to drop a file on the SD
    // card). The file is only needed to set a non-interactive authKey or to
    // disable the overlay. MicroLink/NVS persists the identity after login.
    authKey_.clear();
    hostname_ = "onepage";
    configured_ = true;
    LOG_INF("TS", "no %s; using interactive QR login (default hostname 'onepage')", TS_CONFIG_PATH);
    return true;
  }

  const String json = Storage.readFile(TS_CONFIG_PATH);
  if (json.isEmpty()) {
    LOG_ERR("TS", "failed to read %s", TS_CONFIG_PATH);
    return false;
  }

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, json);
  if (err) {
    LOG_ERR("TS", "tailscale.json parse error: %s", err.c_str());
    return false;
  }

  if (!(doc["enabled"] | false)) {
    LOG_INF("TS", "tailnet disabled in config");
    return false;
  }

  // An empty authKey is valid and is the preferred setup: MicroLink then
  // registers WITHOUT one, Tailscale answers with an interactive login URL
  // (authURL), and the UI shows that URL as a QR code to scan with a phone.
  const char* key = doc["authKey"] | "";
  authKey_ = (key != nullptr) ? key : "";
  hostname_ = doc["hostname"] | "onepage";
  configured_ = true;
  LOG_INF("TS", "tailnet config loaded (hostname=%s, authKey=%u bytes)%s", hostname_.c_str(),
          static_cast<unsigned>(authKey_.size()),
          authKey_.empty() ? " - interactive login (QR) mode" : "");
  return true;
}

void TailscaleManager::ensureUp() {
  if (started_ || starting_) return;
  if (!loadConfig()) return;

  starting_ = true;
  // 8 KB stack: microlink_init() does NVS + several heap allocations and
  // microlink_start() spawns its own long-lived tasks, but neither blocks here.
  // ESP32-C61 is single-core: pin to core 0. Stack is allocated from PSRAM so
  // the bring-up does not compete with Wi-Fi for the small internal SRAM.
  const BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(taskEntry, "ts_bringup", 8 * 1024, this, 4, nullptr, 0,
                                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (ok != pdPASS) {
    LOG_ERR("TS", "failed to create tailnet bring-up task");
    starting_ = false;
  }
}

void TailscaleManager::taskEntry(void* arg) {
  static_cast<TailscaleManager*>(arg)->run();
  vTaskDelete(nullptr);
}

void TailscaleManager::run() {
  logHeap("before tailnet init");

  microlink_config_t config = {};
  config.auth_key = authKey_.c_str();
  config.device_name = hostname_.c_str();
  config.enable_derp = true;
  config.enable_stun = true;
  config.enable_disco = true;
  config.max_peers = 0;         // 0 = MicroLink default (ML_MAX_PEERS)
  config.wifi_tx_power_dbm = 0; // 0 = firmware default

  microlink_t* ml = microlink_init(&config);
  if (ml == nullptr) {
    LOG_ERR("TS", "microlink_init failed");
    starting_ = false;
    failed_ = true;
    return;
  }
  ml_ = ml;

  microlink_set_state_callback(ml, onStateChange, this);
  microlink_set_peer_callback(ml, onPeerUpdate, this);

  const esp_err_t err = microlink_start(ml);
  if (err != ESP_OK) {
    LOG_ERR("TS", "microlink_start failed: %d", static_cast<int>(err));
    starting_ = false;
    failed_ = true;
    return;
  }

  started_ = true;
  starting_ = false;
  LOG_INF("TS", "tailnet bring-up started (state machine running on MicroLink tasks)");
  logHeap("after tailnet start");

  // NOTE: MicroLink keeps its own tasks alive; this bring-up task ends here.
}

bool TailscaleManager::isUp() const {
  if (ml_ == nullptr) return false;
  return microlink_is_connected(static_cast<const microlink_t*>(ml_));
}

std::string TailscaleManager::loginUrl() const {
  if (ml_ == nullptr) return {};
  const char* url = microlink_get_login_url(static_cast<const microlink_t*>(ml_));
  return (url != nullptr) ? std::string(url) : std::string();
}
