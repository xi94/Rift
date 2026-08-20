#include "core/atomic_file.h"

#include <cstdio>
#include <cstdlib>

#include <Windows.h>

bool CAtomicFile::BackupPathFor(const char *pPath, char *pOutBuffer, std::size_t bufferSize)
{
	const int written = std::snprintf(pOutBuffer, bufferSize, "%s.bak", pPath);
	return written > 0 && static_cast<std::size_t>(written) < bufferSize;
}

bool CAtomicFile::WriteAtomic(const char *pPath, const void *pData, std::size_t length)
{
	char tmpPath[MAX_PATH];
	const int written = std::snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", pPath);
	if (written <= 0 || static_cast<std::size_t>(written) >= sizeof(tmpPath)) {
		return false;
	}

	const HANDLE hFile = CreateFileA(tmpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (hFile == INVALID_HANDLE_VALUE) {
		return false;
	}

	DWORD bytesWritten = 0;
	const bool wroteOk =
		WriteFile(hFile, pData, static_cast<DWORD>(length), &bytesWritten, nullptr) != 0 && bytesWritten == length;
	// FlushFileBuffers forces the write through the disk's own cache, not just the OS
	// page cache - fclose/CloseHandle alone survive a process crash (the OS still has the
	// data) but not a power-loss/OS-crash, which is the scenario this whole mechanism
	// exists for.
	const bool flushOk = wroteOk && FlushFileBuffers(hFile) != 0;
	CloseHandle(hFile);
	if (!flushOk) {
		DeleteFileA(tmpPath);
		return false;
	}

	char backupPath[MAX_PATH];
	if (BackupPathFor(pPath, backupPath, sizeof(backupPath))) {
		// Best-effort: rotate any existing primary file to .bak before it's replaced. On
		// the very first save ever, pPath doesn't exist yet and this call simply fails -
		// not a real error, there's nothing to back up.
		MoveFileExA(pPath, backupPath, MOVEFILE_REPLACE_EXISTING);
	}

	// The durability guarantee: MoveFileExA's rename is atomic at the filesystem level for
	// a same-volume move (this project's storage directory and its .tmp sibling are always
	// on the same volume) - `pPath` is either still the previous file or fully the new one,
	// never a partially-written mix of both, regardless of when a crash happens.
	if (!MoveFileExA(tmpPath, pPath, MOVEFILE_REPLACE_EXISTING)) {
		DeleteFileA(tmpPath);
		return false;
	}
	return true;
}

bool CAtomicFile::ReadFile(const char *pPath, std::uint8_t **ppOutData, std::size_t *pOutLength)
{
	FILE *pFile = nullptr;
	if (fopen_s(&pFile, pPath, "rb") != 0 || pFile == nullptr) {
		return false;
	}

	std::fseek(pFile, 0, SEEK_END);
	const long fileSize = std::ftell(pFile);
	std::fseek(pFile, 0, SEEK_SET);
	if (fileSize <= 0) {
		std::fclose(pFile);
		return false;
	}

	auto *pBuffer = static_cast<std::uint8_t *>(std::malloc(static_cast<std::size_t>(fileSize)));
	if (pBuffer == nullptr) {
		std::fclose(pFile);
		return false;
	}

	const std::size_t readCount = std::fread(pBuffer, 1, static_cast<std::size_t>(fileSize), pFile);
	std::fclose(pFile);
	if (readCount != static_cast<std::size_t>(fileSize)) {
		std::free(pBuffer);
		return false;
	}

	*ppOutData = pBuffer;
	*pOutLength = readCount;
	return true;
}
