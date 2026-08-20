#include "core/storage.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <Windows.h>

#include <nlohmann/json.hpp>
#include <sodium.h>

#include "core/account.h"
#include "core/atomic_file.h"
#include "core/crypto.h"
#include "core/string_view.h"

namespace {
constexpr int kFormatVersion = 1;
constexpr const char *kAccountsFileName = "accounts.vault";
constexpr const char *kSettingsFileName = "settings.json";

// Builds %LOCALAPPDATA%\Rift into pBuffer, creating it (and running the one-time migration
// below) if it doesn't exist yet - the shared root GetStorageFilePath resolves a filename
// under, and also CStorage::GetDataDirectory's own public path for anything outside this
// file that needs it (e.g. core/crash_handler.cpp's crash dump folder, or a user-facing
// "open my data folder" action).
bool GetStorageDirectory(char *pBuffer, std::size_t bufferSize)
{
	char localAppData[MAX_PATH];
	const DWORD length = GetEnvironmentVariableA("LOCALAPPDATA", localAppData, sizeof(localAppData));
	if (length == 0 || length >= sizeof(localAppData)) {
		return false;
	}

	char dir[MAX_PATH];
	std::snprintf(dir, sizeof(dir), "%s\\Rift", localAppData);

	// One-time migration from this app's former name (f4-rockstar) - a plain directory
	// rename, not a copy, so an existing user's saved accounts/settings keep working under
	// the new folder instead of silently vanishing behind a renamed lookup path. Only runs
	// while the new directory doesn't exist yet and the old one does, so every later
	// launch just falls through as a no-op.
	if (GetFileAttributesA(dir) == INVALID_FILE_ATTRIBUTES) {
		char oldDir[MAX_PATH];
		std::snprintf(oldDir, sizeof(oldDir), "%s\\f4-rockstar", localAppData);
		if (GetFileAttributesA(oldDir) != INVALID_FILE_ATTRIBUTES) {
			MoveFileA(oldDir, dir);
		}
	}

	CreateDirectoryA(dir, nullptr); // ok if it already exists (or the migration above just created it)

	const int written = std::snprintf(pBuffer, bufferSize, "%s", dir);
	return written > 0 && static_cast<std::size_t>(written) < bufferSize;
}

// Builds %LOCALAPPDATA%\Rift\<pFileName> into pBuffer - see GetStorageDirectory for the
// directory part (creation, migration) this just appends the filename onto.
bool GetStorageFilePath(const char *pFileName, char *pBuffer, std::size_t bufferSize)
{
	char dir[MAX_PATH];
	if (!GetStorageDirectory(dir, sizeof(dir))) {
		return false;
	}

	const int written = std::snprintf(pBuffer, bufferSize, "%s\\%s", dir, pFileName);
	return written > 0 && static_cast<std::size_t>(written) < bufferSize;
}

std::string BytesToHex(const std::uint8_t *pData, std::size_t length)
{
	std::string out(length * 2 + 1, '\0');
	sodium_bin2hex(out.data(), out.size(), pData, length);
	out.resize(length * 2); // drop the trailing nul sodium_bin2hex wrote within out.size()
	return out;
}

// False (pOut left untouched) if hex doesn't decode to exactly outLength bytes - a missing
// or empty key (the common "this field doesn't exist yet in an older file" case) hits this
// path and simply leaves whatever pOut already held, matching this format's own "a missing
// key is a no-op, not a failure" contract.
bool HexToBytes(const std::string &hex, std::uint8_t *pOut, std::size_t outLength)
{
	if (hex.size() != outLength * 2) {
		return false;
	}
	std::size_t decodedLength = 0;
	return sodium_hex2bin(pOut, outLength, hex.c_str(), hex.size(), nullptr, &decodedLength, nullptr) == 0 &&
		  decodedLength == outLength;
}

std::string BytesToBase64(const std::uint8_t *pData, std::size_t length)
{
	const std::size_t encodedLength = sodium_base64_ENCODED_LEN(length, sodium_base64_VARIANT_ORIGINAL);
	std::string out(encodedLength, '\0');
	sodium_bin2base64(out.data(), out.size(), pData, length, sodium_base64_VARIANT_ORIGINAL);
	out.resize(std::strlen(out.c_str())); // sodium_bin2base64 nul-terminates within out.size()
	return out;
}

bool Base64ToBytes(const std::string &base64, std::vector<std::uint8_t> &outBytes)
{
	outBytes.resize(base64.size()); // a safe upper bound - base64 never decodes to more bytes than its own length
	std::size_t decodedLength = 0;
	if (sodium_base642bin(outBytes.data(), outBytes.size(), base64.c_str(), base64.size(), nullptr, &decodedLength,
						  nullptr, sodium_base64_VARIANT_ORIGINAL) != 0) {
		return false;
	}
	outBytes.resize(decodedLength);
	return true;
}

bool WriteTextFile(const char *pPath, const std::string &text)
{
	return CAtomicFile::WriteAtomic(pPath, text.data(), text.size());
}

// Tries pPath first, then its `.bak` sibling - see storage.h's own "Durability" section.
// False if neither parses as a JSON object at all (missing file, corrupted bytes, or
// genuinely not JSON) - a parse failure is caught here, not left to propagate as an
// exception, since a corrupted file is an expected, recoverable condition in this class,
// not a programming error.
bool ReadJsonWithFallback(const char *pPath, nlohmann::json &outJson)
{
	const auto TryParse = [&](const char *pTryPath) -> bool {
		std::uint8_t *pData = nullptr;
		std::size_t length = 0;
		if (!CAtomicFile::ReadFile(pTryPath, &pData, &length)) {
			return false;
		}
		bool ok = false;
		try {
			outJson = nlohmann::json::parse(pData, pData + length);
			ok = outJson.is_object();
		} catch (const nlohmann::json::exception &) {
			ok = false;
		}
		std::free(pData);
		return ok;
	};

	if (TryParse(pPath)) {
		return true;
	}
	char backupPath[MAX_PATH];
	return CAtomicFile::BackupPathFor(pPath, backupPath, sizeof(backupPath)) && TryParse(backupPath);
}

// CAccount::Init asserts a field fits its fixed buffer rather than truncating (see its own
// comment) - a no-op assert in a Release build, so an over-length decrypted string would
// otherwise overflow the buffer outright. Successful AEAD decryption already authenticates
// accounts.vault's contents as genuinely written by this same code, but that's a guarantee
// about *tampering*, not about a future bug in this file's own serialization - clamping
// here costs nothing and removes the dependency on that reasoning being airtight forever.
CStringView TruncatedView(const std::string &value, std::uint64_t maxLength)
{
	return CStringView{value.data(), std::min<std::uint64_t>(value.size(), maxLength)};
}
} // namespace

bool CStorage::GetDataDirectory(char *pBuffer, std::size_t bufferSize)
{
	return GetStorageDirectory(pBuffer, bufferSize);
}

bool CStorage::SaveSettings(const CSettings &settings, std::int32_t carouselZoomStop)
{
	nlohmann::json j;
	j["format_version"] = kFormatVersion;
	j["animations_enabled"] = settings.m_bAnimationsEnabled;
	j["animation_speed"] = settings.m_flAnimationSpeed;
	j["rounded_corners_enabled"] = settings.m_bRoundedCornersEnabled;
	j["font_pixel_size"] = settings.m_flFontPixelSize;
	j["secondary_font_pixel_size"] = settings.m_flSecondaryFontPixelSize;
	j["accent_color"] = {settings.m_clrAccent.R, settings.m_clrAccent.G, settings.m_clrAccent.B,
						 settings.m_clrAccent.A};
	j["font_name"] = std::string(settings.m_szFontName);
	j["exclude_account_list_from_capture"] = settings.m_bExcludeAccountListFromCapture;
	j["carousel_zoom_stop"] = carouselZoomStop;

	// Not itself secret - see this file's own header comment on why settings.json is
	// never encrypted at the file level.
	nlohmann::json masterPassword;
	masterPassword["enabled"] = settings.m_bMasterPasswordEnabled;
	masterPassword["salt_hex"] = BytesToHex(settings.m_aMasterPasswordSalt, sizeof(settings.m_aMasterPasswordSalt));
	masterPassword["ops_limit"] = settings.m_masterPasswordOpsLimit;
	masterPassword["mem_limit"] = static_cast<std::uint64_t>(settings.m_masterPasswordMemLimit);
	masterPassword["wrap_nonce_hex"] =
		BytesToHex(settings.m_aMasterPasswordWrapNonce, sizeof(settings.m_aMasterPasswordWrapNonce));
	masterPassword["wrapped_dek_hex"] =
		BytesToHex(settings.m_aMasterPasswordWrappedDek, sizeof(settings.m_aMasterPasswordWrappedDek));
	j["master_password"] = std::move(masterPassword);

	char path[MAX_PATH];
	return GetStorageFilePath(kSettingsFileName, path, sizeof(path)) && WriteTextFile(path, j.dump(2));
}

EStorageLoadResult CStorage::LoadSettings(CSettings &settings, std::int32_t &outCarouselZoomStop)
{
	char path[MAX_PATH];
	if (!GetStorageFilePath(kSettingsFileName, path, sizeof(path))) {
		return EStorageLoadResult::STORAGE_LOAD_FAILED;
	}

	FILE *pProbe = nullptr;
	const bool fileExists = fopen_s(&pProbe, path, "rb") == 0 && pProbe != nullptr;
	if (pProbe != nullptr) {
		std::fclose(pProbe);
	}

	nlohmann::json j;
	if (!ReadJsonWithFallback(path, j)) {
		return fileExists ? EStorageLoadResult::STORAGE_LOAD_FAILED : EStorageLoadResult::STORAGE_LOAD_NO_FILE;
	}

	settings.m_bAnimationsEnabled = j.value("animations_enabled", settings.m_bAnimationsEnabled);
	settings.m_flAnimationSpeed = j.value("animation_speed", settings.m_flAnimationSpeed);
	settings.m_bRoundedCornersEnabled = j.value("rounded_corners_enabled", settings.m_bRoundedCornersEnabled);
	settings.m_flFontPixelSize = j.value("font_pixel_size", settings.m_flFontPixelSize);
	settings.m_flSecondaryFontPixelSize = j.value("secondary_font_pixel_size", settings.m_flSecondaryFontPixelSize);

	if (j.contains("accent_color") && j["accent_color"].is_array() && j["accent_color"].size() == 4) {
		const nlohmann::json &a = j["accent_color"];
		settings.m_clrAccent = Color{a[0].get<std::uint8_t>(), a[1].get<std::uint8_t>(), a[2].get<std::uint8_t>(),
									 a[3].get<std::uint8_t>()};
	}

	const std::string fontName = j.value("font_name", std::string(settings.m_szFontName));
	const std::uint64_t fontNameLength =
		std::min<std::uint64_t>(fontName.size(), sizeof(settings.m_szFontName) - 1);
	std::memcpy(settings.m_szFontName, fontName.data(), fontNameLength);
	settings.m_szFontName[fontNameLength] = '\0';

	settings.m_bExcludeAccountListFromCapture =
		j.value("exclude_account_list_from_capture", settings.m_bExcludeAccountListFromCapture);
	outCarouselZoomStop = j.value("carousel_zoom_stop", outCarouselZoomStop);

	settings.m_bMasterPasswordEnabled = false;
	if (j.contains("master_password") && j["master_password"].is_object()) {
		const nlohmann::json &mp = j["master_password"];
		settings.m_bMasterPasswordEnabled = mp.value("enabled", false);
		HexToBytes(mp.value("salt_hex", std::string()), settings.m_aMasterPasswordSalt,
				  sizeof(settings.m_aMasterPasswordSalt));
		HexToBytes(mp.value("wrap_nonce_hex", std::string()), settings.m_aMasterPasswordWrapNonce,
				  sizeof(settings.m_aMasterPasswordWrapNonce));
		HexToBytes(mp.value("wrapped_dek_hex", std::string()), settings.m_aMasterPasswordWrappedDek,
				  sizeof(settings.m_aMasterPasswordWrappedDek));
		settings.m_masterPasswordOpsLimit = mp.value("ops_limit", static_cast<std::uint64_t>(0));
		settings.m_masterPasswordMemLimit =
			static_cast<std::size_t>(mp.value("mem_limit", static_cast<std::uint64_t>(0)));
	}

	return EStorageLoadResult::STORAGE_LOAD_OK;
}

bool CStorage::SaveAccounts(CBanner *pBanners, std::uint32_t bannerCount, bool masterPasswordEnabled,
						   const CMasterKey &masterKey)
{
	// Nothing trustworthy to encrypt while locked - accounts.vault is never even read in
	// that state (see LoadAccounts), so there's nothing real in pBanners to save over it
	// either. masterPasswordEnabled false shouldn't reach here at all (the master password
	// is mandatory - see ui/unlock_screen.h), but is refused the same way regardless.
	if (!masterPasswordEnabled || !masterKey.m_bEnabled) {
		return false;
	}

	nlohmann::json banners = nlohmann::json::array();
	for (std::uint32_t i = 0; i < bannerCount; i += 1) {
		const CBanner &banner = pBanners[i];

		nlohmann::json accounts = nlohmann::json::array();
		for (std::uint32_t a = 0; a < banner.AccountCount; a += 1) {
			const CAccount &account = banner.Accounts[a];
			nlohmann::json acc;
			acc["username"] = std::string(account.m_szUsername);
			acc["note"] = std::string(account.m_szNote);
			acc["password"] = std::string(account.m_szPassword);
			acc["visible_mask"] = account.m_uVisibleBannerMask;
			accounts.push_back(std::move(acc));
		}

		nlohmann::json b;
		b["title"] = std::string(banner.Title.pData, banner.Title.Length);
		b["accounts"] = std::move(accounts);
		banners.push_back(std::move(b));
	}

	const std::string plaintext = banners.dump();

	std::uint8_t nonce[CCrypto::kNonceSize];
	CCrypto::RandomBytes(nonce, sizeof(nonce));

	std::vector<std::uint8_t> ciphertext(plaintext.size());
	std::uint8_t tag[CCrypto::kTagSize];
	if (!CCrypto::Encrypt(masterKey.m_aDek, nonce, reinterpret_cast<const std::uint8_t *>(plaintext.data()),
						  static_cast<std::uint32_t>(plaintext.size()), ciphertext.data(), tag)) {
		return false;
	}

	nlohmann::json envelope;
	envelope["format_version"] = kFormatVersion;
	envelope["nonce_hex"] = BytesToHex(nonce, sizeof(nonce));
	envelope["tag_hex"] = BytesToHex(tag, sizeof(tag));
	envelope["ciphertext_b64"] = BytesToBase64(ciphertext.data(), ciphertext.size());

	char path[MAX_PATH];
	return GetStorageFilePath(kAccountsFileName, path, sizeof(path)) && WriteTextFile(path, envelope.dump());
}

EStorageLoadResult CStorage::LoadAccounts(CBanner *pBanners, std::uint32_t bannerCount, bool masterPasswordEnabled,
										 const CMasterKey &masterKey)
{
	// The whole point of this format - see storage.h's own "Why accounts.vault waits for
	// unlock" section: never even attempt to read the file, let alone parse or decrypt it,
	// without the key. pBanners is left completely untouched.
	if (!masterPasswordEnabled || !masterKey.m_bEnabled) {
		return EStorageLoadResult::STORAGE_LOAD_LOCKED;
	}

	char path[MAX_PATH];
	if (!GetStorageFilePath(kAccountsFileName, path, sizeof(path))) {
		return EStorageLoadResult::STORAGE_LOAD_FAILED;
	}

	FILE *pProbe = nullptr;
	const bool fileExists = fopen_s(&pProbe, path, "rb") == 0 && pProbe != nullptr;
	if (pProbe != nullptr) {
		std::fclose(pProbe);
	}

	nlohmann::json envelope;
	if (!ReadJsonWithFallback(path, envelope)) {
		return fileExists ? EStorageLoadResult::STORAGE_LOAD_FAILED : EStorageLoadResult::STORAGE_LOAD_NO_FILE;
	}

	std::uint8_t nonce[CCrypto::kNonceSize];
	std::uint8_t tag[CCrypto::kTagSize];
	std::vector<std::uint8_t> ciphertext;
	if (!HexToBytes(envelope.value("nonce_hex", std::string()), nonce, sizeof(nonce)) ||
		!HexToBytes(envelope.value("tag_hex", std::string()), tag, sizeof(tag)) ||
		!Base64ToBytes(envelope.value("ciphertext_b64", std::string()), ciphertext)) {
		return EStorageLoadResult::STORAGE_LOAD_FAILED;
	}

	std::vector<std::uint8_t> plaintext(ciphertext.size());
	if (!CCrypto::Decrypt(masterKey.m_aDek, nonce, ciphertext.data(), static_cast<std::uint32_t>(ciphertext.size()),
						  tag, plaintext.data())) {
		return EStorageLoadResult::STORAGE_LOAD_FAILED; // wrong key, or corrupted/tampered ciphertext
	}

	nlohmann::json banners;
	try {
		banners = nlohmann::json::parse(plaintext.begin(), plaintext.end());
	} catch (const nlohmann::json::exception &) {
		return EStorageLoadResult::STORAGE_LOAD_FAILED;
	}
	if (!banners.is_array()) {
		return EStorageLoadResult::STORAGE_LOAD_FAILED;
	}

	for (const nlohmann::json &b : banners) {
		const std::string title = b.value("title", std::string());

		CBanner *pMatch = nullptr;
		for (std::uint32_t i = 0; i < bannerCount; i += 1) {
			if (StringViewEqual(pBanners[i].Title, StringViewFromCString(title.c_str()))) {
				pMatch = &pBanners[i];
				break;
			}
		}
		if (pMatch == nullptr) {
			continue; // a banner in the file no longer exists in this build - skip it
		}

		pMatch->AccountCount = 0;
		if (!b.contains("accounts") || !b["accounts"].is_array()) {
			continue;
		}
		for (const nlohmann::json &acc : b["accounts"]) {
			if (pMatch->AccountCount >= kCarouselMaxAccountsPerBanner) {
				break;
			}
			const std::string username = acc.value("username", std::string());
			const std::string note = acc.value("note", std::string());
			const std::string password = acc.value("password", std::string());

			CAccount &out = pMatch->Accounts[pMatch->AccountCount];
			out.Init(TruncatedView(username, sizeof(out.m_szUsername) - 1),
					TruncatedView(note, sizeof(out.m_szNote) - 1),
					TruncatedView(password, sizeof(out.m_szPassword) - 1));
			out.m_uVisibleBannerMask = acc.value("visible_mask", static_cast<std::uint16_t>(0));
			pMatch->AccountCount += 1;
		}
	}

	return EStorageLoadResult::STORAGE_LOAD_OK;
}
