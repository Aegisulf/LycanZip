#pragma once
// The update feed of the installed form of LycanZip. The installed program updates itself from a feed (releases.win.json and the packages it lists,
// made by the release flow). The program does not trust the web server: the feed carries our own signature (an ECDSA P-256 signature
// of the feed file, made with a key that only the release computer has), and every package is checked against the SHA-256
// that the signed feed lists. A feed or package that was changed on the way, or on the server, is refused.
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace lycan {

// The public key that signs the feed: X || Y of the P-256 point, 64 bytes. (In a debug build LYCAN_TEST_UPDATE_PUBKEY, 128 hex digits, replaces it: tests.)
using UpdatePublicKey = std::array<uint8_t, 64>;
UpdatePublicKey GetUpdatePublicKey();

// Is `signatureBase64` (r || s of an ECDSA P-256 signature, 64 bytes, in base64) the signature of SHA-256(feed) by the key? Anything wrong with the input is false.
bool VerifyFeedSignature(const std::string& feed, const std::string& signatureBase64, const UpdatePublicKey& key);

// The lower case hex SHA-256 of a file; empty when it cannot be read.
std::string FileSha256Hex(const std::filesystem::path& file);

// Where the feed is: https://aegisulf.com/lycanzip/updates/ (LYCAN_UPDATE_FEED replaces it: tests, a server on this computer; http only for localhost).
std::wstring UpdateFeedBase();

// One GET. The answer is at most maxBytes; empty on any failure (no network, a status other than 200, too long). Short timeouts, no cookies, no redirects to other hosts.
std::optional<std::string> HttpGetBytes(const std::wstring& url, size_t maxBytes);
// One GET to a file (the file is only complete when true is returned; a part is deleted). progress(got, total) (total 0 = not known) may return false to cancel.
bool HttpDownloadFile(const std::wstring& url, const std::filesystem::path& to, uint64_t maxBytes, const std::function<bool(uint64_t, uint64_t)>& progress);

// The name of a package in the feed is a plain file name (no folders, no ..), so that it can never point outside of the feed or the packages folder.
bool IsPlainPackageName(const std::string& name);

// The feed: the file and its signature, verified. Empty when the file is missing, too big, or the signature does not match.
std::optional<std::string> FetchSignedFeed(const std::wstring& base, const std::string& name, const UpdatePublicKey& key);

}  // namespace lycan
