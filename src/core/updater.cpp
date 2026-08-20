#include "core/updater.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <vector>

#include <Windows.h>
#include <winhttp.h>

#include <sodium.h>

#include "core/semver.h"
#include "core/string_view.h"
#include "core/thread_util.h"
#include "core/update_signing_key.h"

namespace {

constexpr wchar_t kManifestUrl[] = L"https://github.com/xi94/Rift/releases/latest/download/update.json";
constexpr wchar_t kUserAgent[] = L"Rift-Updater/1.0";

struct HInternetHandle {
	HINTERNET Handle = nullptr;
	~HInternetHandle()
	{
		if (Handle != nullptr) {
			WinHttpCloseHandle(Handle);
		}
	}
	operator HINTERNET() const
	{
		return Handle;
	}
};

struct CrackedUrl {
	std::wstring Host;
	std::wstring PathAndQuery;
	INTERNET_PORT Port = 0;
	bool Https = false;
};

bool CrackUrl(const wchar_t *pUrl, CrackedUrl &out)
{
	wchar_t host[256]{};
	wchar_t path[2048]{};
	wchar_t extra[2048]{};

	URL_COMPONENTS components{};
	components.dwStructSize = sizeof(components);
	components.lpszHostName = host;
	components.dwHostNameLength = static_cast<DWORD>(std::size(host));
	components.lpszUrlPath = path;
	components.dwUrlPathLength = static_cast<DWORD>(std::size(path));
	components.lpszExtraInfo = extra;
	components.dwExtraInfoLength = static_cast<DWORD>(std::size(extra));

	if (!WinHttpCrackUrl(pUrl, 0, 0, &components)) {
		return false;
	}

	out.Host = host;
	out.PathAndQuery = std::wstring(path) + extra;
	out.Port = components.nPort;
	out.Https = components.nScheme == INTERNET_SCHEME_HTTPS;
	return true;
}

enum class EHttpResult : std::uint8_t {
	HTTP_RESULT_OK,
	HTTP_RESULT_CANCELLED,
	HTTP_RESULT_FAILED,
};

// The reverse of Utf8ToWide below - only ever used to fold a hostname (always plain ASCII
// for every URL this file ever touches) into an error message, not for anything that needs
// to round-trip real Unicode.
std::string WideToUtf8(const std::wstring &wide)
{
	if (wide.empty()) {
		return {};
	}
	const int length =
		WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
	if (length <= 0) {
		return {};
	}
	std::string result(static_cast<std::size_t>(length), '\0');
	WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), result.data(), length, nullptr,
					   nullptr);
	return result;
}

// A plain blocking GET, following redirects (WinHttpSendRequest/WinHttpReceiveResponse do
// this internally, including across hosts - github.com -> objects.githubusercontent.com is
// exactly this case - once WINHTTP_OPTION_REDIRECT_POLICY allows it below), with optional
// progress reporting for the one caller (the exe download) that wants it - the manifest
// fetch passes nullptr for all three progress pointers, and pCancelRequested is only
// meaningfully polled between WinHttpReadData chunks (the manifest is a few hundred bytes,
// not worth cancelling mid-flight).
EHttpResult HttpGet(const std::wstring &url, std::vector<std::uint8_t> &outBody, std::atomic<bool> *pCancelRequested,
					std::atomic<std::uint64_t> *pBytesDownloaded, std::atomic<std::uint64_t> *pTotalBytes,
					std::atomic<double> *pBytesPerSecond, std::string &outError)
{
	CrackedUrl cracked;
	if (!CrackUrl(url.c_str(), cracked)) {
		outError = "could not parse the update URL";
		return EHttpResult::HTTP_RESULT_FAILED;
	}

	HInternetHandle session{WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
										WINHTTP_NO_PROXY_BYPASS, 0)};
	if (session.Handle == nullptr) {
		outError = "could not open an HTTP session";
		return EHttpResult::HTTP_RESULT_FAILED;
	}

	// Explicit, not trusting whatever WinHTTP's own platform default happens to be: a
	// connection that's accepted but then never sends anything (a transparent proxy or
	// firewall that silently drops packets rather than resetting, rather than an outright
	// connection failure) would otherwise leave WinHttpSendRequest/WinHttpReceiveResponse/
	// WinHttpReadData blocked with nothing to time them out - the exact same "stuck forever
	// inside one blocking call" risk this file's own JoinWithTimeoutOrDetach exists to
	// contain on the thread-lifetime side; this closes the same gap at its actual source.
	// Resolve/connect get a shorter budget than send/receive - DNS and TCP handshakes should
	// be fast or fail fast, while a slow server genuinely streaming a multi-megabyte exe
	// download needs more room per individual read.
	WinHttpSetTimeouts(session, /*resolve*/ 10000, /*connect*/ 10000, /*send*/ 15000, /*receive*/ 15000);

	HInternetHandle connect{WinHttpConnect(session, cracked.Host.c_str(), cracked.Port, 0)};
	if (connect.Handle == nullptr) {
		outError = "could not connect to " + WideToUtf8(cracked.Host);
		return EHttpResult::HTTP_RESULT_FAILED;
	}

	const DWORD requestFlags = cracked.Https ? WINHTTP_FLAG_SECURE : 0;
	HInternetHandle request{WinHttpOpenRequest(connect, L"GET", cracked.PathAndQuery.c_str(), nullptr,
											   WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, requestFlags)};
	if (request.Handle == nullptr) {
		outError = "could not open an HTTP request";
		return EHttpResult::HTTP_RESULT_FAILED;
	}

	// Explicit, not relying on WinHTTP's own default (which already follows HTTPS->HTTPS
	// redirects, but not HTTPS->HTTP) - this is exactly the github.com -> CDN hop every
	// manifest/exe fetch here goes through.
	DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
	WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));

	if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
		outError = "the request failed to send";
		return EHttpResult::HTTP_RESULT_FAILED;
	}
	if (!WinHttpReceiveResponse(request, nullptr)) {
		outError = "no response was received";
		return EHttpResult::HTTP_RESULT_FAILED;
	}

	DWORD statusCode = 0;
	DWORD statusCodeSize = sizeof(statusCode);
	WinHttpQueryHeaders(request, WINHTTP_QUERY_FLAG_NUMBER | WINHTTP_QUERY_STATUS_CODE, WINHTTP_HEADER_NAME_BY_INDEX,
						&statusCode, &statusCodeSize, WINHTTP_NO_HEADER_INDEX);
	if (statusCode < 200 || statusCode >= 300) {
		outError = "server returned HTTP " + std::to_string(statusCode);
		return EHttpResult::HTTP_RESULT_FAILED;
	}

	DWORD contentLength = 0;
	DWORD contentLengthSize = sizeof(contentLength);
	if (WinHttpQueryHeaders(request, WINHTTP_QUERY_FLAG_NUMBER | WINHTTP_QUERY_CONTENT_LENGTH,
							WINHTTP_HEADER_NAME_BY_INDEX, &contentLength, &contentLengthSize,
							WINHTTP_NO_HEADER_INDEX)) {
		if (pTotalBytes != nullptr) {
			pTotalBytes->store(contentLength, std::memory_order_relaxed);
		}
		outBody.reserve(contentLength);
	}

	const ULONGLONG startTick = GetTickCount64();
	std::uint64_t totalRead = 0;

	for (;;) {
		if (pCancelRequested != nullptr && pCancelRequested->load(std::memory_order_relaxed)) {
			return EHttpResult::HTTP_RESULT_CANCELLED;
		}

		DWORD available = 0;
		if (!WinHttpQueryDataAvailable(request, &available)) {
			outError = "the connection was interrupted while reading";
			return EHttpResult::HTTP_RESULT_FAILED;
		}
		if (available == 0) {
			break;
		}

		const std::size_t previousSize = outBody.size();
		outBody.resize(previousSize + available);

		DWORD bytesRead = 0;
		if (!WinHttpReadData(request, outBody.data() + previousSize, available, &bytesRead)) {
			outError = "the connection was interrupted while reading";
			return EHttpResult::HTTP_RESULT_FAILED;
		}
		outBody.resize(previousSize + bytesRead);
		totalRead += bytesRead;

		if (pBytesDownloaded != nullptr) {
			pBytesDownloaded->store(totalRead, std::memory_order_relaxed);
		}
		if (pBytesPerSecond != nullptr) {
			const ULONGLONG elapsedMs = GetTickCount64() - startTick;
			if (elapsedMs > 0) {
				pBytesPerSecond->store(static_cast<double>(totalRead) / (static_cast<double>(elapsedMs) / 1000.0),
									   std::memory_order_relaxed);
			}
		}
	}

	return EHttpResult::HTTP_RESULT_OK;
}

std::wstring Utf8ToWide(const char *pUtf8)
{
	const int length = MultiByteToWideChar(CP_UTF8, 0, pUtf8, -1, nullptr, 0);
	if (length <= 0) {
		return L"";
	}
	std::wstring result(static_cast<std::size_t>(length - 1), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, pUtf8, -1, result.data(), length);
	return result;
}

// SHA-256 the downloaded bytes and check it against manifest.szSha256Hex (integrity), then
// Ed25519-verify that same digest against manifest.szSignatureBase64 and this project's own
// embedded public key (authenticity) - see updater.h's own "Trust" section for why both
// checks matter and neither substitutes for the other.
bool VerifyDownload(const std::vector<std::uint8_t> &body, const CUpdateManifest &manifest, std::string &outError)
{
	unsigned char digest[crypto_hash_sha256_BYTES];
	crypto_hash_sha256(digest, body.data(), body.size());

	static constexpr char kHexDigits[] = "0123456789abcdef";
	char hex[crypto_hash_sha256_BYTES * 2 + 1];
	for (std::size_t i = 0; i < crypto_hash_sha256_BYTES; i += 1) {
		hex[i * 2] = kHexDigits[digest[i] >> 4];
		hex[i * 2 + 1] = kHexDigits[digest[i] & 0x0F];
	}
	hex[crypto_hash_sha256_BYTES * 2] = '\0';

	if (_stricmp(hex, manifest.szSha256Hex) != 0) {
		outError = "the download doesn't match the manifest's SHA-256 - it may be corrupted or truncated";
		return false;
	}

	unsigned char signature[crypto_sign_BYTES];
	std::size_t signatureLength = 0;
	if (sodium_base642bin(signature, sizeof(signature), manifest.szSignatureBase64,
						  std::strlen(manifest.szSignatureBase64), nullptr, &signatureLength, nullptr,
						  sodium_base64_VARIANT_ORIGINAL) != 0 ||
		signatureLength != crypto_sign_BYTES) {
		outError = "the manifest's signature is malformed";
		return false;
	}

	if (crypto_sign_verify_detached(signature, digest, sizeof(digest), kestrel::update::kEd25519PublicKey.data()) !=
		0) {
		outError = "signature verification failed - refusing to install an unsigned or tampered update";
		return false;
	}

	return true;
}

// Writes newExeBytes to disk and swaps it into this exe's own path. The atomic path (one
// MoveFileExW) is tried first: a same-volume rename via MOVEFILE_REPLACE_EXISTING is a
// single NTFS transaction, so there is never a moment where selfPath doesn't exist on disk -
// and Windows permits renaming a currently-executing image's own backing file (the loader
// opens it with FILE_SHARE_DELETE), which is what makes this possible without a separate
// helper process at all. That's only refused in unusual cases (a file lock some other tool
// is holding, certain antivirus real-time-scan configurations), so a two-step fallback
// exists for those: rename self out of the way to "<exe>.old" first, then move the new build
// into place. If crashed exactly between those two steps, "<exe>.old" is left holding a
// known-good previous build and the canonical path is missing - see
// CUpdater::RunStartupRecoveryAndMaybeExit, which is the net under exactly that.
bool ApplyDownloadedExe(const std::vector<std::uint8_t> &newExeBytes, std::string &outError)
{
	wchar_t selfPath[MAX_PATH];
	const DWORD selfPathLength = GetModuleFileNameW(nullptr, selfPath, MAX_PATH);
	if (selfPathLength == 0 || selfPathLength == MAX_PATH) {
		outError = "could not resolve this program's own path";
		return false;
	}

	const std::wstring updatePath = std::wstring(selfPath) + L".update";
	const std::wstring oldPath = std::wstring(selfPath) + L".old";

	HANDLE hFile =
		CreateFileW(updatePath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (hFile == INVALID_HANDLE_VALUE) {
		outError = "could not write the downloaded update to disk";
		return false;
	}
	DWORD written = 0;
	const BOOL wroteOk = WriteFile(hFile, newExeBytes.data(), static_cast<DWORD>(newExeBytes.size()), &written, nullptr);
	CloseHandle(hFile);
	if (wroteOk == FALSE || written != newExeBytes.size()) {
		DeleteFileW(updatePath.c_str());
		outError = "could not write the downloaded update to disk (disk full?)";
		return false;
	}

	if (MoveFileExW(updatePath.c_str(), selfPath, MOVEFILE_REPLACE_EXISTING)) {
		DeleteFileW(oldPath.c_str()); // best-effort - a stale .old from a previous fallback run, if any
		return true;
	}

	if (!MoveFileExW(selfPath, oldPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
		DeleteFileW(updatePath.c_str());
		outError = "could not replace the running executable (it may be locked by another program)";
		return false;
	}
	if (!MoveFileExW(updatePath.c_str(), selfPath, MOVEFILE_REPLACE_EXISTING)) {
		MoveFileExW(oldPath.c_str(), selfPath, MOVEFILE_REPLACE_EXISTING); // best-effort immediate restore
		outError = "could not move the new build into place";
		return false;
	}
	return true;
}
} // namespace

CUpdater::~CUpdater()
{
	RequestCancel();
	// Bounded, not a bare join() - see core/thread_util.h's own file comment for why: a
	// WinHTTP call with no response, no explicit timeout of its own, and this class's own
	// cancellation flag only checked between reads, is the exact same "stuck forever inside
	// one blocking call" risk core/login_attempt.cpp's own worker already has.
	JoinWithTimeoutOrDetach(m_worker, std::chrono::milliseconds(3000));
}

void CUpdater::Init()
{
	m_stage.store(EUpdateStage::UPDATE_STAGE_IDLE, std::memory_order_relaxed);
	m_bCancelRequested.store(false, std::memory_order_relaxed);
	m_bWorkerFinished.store(false, std::memory_order_relaxed);
	m_bActive = false;
	m_bReadyToRelaunchLatched = false;
}

void CUpdater::CheckForUpdateAsync(const char *currentVersion)
{
	if (m_bActive) {
		return;
	}
	if (m_worker.joinable()) {
		m_worker.join();
	}

	m_bCancelRequested.store(false, std::memory_order_relaxed);
	m_bWorkerFinished.store(false, std::memory_order_relaxed);
	m_stage.store(EUpdateStage::UPDATE_STAGE_CHECKING, std::memory_order_relaxed);
	m_bActive = true;

	std::string versionCopy(currentVersion);
	m_worker = std::thread([this, versionCopy = std::move(versionCopy)]() mutable {
		WorkerCheckForUpdate(std::move(versionCopy));
	});
}

void CUpdater::StartDownloadAsync()
{
	if (m_bActive || m_stage.load(std::memory_order_acquire) != EUpdateStage::UPDATE_STAGE_AVAILABLE) {
		return;
	}
	if (m_worker.joinable()) {
		m_worker.join();
	}

	m_bCancelRequested.store(false, std::memory_order_relaxed);
	m_bWorkerFinished.store(false, std::memory_order_relaxed);
	m_bActive = true;

	const CUpdateManifest manifestCopy = m_manifest;
	m_worker = std::thread([this, manifestCopy]() { WorkerDownloadAndInstall(manifestCopy); });
}

void CUpdater::RequestCancel()
{
	m_bCancelRequested.store(true, std::memory_order_relaxed);
}

EUpdateStage CUpdater::GetStage() const
{
	return m_stage.load(std::memory_order_acquire);
}

void CUpdater::Update()
{
	if (!m_bActive) {
		return;
	}
	if (!m_bWorkerFinished.load(std::memory_order_acquire)) {
		return;
	}

	if (m_worker.joinable()) {
		m_worker.join();
	}
	m_bActive = false;

	if (GetStage() == EUpdateStage::UPDATE_STAGE_READY_TO_RELAUNCH) {
		m_bReadyToRelaunchLatched = true;
	}
}

bool CUpdater::ConsumeReadyToRelaunch()
{
	const bool ready = m_bReadyToRelaunchLatched;
	m_bReadyToRelaunchLatched = false;
	return ready;
}

void CUpdater::WorkerCheckForUpdate(std::string currentVersion)
{
	std::vector<std::uint8_t> body;
	std::string error;
	const EHttpResult result = HttpGet(kManifestUrl, body, nullptr, nullptr, nullptr, nullptr, error);
	if (result != EHttpResult::HTTP_RESULT_OK) {
		std::snprintf(m_szErrorMessage, sizeof(m_szErrorMessage), "Couldn't check for updates: %s", error.c_str());
		m_stage.store(EUpdateStage::UPDATE_STAGE_CHECK_FAILED, std::memory_order_release);
		m_bWorkerFinished.store(true, std::memory_order_release);
		return;
	}

	CUpdateManifest manifest{};
	if (!ParseUpdateManifest(reinterpret_cast<const char *>(body.data()), body.size(), &manifest)) {
		std::snprintf(m_szErrorMessage, sizeof(m_szErrorMessage), "Couldn't check for updates: malformed manifest");
		m_stage.store(EUpdateStage::UPDATE_STAGE_CHECK_FAILED, std::memory_order_release);
		m_bWorkerFinished.store(true, std::memory_order_release);
		return;
	}

	SemVer current{};
	SemVer latest{};
	SemVer minUpgrade{};
	SemVerParse(StringViewFromCString(currentVersion.c_str()), current);
	if (!SemVerParse(StringViewFromCString(manifest.szVersion), latest)) {
		std::snprintf(m_szErrorMessage, sizeof(m_szErrorMessage), "Couldn't check for updates: manifest has an "
															 "unparseable version");
		m_stage.store(EUpdateStage::UPDATE_STAGE_CHECK_FAILED, std::memory_order_release);
		m_bWorkerFinished.store(true, std::memory_order_release);
		return;
	}
	SemVerParse(StringViewFromCString(manifest.szMinUpgradeVersion), minUpgrade);

	m_manifest = manifest;

	EUpdateStage finalStage = EUpdateStage::UPDATE_STAGE_UP_TO_DATE;
	if (latest > current) {
		finalStage = current < minUpgrade ? EUpdateStage::UPDATE_STAGE_MANUAL_UPGRADE_REQUIRED
										  : EUpdateStage::UPDATE_STAGE_AVAILABLE;
	}
	m_stage.store(finalStage, std::memory_order_release);
	m_bWorkerFinished.store(true, std::memory_order_release);
}

void CUpdater::WorkerDownloadAndInstall(CUpdateManifest manifest)
{
	m_bytesDownloaded.store(0, std::memory_order_relaxed);
	m_totalBytes.store(0, std::memory_order_relaxed);
	m_bytesPerSecond.store(0.0, std::memory_order_relaxed);
	m_stage.store(EUpdateStage::UPDATE_STAGE_DOWNLOADING, std::memory_order_release);

	const std::wstring url = Utf8ToWide(manifest.szUrl);
	std::vector<std::uint8_t> body;
	std::string error;
	const EHttpResult result =
		HttpGet(url, body, &m_bCancelRequested, &m_bytesDownloaded, &m_totalBytes, &m_bytesPerSecond, error);

	if (result == EHttpResult::HTTP_RESULT_CANCELLED) {
		m_stage.store(EUpdateStage::UPDATE_STAGE_CANCELLED, std::memory_order_release);
		m_bWorkerFinished.store(true, std::memory_order_release);
		return;
	}
	if (result != EHttpResult::HTTP_RESULT_OK) {
		std::snprintf(m_szErrorMessage, sizeof(m_szErrorMessage), "Download failed: %s", error.c_str());
		m_stage.store(EUpdateStage::UPDATE_STAGE_ERROR, std::memory_order_release);
		m_bWorkerFinished.store(true, std::memory_order_release);
		return;
	}

	m_stage.store(EUpdateStage::UPDATE_STAGE_VERIFYING, std::memory_order_release);
	if (!VerifyDownload(body, manifest, error)) {
		std::snprintf(m_szErrorMessage, sizeof(m_szErrorMessage), "%s", error.c_str());
		m_stage.store(EUpdateStage::UPDATE_STAGE_ERROR, std::memory_order_release);
		m_bWorkerFinished.store(true, std::memory_order_release);
		return;
	}

	if (m_bCancelRequested.load(std::memory_order_relaxed)) {
		m_stage.store(EUpdateStage::UPDATE_STAGE_CANCELLED, std::memory_order_release);
		m_bWorkerFinished.store(true, std::memory_order_release);
		return;
	}

	m_stage.store(EUpdateStage::UPDATE_STAGE_INSTALLING, std::memory_order_release);
	if (!ApplyDownloadedExe(body, error)) {
		std::snprintf(m_szErrorMessage, sizeof(m_szErrorMessage), "%s", error.c_str());
		m_stage.store(EUpdateStage::UPDATE_STAGE_ERROR, std::memory_order_release);
		m_bWorkerFinished.store(true, std::memory_order_release);
		return;
	}

	m_stage.store(EUpdateStage::UPDATE_STAGE_READY_TO_RELAUNCH, std::memory_order_release);
	m_bWorkerFinished.store(true, std::memory_order_release);
}

bool CUpdater::RunStartupRecoveryAndMaybeExit()
{
	wchar_t selfPathBuffer[MAX_PATH];
	const DWORD length = GetModuleFileNameW(nullptr, selfPathBuffer, MAX_PATH);
	if (length == 0 || length == MAX_PATH) {
		return false; // can't resolve our own path - nothing sensible to recover, proceed normally
	}
	const std::wstring selfPath(selfPathBuffer);

	constexpr std::wstring_view kOldSuffix = L".old";
	const bool isOldCopy =
		selfPath.size() > kOldSuffix.size() &&
		selfPath.compare(selfPath.size() - kOldSuffix.size(), kOldSuffix.size(), kOldSuffix) == 0;

	if (!isOldCopy) {
		// The overwhelmingly common case: a normal launch. A leftover "<exe>.old" sibling
		// means a previous update's two-step fallback (see ApplyDownloadedExe) finished
		// successfully but never got to clean up after itself - delete it now. Best-effort:
		// an antivirus scanner transiently holding it open is not a reason to fail startup.
		DeleteFileW((selfPath + L".old").c_str());
		return false;
	}

	// We ARE the ".old" leftover - something launched this exact file rather than the
	// canonical path (a stale shortcut, a leftover Run entry, a curious user double-clicking
	// it). If the canonical exe is missing, ApplyDownloadedExe's two-step fallback crashed
	// between its own two MoveFileExW calls - exactly the interrupted-update window this
	// function exists to close. This process is currently executing known-good bytes (its
	// own), so recovering is just copying itself back to the canonical name and handing off.
	const std::wstring canonicalPath = selfPath.substr(0, selfPath.size() - kOldSuffix.size());
	if (GetFileAttributesW(canonicalPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
		CopyFileW(selfPath.c_str(), canonicalPath.c_str(), FALSE);
	}

	STARTUPINFOW startupInfo{sizeof(startupInfo)};
	PROCESS_INFORMATION processInfo{};
	if (CreateProcessW(canonicalPath.c_str(), nullptr, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startupInfo,
					   &processInfo)) {
		CloseHandle(processInfo.hProcess);
		CloseHandle(processInfo.hThread);
	}
	return true;
}
