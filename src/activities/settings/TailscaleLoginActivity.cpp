#include "TailscaleLoginActivity.h"

#include <GfxRenderer.h>
#include <Logging.h>
#include <WiFi.h>

#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/TailscaleManager.h"
#include "util/QrUtils.h"

namespace {
constexpr unsigned long REFRESH_MS = 1500;
}

void TailscaleLoginActivity::onEnter() {
  Activity::onEnter();
  WiFi.mode(WIFI_STA);
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelected(!result.isCancelled); });
}

void TailscaleLoginActivity::onExit() {
  // Deliberately NOT tearing Wi-Fi/tailnet down: the whole point is to keep the
  // overlay up so readflix (tailnet-only) stays reachable afterwards.
  Activity::onExit();
}

void TailscaleLoginActivity::onWifiSelected(const bool ok) {
  if (!ok) {
    finish();
    return;
  }
  TailscaleManager::instance().ensureUp();
  requestUpdate();
  lastRefresh_ = millis();
}

void TailscaleLoginActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  // Poll for the login URL / link state and repaint when it changes.
  if (millis() - lastRefresh_ >= REFRESH_MS) {
    lastRefresh_ = millis();
    requestUpdate();
  }
}

void TailscaleLoginActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Tailscale 登入");

  auto& ts = TailscaleManager::instance();
  const std::string url = ts.loginUrl();

  const auto contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const auto centerY = (pageHeight - lineHeight) / 2;

  if (ts.isUp()) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, "Tailscale 已連線", true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, centerY + lineHeight * 2, "可以開始下載書籍");
  } else if (!url.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, contentTop, "用手機掃描這個 QR 登入 Tailscale");
    const int qrTop = contentTop + lineHeight + metrics.verticalSpacing;
    const int qrSize = pageHeight - qrTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 3 - lineHeight * 2;
    const int side = (qrSize < pageWidth - 40) ? qrSize : pageWidth - 40;
    QrUtils::drawQrCode(renderer, Rect{(pageWidth - side) / 2, qrTop, side, side}, url);
    renderer.drawCenteredText(UI_10_FONT_ID, qrTop + side + metrics.verticalSpacing, url.c_str());
  } else {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, "正在連線 Tailscale…");
    renderer.drawCenteredText(UI_10_FONT_ID, centerY + lineHeight * 2, "請先連上 Wi-Fi，稍候會出現 QR");
  }

  const auto labels = mappedInput.mapLabels("返回", "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
