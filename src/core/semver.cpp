#include "core/semver.h"

namespace {
// Parses one dot-separated numeric component starting at *pIndex, advancing it past the
// component and the following '.' (if any). Returns false (leaving outValue at 0) if there
// is no digit at *pIndex to parse at all.
bool ParseComponent(CStringView text, std::uint64_t *pIndex, std::uint32_t &outValue)
{
	outValue = 0;
	const std::uint64_t start = *pIndex;
	while (*pIndex < text.Length && text.pData[*pIndex] >= '0' && text.pData[*pIndex] <= '9') {
		outValue = outValue * 10 + static_cast<std::uint32_t>(text.pData[*pIndex] - '0');
		*pIndex += 1;
	}
	if (*pIndex == start) {
		return false;
	}
	if (*pIndex < text.Length && text.pData[*pIndex] == '.') {
		*pIndex += 1;
	}
	return true;
}
} // namespace

bool SemVerParse(CStringView text, SemVer &outVersion)
{
	outVersion = SemVer{};

	std::uint64_t index = 0;
	if (!ParseComponent(text, &index, outVersion.Major)) {
		return false;
	}
	// Minor/Patch are optional past the first component - "1" and "1.2" are both accepted,
	// just with the missing components left at 0. ParseComponent's own false return here
	// just means "nothing more to parse," not a format error, since Major already succeeded.
	ParseComponent(text, &index, outVersion.Minor);
	ParseComponent(text, &index, outVersion.Patch);
	return true;
}
