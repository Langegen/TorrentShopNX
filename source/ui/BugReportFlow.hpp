#pragma once
// "Report a problem": the dialogs around report::buildReport, started from
// Settings -> "Report a problem". Two ways to send, both started by the user:
//   - startSend(): upload to the report server, which posts the ZIP to the
//     developer's chat signed with the user's Telegram @username;
//   - startShare(): download the ZIP to a phone (QR code -> page served by
//     the console) and post it to the bugs topic from the user's own account.

#include <borealis.hpp>
#include <string>

namespace ui::bugreport {

// The bugs topic of the Telegram group.
inline constexpr const char* kBugsTopicUrl = "https://t.me/TorrentShopNX/553";

// The user's description, kept for the whole session so that leaving and
// reopening Settings does not lose it. It goes into info.txt.
const std::string& message();
void setMessage(const std::string& text);

// Consent dialog, then build + upload, then the report code. Falls back to
// startShare() when sending is not set up or fails.
void startSend();
// Build, then a QR code for downloading the ZIP to a phone or PC.
void startShare();
// Build and tell where the ZIP is on the SD card.
void startSave();

} // namespace ui::bugreport
