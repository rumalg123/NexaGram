/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "storage/pending_downloads.h"

#include "storage/download_policy.h"

#include <QtCore/QDataStream>
#include <QtCore/QDebug>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QLockFile>
#include <QtCore/QSaveFile>
#include <QtCore/QSet>

#include <algorithm>
#include <utility>

namespace Storage {
namespace {

using namespace Qt::StringLiterals;
using State = PendingDownloadState;
constexpr auto kMagic = quint32(0x36475052);
constexpr auto kLegacyVersion = quint32(1);
constexpr auto kAuthenticatedVersion = quint32(2);
constexpr auto kMaximumFileSize = qint64(4000) * 1024 * 1024;

bool Valid(const PendingDownload &job) {
	const auto state = quint8(job.state);
	return !job.file.id.isNull()
		&& (job.file.checkpointKey.isEmpty() || job.file.checkpointKey.size() == 32)
		&& job.file.size > 0 && job.file.size <= kMaximumFileSize
		&& job.file.destination.size() <= 4096
		&& ValidPersistentDownloadPath(job.file.destination)
		&& !IsPendingDownloadArtifactName(QFileInfo(job.file.destination).fileName())
		&& job.displayName.size() <= 512
		&& job.peer && job.message > 0 && job.message <= 0x7FFFFFFF
		&& job.document && job.started > 0 && job.updated > 0
		&& (job.stagingIdentity.isEmpty()
			|| job.stagingIdentity.size() == 16
			|| job.stagingIdentity.size() == 24)
		&& state >= quint8(State::Initializing)
		&& state <= quint8(State::Cancelling)
		&& (!job.stagingIdentity.isEmpty() || job.state == State::Initializing
			|| job.state == State::Unavailable || job.state == State::Cancelling);
}

} // namespace

bool PendingDownloadTransition(State from, State to) {
	if (from == to) {
		return true;
	}
	if (to == State::Cancelling) {
		return true;
	}
	switch (from) {
	case State::Initializing:
	case State::Paused:
		return to == State::Downloading || to == State::Unavailable;
	case State::Retryable:
		return to == State::Downloading || to == State::Unavailable
			|| to == State::Paused;
	case State::Downloading:
		return to == State::Paused || to == State::Interrupted
			|| to == State::Retryable || to == State::Unavailable
			|| to == State::Finalizing;
	case State::Interrupted:
		return to == State::Downloading || to == State::Retryable
			|| to == State::Unavailable || to == State::Paused;
	case State::Finalizing:
		return to == State::Downloading;
	case State::Unavailable:
	case State::Cancelling:
		return false;
	}
	return false;
}

QByteArray SerializePendingDownloads(
		quint64 account,
		const PendingDownloadList &jobs) {
	if (!account || jobs.size() > kPendingDownloadLimit) {
		return {};
	}
	auto bytes = QByteArray();
	auto stream = QDataStream(&bytes, QIODevice::WriteOnly);
	stream.setVersion(QDataStream::Qt_5_1);
	const auto keyed = std::any_of(jobs.begin(), jobs.end(), [](const auto &job) {
		return !job.file.checkpointKey.isEmpty();
	});
	stream << kMagic << (keyed ? kAuthenticatedVersion : kLegacyVersion) << account << quint32(jobs.size());
	auto ids = QSet<QUuid>();
	for (const auto &job : jobs) {
		if (!Valid(job) || ids.contains(job.file.id)) {
			return {};
		}
		ids.insert(job.file.id);
		stream << job.file.id << job.file.destination << job.file.size
			<< quint8(job.file.replace) << job.stagingIdentity << job.peer
			<< job.message << job.channelAccess << job.document
			<< job.displayName << job.started << job.updated << quint8(job.state);
		if (keyed) {
			stream << job.file.checkpointKey;
		}
	}
	return (stream.status() == QDataStream::Ok
		&& bytes.size() <= kPendingDownloadRegistrySize)
		? bytes
		: QByteArray();
}

std::optional<PendingDownloadList> ParsePendingDownloads(
		quint64 account,
		const QByteArray &bytes) {
	if (!account || bytes.isEmpty() || bytes.size() > kPendingDownloadRegistrySize) {
		return std::nullopt;
	}
	auto stream = QDataStream(bytes);
	stream.setVersion(QDataStream::Qt_5_1);
	auto magic = quint32();
	auto version = quint32();
	auto owner = quint64();
	auto count = quint32();
	stream >> magic >> version >> owner >> count;
	if (stream.status() != QDataStream::Ok || magic != kMagic
		|| (version != kLegacyVersion && version != kAuthenticatedVersion) || owner != account || count > kPendingDownloadLimit) {
		return std::nullopt;
	}
	auto jobs = PendingDownloadList();
	jobs.reserve(count);
	auto ids = QSet<QUuid>();
	for (auto i = quint32(); i != count; ++i) {
		auto job = PendingDownload();
		auto replace = quint8();
		auto state = quint8();
		stream >> job.file.id;
		if (!FitsPersistentDownloadField(stream, 8192)) {
			return std::nullopt;
		}
		stream >> job.file.destination >> job.file.size >> replace;
		if (!FitsPersistentDownloadField(stream, 24)) {
			return std::nullopt;
		}
		stream >> job.stagingIdentity >> job.peer >> job.message
			>> job.channelAccess >> job.document;
		if (!FitsPersistentDownloadField(stream, 1024)) {
			return std::nullopt;
		}
		stream >> job.displayName >> job.started >> job.updated >> state;
		if (version == kAuthenticatedVersion) {
			if (!FitsPersistentDownloadField(stream, 32)) {
				return std::nullopt;
			}
			stream >> job.file.checkpointKey;
		}
		job.file.replace = (replace == 1);
		job.state = State(state);
		if (stream.status() != QDataStream::Ok || replace > 1 || !Valid(job)) {
			return std::nullopt;
		}
		if (ids.contains(job.file.id)) {
			return std::nullopt;
		}
		ids.insert(job.file.id);
		jobs.push_back(std::move(job));
	}
	if (!stream.atEnd()) {
		return std::nullopt;
	}
	return jobs;
}

std::shared_ptr<QLockFile> LockPendingDownloadRegistry(const QString &path) {
	const auto lockPath = path + u".lock"_s;
	if (!SafePersistentDownloadPath(lockPath)
		|| !QDir().mkpath(QFileInfo(path).absolutePath())
		|| !SafePersistentDownloadPath(lockPath)) {
		return nullptr;
	}
	auto lock = std::make_shared<QLockFile>(lockPath);
	lock->setStaleLockTime(0);
	return lock->tryLock(0) ? lock : nullptr;
}

bool ReplacePendingDownloadRegistry(const QString &path, QByteArrayView encrypted) {
	if (encrypted.isEmpty() || encrypted.size() > kPendingDownloadRegistrySize + 64
		|| !SafePersistentDownloadPath(path)
		|| !QDir().mkpath(QFileInfo(path).absolutePath())
		|| !SafePersistentDownloadPath(path)) {
		return false;
	}
	auto file = QSaveFile(path);
	file.setDirectWriteFallback(false);
	return file.open(QIODevice::WriteOnly)
		&& file.write(encrypted.data(), encrypted.size()) == encrypted.size()
		&& FlushPersistentDownloadFile(file)
		&& file.commit();
}

bool PendingDownloadsTrace() {
	return DownloadOverrideValue(PersistentDownloadOverrides().trace).value_or(false);
}

void TracePendingDownload(const QUuid &id, const char *event, int detail) {
	if (PendingDownloadsTrace()) {
		qInfo() << "Persistent download" << id << event << detail;
	}
}

} // namespace Storage
