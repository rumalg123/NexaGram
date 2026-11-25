/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QSet>
#include <QtCore/QString>
#include <QtCore/QStringView>
#include <QtCore/QUuid>

#include <algorithm>
#include <optional>
#include <vector>

namespace Storage {

struct PendingDownloadId {
	quint64 account = 0;
	QUuid job;

	friend bool operator==(const PendingDownloadId &, const PendingDownloadId &) = default;
};

enum class DownloadStatus {
	Preparing, Downloading, Pausing, Paused, Interrupted, Resolving,
	Verifying, Resuming, Waiting, Failed, Unavailable, Finalizing,
	Cancelling, Completed,
};

enum class DownloadFilter { All, Active, Paused, Interrupted, Failed, Completed };
enum class DownloadAction { Pause, Resume, Cancel };
enum class DownloadIssue {
	None, SourceLookup, MissingFile, ChangedFile, Checkpoint,
	Storage, Destination, Transfer,
};

[[nodiscard]] constexpr bool MatchesDownloadFilter(
		DownloadStatus status,
		DownloadFilter filter) {
	using Status = DownloadStatus;
	switch (filter) {
	case DownloadFilter::All: return true;
	case DownloadFilter::Paused: return status == Status::Paused;
	case DownloadFilter::Interrupted:
		return status == Status::Interrupted || status == Status::Waiting;
	case DownloadFilter::Failed:
		return status == Status::Failed || status == Status::Unavailable;
	case DownloadFilter::Completed: return status == Status::Completed;
	case DownloadFilter::Active:
		return status == Status::Preparing || status == Status::Downloading
			|| status == Status::Pausing || status == Status::Resolving
			|| status == Status::Verifying || status == Status::Resuming
			|| status == Status::Finalizing || status == Status::Cancelling;
	}
	return false;
}

[[nodiscard]] constexpr bool AllowsDownloadAction(
		bool persistent,
		DownloadStatus status,
		DownloadAction action) {
	using Status = DownloadStatus;
	if (!persistent || status == Status::Completed || status == Status::Cancelling) {
		return false;
	}
	switch (action) {
	case DownloadAction::Pause:
		return status == Status::Downloading || status == Status::Resolving
			|| status == Status::Resuming;
	case DownloadAction::Resume:
		return status == Status::Paused || status == Status::Interrupted
			|| status == Status::Waiting || status == Status::Failed;
	case DownloadAction::Cancel:
		return status != Status::Finalizing;
	}
	return false;
}

[[nodiscard]] inline bool MatchesDownloadName(QStringView name, QStringView query) {
	return name.contains(query.trimmed(), Qt::CaseInsensitive);
}

[[nodiscard]] inline std::vector<PendingDownloadId> ScopedDownloadIds(
		quint64 account,
		const std::vector<PendingDownloadId> &ids) {
	auto result = std::vector<PendingDownloadId>();
	auto seen = QSet<QUuid>();
	for (const auto &id : ids) {
		if (account && id.account == account && !id.job.isNull() && !seen.contains(id.job)) {
			seen.insert(id.job);
			result.push_back(id);
		}
	}
	return result;
}

struct DownloadByteTotals {
	qint64 written = 0;
	qint64 total = 0;

	void add(qint64 ready, qint64 expected) {
		if (expected > 0) {
			written += std::clamp(ready, qint64(0), expected);
			total += expected;
		}
	}

	[[nodiscard]] int percent() const {
		return total ? int(100. * written / total) : 0;
	}
};

class DownloadRate final {
public:
	void accept(qint64 bytes, qint64 now) {
		if (_last && now - _last >= 2000) {
			*this = DownloadRate();
		}
		_last = now;
		if (!_started) {
			_started = now;
			return;
		}
		_bytes += bytes;
		if (now - _started >= 500) {
			const auto rate = _bytes * 1000 / (now - _started);
			_rate = _rate ? (_rate * 2 + rate) / 3 : rate;
			_bytes = 0;
			_started = now;
		}
	}

	[[nodiscard]] qint64 rate(qint64 now) const {
		return (_last && now - _last < 2000) ? _rate : 0;
	}

private:
	qint64 _started = 0;
	qint64 _last = 0;
	qint64 _bytes = 0;
	qint64 _rate = 0;
};

[[nodiscard]] inline std::optional<qint64> DownloadEta(
		qint64 ready,
		qint64 total,
		qint64 bytesPerSecond) {
	const auto remaining = total - ready;
	if (bytesPerSecond <= 0 || remaining <= bytesPerSecond) {
		return std::nullopt;
	}
	const auto seconds = remaining / bytesPerSecond;
	return (seconds < 24 * 60 * 60) ? std::make_optional(seconds) : std::nullopt;
}

struct DownloadTransferView {
	DownloadStatus status = DownloadStatus::Preparing;
	DownloadIssue issue = DownloadIssue::None;
	qint64 written = 0;
	qint64 committed = 0;
	qint64 bytesPerSecond = 0;
};

} // namespace Storage
