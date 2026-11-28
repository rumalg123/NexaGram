/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "storage/persistent_file_sink.h"

#include <memory>
#include <optional>
#include <vector>

class QLockFile;

namespace Storage {

enum class PendingDownloadState : quint8 {
	Initializing = 1,
	Downloading,
	Paused,
	Interrupted,
	Retryable,
	Unavailable,
	Finalizing,
	Cancelling,
};

struct PendingDownload {
	PersistentFileSink::Job file;
	QByteArray stagingIdentity;
	quint64 peer = 0;
	qint64 message = 0;
	quint64 channelAccess = 0;
	quint64 document = 0;
	QString displayName;
	qint64 started = 0;
	qint64 updated = 0;
	PendingDownloadState state = PendingDownloadState::Initializing;
	quint64 backupPeer = 0;
	qint64 backupMessage = 0;
	quint64 backupChannelAccess = 0;
};

using PendingDownloadList = std::vector<PendingDownload>;

inline constexpr auto kPendingDownloadLimit = 64;
inline constexpr auto kPendingDownloadRegistrySize = 2 * 1024 * 1024;

[[nodiscard]] bool PendingDownloadTransition(
	PendingDownloadState from,
	PendingDownloadState to);
[[nodiscard]] QByteArray SerializePendingDownloads(
	quint64 account,
	const PendingDownloadList &jobs);
[[nodiscard]] std::optional<PendingDownloadList> ParsePendingDownloads(
	quint64 account,
	const QByteArray &bytes);
[[nodiscard]] std::shared_ptr<QLockFile> LockPendingDownloadRegistry(const QString &path);
[[nodiscard]] bool ReplacePendingDownloadRegistry(
	const QString &path,
	QByteArrayView encrypted);
[[nodiscard]] constexpr bool PendingDownloadMayAutoResume(PendingDownloadState state) {
	return state == PendingDownloadState::Downloading
		|| state == PendingDownloadState::Interrupted
		|| state == PendingDownloadState::Finalizing;
}

[[nodiscard]] bool PendingDownloadsTrace();
void TracePendingDownload(const QUuid &id, const char *event, int detail = 0);

} // namespace Storage
