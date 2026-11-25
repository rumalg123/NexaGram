/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "storage/download_presentation.h"

#include <QtCore/QBitArray>
#include <QtCore/QByteArray>
#include <QtCore/QDataStream>
#include <QtCore/QFile>
#include <QtCore/QUuid>

#include <atomic>
#include <memory>

namespace Storage {

inline constexpr auto kPersistentDownloadPartSize = 128 * 1024;

[[nodiscard]] bool FlushPersistentDownloadFile(QFileDevice &file);
[[nodiscard]] bool ValidPersistentDownloadPath(const QString &path);
[[nodiscard]] QByteArray PersistentDownloadAliasIdentity(const QString &path);
[[nodiscard]] bool SafePersistentDownloadPath(const QString &path);
[[nodiscard]] bool PersistentDownloadPathMissing(const QString &path);
[[nodiscard]] bool FitsPersistentDownloadField(
	QDataStream &stream,
	quint32 limit,
	bool bitCount = false);

class PersistentDownloadRequests final {
public:
	void reset(QBitArray committed);
	[[nodiscard]] bool empty() const;
	[[nodiscard]] qint64 take();
	void invalidate();
	[[nodiscard]] quint64 generation() const;
	[[nodiscard]] int outstanding() const;
	[[nodiscard]] bool written(quint64 generation);

private:
	void skipCommitted();

	QBitArray _committed;
	qsizetype _next = 0;
	quint64 _generation = 0;
	int _outstanding = 0;

};

class PersistentFileSink final {
public:
	struct Job {
		QUuid id;
		QString destination;
		qint64 size = 0;
		bool replace = false;
		QByteArray checkpointKey;
	};
	enum class Result {
		Ok,
		InvalidJob,
		Missing,
		Replaced,
		InvalidCheckpoint,
		InvalidPart,
		TransferFailed,
		SeekFailed,
		WriteFailed,
		CheckpointFailed,
		Incomplete,
		PublishFailed,
		RemoveFailed,
	};

	explicit PersistentFileSink(
		Job job,
		std::unique_ptr<QFile> file = std::make_unique<QFile>());

	[[nodiscard]] Result open(
		bool resume,
		QByteArrayView expectedIdentity = {},
		bool deferCheckpoint = false,
		bool allowEmptyRecovery = false,
		const std::atomic<bool> *stop = nullptr);
	[[nodiscard]] Result inspectForResume(QByteArrayView expectedIdentity);
	[[nodiscard]] Result inspectPublished(QByteArrayView expectedIdentity,
		const std::atomic<bool> *stop = nullptr);
	[[nodiscard]] bool matchesPublished(QByteArrayView expectedIdentity) const;
	[[nodiscard]] Result purge(QByteArrayView expectedIdentity);
	[[nodiscard]] bool removePublishedCheckpoint();
	[[nodiscard]] const QByteArray &identity() const;
	[[nodiscard]] Result write(qint64 offset, QByteArrayView bytes);
	[[nodiscard]] Result checkpoint();
	[[nodiscard]] Result suspend();
	[[nodiscard]] Result preparePublish();
	[[nodiscard]] Result publish(bool keepCheckpoint = false, const std::atomic<bool> *stop = nullptr);
	[[nodiscard]] Result cancel();

	[[nodiscard]] const QBitArray &committed() const;
	[[nodiscard]] qint64 committedSize() const;
	[[nodiscard]] qint64 writtenSize() const;
	[[nodiscard]] static QString CheckpointPath(const Job &job);
	[[nodiscard]] QString stagingPath() const;
	[[nodiscard]] QString checkpointPath() const;

private:
	[[nodiscard]] Result readCheckpoint(bool verify = true, QFile *opened = nullptr,
		const std::atomic<bool> *stop = nullptr);
	[[nodiscard]] Result verifyParts(QBitArray &bitmap, const QByteArray &digests,
		qint64 &verified, const std::atomic<bool> *stop);
	void initializeCoverage();
	[[nodiscard]] bool ownsStaging() const;
	[[nodiscard]] qint64 partLength(qsizetype index) const;
	void log(const char *action) const;

	Job _job;
	std::unique_ptr<QFile> _file;
	QString _stagingPath;
	QByteArray _identity;
	QByteArray _destinationIdentity;
	QByteArray _checkpointDigest;
	QByteArray _checkpointIdentity;
	QByteArray _digests;
	QBitArray _written;
	QBitArray _committed;
	qint64 _writtenSize = 0;
	qint64 _committedSize = 0;
	qint64 _pendingBytes = 0;
	bool _checkpointDirty = false;
	bool _published = false;
	bool _created = false;

};

[[nodiscard]] DownloadIssue DownloadIssueFromSink(PersistentFileSink::Result result);

} // namespace Storage
