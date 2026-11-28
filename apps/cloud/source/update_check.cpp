//
// cloud: store update check.
//

#include "cloud/update_check.hpp"

#include <algorithm>
#include <cstdlib>
#include <vector>

#include <borealis.hpp>
#include <cpr/async.h>

#include "nxkit/http_client.hpp"

#ifndef NXKIT_APP_VERSION
#define NXKIT_APP_VERSION "unknown"
#endif

using namespace brls::literals;

namespace cloud {

namespace {

constexpr const char* kStoreBaseUrl = "https://ecfun.cc";
constexpr const char* kStoreAppPath = "/api/v1/apps/cloud";

std::vector<int> parseVersion(const std::string& text) {
    std::vector<int> parts;
    std::string digits;
    for (char c : text) {
        if (c >= '0' && c <= '9') {
            digits.push_back(c);
        } else if (c == '.') {
            parts.push_back(digits.empty() ? 0 : std::atoi(digits.c_str()));
            digits.clear();
        } else {
            break;  // stop at suffixes like "-beta"
        }
    }
    if (!digits.empty()) parts.push_back(std::atoi(digits.c_str()));
    return parts;
}

}  // namespace

bool isNewerVersion(const std::string& latest, const std::string& current) {
    std::vector<int> newer = parseVersion(latest);
    std::vector<int> running = parseVersion(current);
    size_t count = std::max(newer.size(), running.size());
    for (size_t i = 0; i < count; i++) {
        int a = i < newer.size() ? newer[i] : 0;
        int b = i < running.size() ? running[i] : 0;
        if (a != b) return a > b;
    }
    return false;
}

void checkForUpdate(
    std::function<void(bool, const UpdateInfo&, const std::string&)> callback) {
    cpr::async([callback = std::move(callback)] {
        nxkit::HttpClient client;
        client.setBaseUrl(kStoreBaseUrl);
        client.setTimeout(8000);

        nxkit::HttpResult result = client.get(kStoreAppPath);
        if (!result.ok) {
            callback(false, UpdateInfo(),
                     result.error.empty() ? "network error" : result.error);
            return;
        }

        nlohmann::json body = result.json();
        if (!body.is_object() || !body.contains("latest_version")) {
            callback(false, UpdateInfo(), "invalid response");
            return;
        }

        const nlohmann::json& latest = body["latest_version"];
        UpdateInfo info;
        info.version = latest.value("version", std::string());
        info.changelog = latest.value("changelog", std::string());
        info.size = latest.value("size", static_cast<uint64_t>(0));
        info.available =
            !info.version.empty() && isNewerVersion(info.version, NXKIT_APP_VERSION);
        callback(true, info, std::string());
    });
}

void presentUpdateResult(const UpdateInfo& info, bool ok,
                         const std::string& error, bool manual) {
    if (!ok) {
        if (!manual) return;
        brls::Dialog* dialog = new brls::Dialog(
            "cloud/update/failed"_i18n + "\n" + error);
        dialog->addButton("cloud/profile/ok"_i18n, [] {});
        dialog->open();
        return;
    }
    if (info.available) {
        // The stock text dialog does not constrain its label, so a paragraph
        // long enough (a changelog) is drawn as one line that runs off the
        // dialog. Build the content by hand and give the label a real width so
        // it wraps.
        brls::Theme theme = brls::Application::getTheme();
        brls::Box* content = new brls::Box(brls::Axis::COLUMN);
        content->setWidth(720);
        content->setPadding(26, 34, 26, 34);
        content->setAlignItems(brls::AlignItems::STRETCH);

        brls::Label* title = new brls::Label();
        title->setText("cloud/update/found"_i18n + " " + info.version);
        title->setFontSize(22);
        title->setWidthPercentage(100);
        title->setSingleLine(false);
        title->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        content->addView(title);

        if (!info.changelog.empty()) {
            brls::Label* body = new brls::Label();
            body->setText(info.changelog);
            body->setFontSize(16);
            body->setLineHeight(1.5f);
            body->setWidthPercentage(100);
            body->setSingleLine(false);
            body->setMarginTop(14);
            content->addView(body);
        }

        brls::Label* hint = new brls::Label();
        hint->setText("cloud/update/hint"_i18n);
        hint->setFontSize(14);
        hint->setTextColor(theme["cloud/text_secondary"]);
        hint->setWidthPercentage(100);
        hint->setSingleLine(false);
        hint->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        hint->setMarginTop(16);
        content->addView(hint);

        brls::Dialog* dialog = new brls::Dialog(content);
        dialog->addButton("cloud/profile/ok"_i18n, [] {});
        dialog->open();
        return;
    }
    if (!manual) return;
    brls::Dialog* dialog = new brls::Dialog(
        "cloud/update/up_to_date"_i18n + " " + NXKIT_APP_VERSION);
    dialog->addButton("cloud/profile/ok"_i18n, [] {});
    dialog->open();
}

}  // namespace cloud
