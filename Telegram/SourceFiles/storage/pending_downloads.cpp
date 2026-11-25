/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "storage/pending_downloads.h"

#include <QtCore/QDataStream>
#include <QtCore/QDebug>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QSaveFile>
#include <QtCore/QSet>

#include <utility>

namespace Storage {
namespace {

using State = PendingDownloadState;
constexpr auto kMagic = quint32(0x36475052);
constexpr auto kVersion = quint32(1);
constexpr auto kMaximumFileSize = qint64(4000) * 1024 * 1024;

bool Valid(const PendingDownload &job) {
	const auto state = quint8(job.state);
	return !job.file.id.isNull()
		&& job.file.size > 0 && job.file.size <= kMaximumFileSize
		&& job.file.destination.size() <= 4096
		&& QFileInfo(job.file.destination).isAbsolute()
		&& QDir::cleanPath(job.file.destination) == job.file.destination
		&& !QFileInfo(job.file.destination).fileName().isEmpty()
		&& !job.file.destination.contains(QChar(0))
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
	stream << kMagic << kVersion << account << quint32(jobs.size());
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
		|| version != kVersion || owner != account || count > kPendingDownloadLimit) {
		return std::nullopt;
	}
	auto jobs = PendingDownloadList();
	jobs.reserve(count);
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
		job.file.replace = (replace == 1);
		job.state = State(state);
		if (stream.status() != QDataStream::Ok || replace > 1 || !Valid(job)) {
			return std::nullopt;
		}
		for (const auto &existing : jobs) {
			if (existing.file.id == job.file.id) {
				return std::nullopt;
			}
		}
		jobs.push_back(std::move(job));
	}
	if (!stream.atEnd()) {
		return std::nullopt;
	}
	return jobs;
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

bool PendingDownloadsEnabled() {
	return qEnvironmentVariableIntValue("TDESKTOP_EXPERIMENTAL_RESUMABLE_DOWNLOADS") == 1;
}

bool PendingDownloadsAutoResume() {
	return qEnvironmentVariableIntValue("TDESKTOP_EXPERIMENTAL_RESUME_ON_STARTUP") == 1;
}

bool PendingDownloadsTrace() {
	return qEnvironmentVariableIntValue("TDESKTOP_PERSISTENT_DOWNLOAD_TRACE") == 1;
}

void TracePendingDownload(const QUuid &id, const char *event, int detail) {
	if (PendingDownloadsTrace()) {
		qInfo() << "Persistent download" << id << event << detail;
	}
}

} // namespace Storage
