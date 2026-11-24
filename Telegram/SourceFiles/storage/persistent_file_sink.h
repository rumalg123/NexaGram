/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QBitArray>
#include <QtCore/QByteArray>
#include <QtCore/QFile>
#include <QtCore/QUuid>

#include <memory>

namespace Storage {

inline constexpr auto kPersistentDownloadPartSize = 128 * 1024;

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

	[[nodiscard]] Result open(bool resume);
	[[nodiscard]] Result write(qint64 offset, QByteArrayView bytes);
	[[nodiscard]] Result checkpoint();
	[[nodiscard]] Result suspend();
	[[nodiscard]] Result publish();
	[[nodiscard]] Result cancel();

	[[nodiscard]] const QBitArray &committed() const;
	[[nodiscard]] qint64 writtenSize() const;
	[[nodiscard]] QString stagingPath() const;
	[[nodiscard]] QString checkpointPath() const;

private:
	[[nodiscard]] Result readCheckpoint();
	[[nodiscard]] bool ownsStaging() const;
	[[nodiscard]] qint64 partLength(qsizetype index) const;
	void log(const char *action) const;

	Job _job;
	std::unique_ptr<QFile> _file;
	QString _stagingPath;
	QByteArray _identity;
	QByteArray _destinationIdentity;
	QByteArray _digests;
	QBitArray _written;
	QBitArray _committed;
	qint64 _writtenSize = 0;
	qint64 _pendingBytes = 0;
	bool _checkpointDirty = false;
	bool _published = false;
	bool _created = false;

};

} // namespace Storage
