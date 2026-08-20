#include "core/update_manifest.h"

#include <cstring>

#include <nlohmann/json.hpp>

namespace {
// Copies a JSON string field into a fixed destination buffer, truncating (not failing) on
// overflow - matches StringViewCopyToFixed's own truncate-don't-assert convention
// (core/string_view.h), just without needing a CStringView round-trip for a
// std::string source. Returns false if the key is missing or not a string.
bool CopyStringField(const nlohmann::json &json, const char *pKey, char *pDest, std::size_t destCapacity)
{
	const auto it = json.find(pKey);
	if (it == json.end() || !it->is_string()) {
		return false;
	}
	const std::string &value = it->get_ref<const std::string &>();
	const std::size_t length = value.size() < destCapacity - 1 ? value.size() : destCapacity - 1;
	std::memcpy(pDest, value.data(), length);
	pDest[length] = '\0';
	return true;
}
} // namespace

bool ParseUpdateManifest(const char *pJson, std::uint64_t jsonLength, CUpdateManifest *pOutManifest)
{
	nlohmann::json parsed;
	try {
		parsed = nlohmann::json::parse(pJson, pJson + jsonLength);
	} catch (const nlohmann::json::exception &) {
		return false;
	}
	if (!parsed.is_object()) {
		return false;
	}

	const bool required = CopyStringField(parsed, "version", pOutManifest->szVersion, sizeof(pOutManifest->szVersion)) &&
						  CopyStringField(parsed, "url", pOutManifest->szUrl, sizeof(pOutManifest->szUrl)) &&
						  CopyStringField(parsed, "sha256", pOutManifest->szSha256Hex, sizeof(pOutManifest->szSha256Hex)) &&
						  CopyStringField(parsed, "signature", pOutManifest->szSignatureBase64,
										  sizeof(pOutManifest->szSignatureBase64));
	if (!required) {
		return false;
	}

	// Optional fields: a manifest that omits either simply means "no minimum, any older
	// build may auto-update" and "no release notes to show," not a parse failure.
	if (!CopyStringField(parsed, "min_upgrade_version", pOutManifest->szMinUpgradeVersion,
						 sizeof(pOutManifest->szMinUpgradeVersion))) {
		std::memcpy(pOutManifest->szMinUpgradeVersion, "0.0.0", sizeof("0.0.0"));
	}
	if (!CopyStringField(parsed, "notes", pOutManifest->szNotes, sizeof(pOutManifest->szNotes))) {
		pOutManifest->szNotes[0] = '\0';
	}

	return true;
}
