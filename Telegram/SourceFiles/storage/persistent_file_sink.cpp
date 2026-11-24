/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "storage/persistent_file_sink.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDataStream>
#include <QtCore/QDebug>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QSaveFile>

#include <algorithm>
#include <cstdio>
#include <utility>

#ifdef Q_OS_WIN
#include <QtCore/qt_windows.h>
#include <io.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace Storage {
namespace {

using namespace Qt::StringLiterals;

constexpr auto kMagic = quint32(0x36475044);
constexpr auto kVersion = quint32(1);
constexpr auto kDigestSize = 32;
constexpr auto kMaximumSize = qint64(4000) * 1024 * 1024;
constexpr auto kCheckpointBytes = qint64(8) * 1024 * 1024;
constexpr auto kMaximumCheckpoint = kMaximumSize / kPersistentDownloadPartSize
	* (kDigestSize + 1) + 512;

QByteArray Digest(QByteArrayView bytes) {
	return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
}

QByteArray Identity(QFile &file) {
	if (file.handle() < 0) {
		return {};
	}
	auto bytes = QByteArray();
	auto stream = QDataStream(&bytes, QIODevice::WriteOnly);
#ifdef Q_OS_WIN
	const auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(file.handle()));
	auto info = BY_HANDLE_FILE_INFORMATION{};
	if (!GetFileInformationByHandle(handle, &info)
		|| (info.dwFileAttributes
			& (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
		return {};
	}
	stream
		<< quint64(info.dwVolumeSerialNumber)
		<< quint64((quint64(info.nFileIndexHigh) << 32) | info.nFileIndexLow)
		<< quint64((quint64(info.ftCreationTime.dwHighDateTime) << 32)
			| info.ftCreationTime.dwLowDateTime);
#else
	struct stat info = {};
	if (fstat(file.handle(), &info) != 0 || !S_ISREG(info.st_mode)) {
		return {};
	}
	stream << quint64(info.st_dev) << quint64(info.st_ino);
#endif
	return bytes;
}

QByteArray PathIdentity(const QString &path) {
	auto file = QFile(path);
	return (!QFileInfo(file).isSymLink()
		&& file.open(QIODevice::ReadOnly | QIODevice::ExistingOnly))
		? Identity(file)
		: QByteArray();
}

bool Flush(QFileDevice &file) {
	if (!file.flush() || file.handle() < 0) {
		return false;
	}
#ifdef Q_OS_WIN
	const auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(file.handle()));
	return FlushFileBuffers(handle) != 0;
#else
	return fsync(file.handle()) == 0;
#endif
}

bool PublishFile(const QString &from, const QString &to, bool replace) {
#ifdef Q_OS_WIN
	const auto source = QDir::toNativeSeparators(from).toStdWString();
	const auto target = QDir::toNativeSeparators(to).toStdWString();
	return MoveFileExW(source.c_str(), target.c_str(),
		replace ? MOVEFILE_REPLACE_EXISTING : 0) != 0;
#else
	const auto source = QFile::encodeName(from);
	const auto target = QFile::encodeName(to);
	if (replace) {
		return std::rename(source.constData(), target.constData()) == 0;
	}
	if (link(source.constData(), target.constData()) != 0) {
		return false;
	}
	QFile::remove(from);
	return true;
#endif
}

} // namespace

void PersistentDownloadRequests::reset(QBitArray committed) {
	_committed = std::move(committed);
	_next = 0;
	_outstanding = 0;
	skipCommitted();
}

bool PersistentDownloadRequests::empty() const {
	return _next == _committed.size();
}

qint64 PersistentDownloadRequests::take() {
	Q_ASSERT(!empty());
	const auto offset = qint64(_next++) * kPersistentDownloadPartSize;
	skipCommitted();
	++_outstanding;
	return offset;
}

void PersistentDownloadRequests::invalidate() {
	++_generation;
	_outstanding = 0;
}

quint64 PersistentDownloadRequests::generation() const {
	return _generation;
}

int PersistentDownloadRequests::outstanding() const {
	return _outstanding;
}

bool PersistentDownloadRequests::written(quint64 generation) {
	if (generation != _generation || !_outstanding) {
		return false;
	}
	--_outstanding;
	return true;
}

void PersistentDownloadRequests::skipCommitted() {
	while (_next < _committed.size() && _committed.testBit(_next)) {
		++_next;
	}
}

PersistentFileSink::PersistentFileSink(Job job, std::unique_ptr<QFile> file)
: _job(std::move(job))
, _file(std::move(file))
, _stagingPath(QFileInfo(_job.destination).absoluteDir().filePath(
	u".64gram-"_s + _job.id.toString(QUuid::WithoutBraces) + u".part"_s)) {
	_file->setFileName(_stagingPath);
}

auto PersistentFileSink::open(bool resume) -> Result {
	if (_job.id.isNull() || !QFileInfo(_job.destination).isAbsolute()
		|| _job.size <= 0 || _job.size > kMaximumSize || _published
		|| _file->isOpen()) {
		return Result::InvalidJob;
	}
	if (!resume) {
		_destinationIdentity.clear();
	}
	if (!resume && _job.replace) {
		const auto destination = QFileInfo(_job.destination);
		if (destination.exists() || destination.isSymLink()) {
			_destinationIdentity = PathIdentity(_job.destination);
			if (_destinationIdentity.isEmpty()) {
				return Result::Replaced;
			}
		}
	}
	const auto info = QFileInfo(_stagingPath);
	if (info.isSymLink() || (info.exists() && !info.isFile())) {
		return Result::Replaced;
	}
	const auto mode = QIODevice::ReadWrite | QIODevice::Unbuffered;
	if (!_file->open(mode | (resume ? QIODevice::ExistingOnly : QIODevice::NewOnly))) {
		return (resume && !info.exists()) ? Result::Missing : Result::WriteFailed;
	}
	const auto identity = Identity(*_file);
	auto result = Result::Ok;
	if (identity.isEmpty()) {
		result = Result::Replaced;
	} else if (resume) {
		auto previousIdentity = std::exchange(_identity, identity);
		result = readCheckpoint();
		if (result != Result::Ok) {
			_identity = std::move(previousIdentity);
		}
	} else {
		const auto parts = (_job.size + kPersistentDownloadPartSize - 1)
			/ kPersistentDownloadPartSize;
		_identity = identity;
		_written = QBitArray(parts);
		_committed = QBitArray(parts);
		_digests = QByteArray(parts * kDigestSize, '\0');
		_writtenSize = 0;
		_pendingBytes = 0;
		_created = true;
		_checkpointDirty = true;
		result = checkpoint();
		if (result != Result::Ok && cancel() == Result::Ok) {
			_checkpointDirty = false;
		}
	}
	if (result != Result::Ok) {
		_file->close();
	} else {
		_created = true;
		log(resume ? "reopened" : "created");
	}
	return result;
}

auto PersistentFileSink::partLength(qsizetype index) const -> qint64 {
	const auto offset = qint64(index) * kPersistentDownloadPartSize;
	return std::min(qint64(kPersistentDownloadPartSize), _job.size - offset);
}

auto PersistentFileSink::write(qint64 offset, QByteArrayView bytes) -> Result {
	if (!_file->isOpen() || offset < 0 || offset >= _job.size
		|| offset % kPersistentDownloadPartSize) {
		return Result::InvalidPart;
	}
	const auto index = qsizetype(offset / kPersistentDownloadPartSize);
	if (bytes.size() != partLength(index)) {
		return Result::InvalidPart;
	}
	const auto digest = Digest(bytes);
	if (_written.testBit(index)) {
		return (QByteArrayView(_digests).sliced(index * kDigestSize, kDigestSize)
			== digest) ? Result::Ok : Result::InvalidPart;
	}
	if (!_file->seek(offset)) {
		return Result::SeekFailed;
	}
	if (_file->write(bytes.data(), bytes.size()) != bytes.size()) {
		return Result::WriteFailed;
	}
	std::copy(digest.begin(), digest.end(), _digests.begin() + index * kDigestSize);
	_written.setBit(index);
	_writtenSize += bytes.size();
	_pendingBytes += bytes.size();
	_checkpointDirty = true;
	return (_pendingBytes >= kCheckpointBytes) ? checkpoint() : Result::Ok;
}

auto PersistentFileSink::checkpoint() -> Result {
	if (!_checkpointDirty) {
		return Result::Ok;
	}
	if (!_file->isOpen() || !ownsStaging() || !Flush(*_file)) {
		log("data flush failed");
		return Result::CheckpointFailed;
	}
	auto body = QByteArray();
	auto stream = QDataStream(&body, QIODevice::WriteOnly);
	stream.setVersion(QDataStream::Qt_5_1);
	stream << kMagic << kVersion << _job.id << _job.size
		<< quint32(kPersistentDownloadPartSize)
		<< Digest(QDir::cleanPath(_job.destination).toUtf8())
		<< _identity << _destinationIdentity << _written << _digests;
	const auto digest = Digest(body);
	auto file = QSaveFile(checkpointPath());
	file.setDirectWriteFallback(false);
	if (stream.status() != QDataStream::Ok || !file.open(QIODevice::WriteOnly)
		|| file.write(body) != body.size() || file.write(digest) != digest.size()
		|| !Flush(file) || !file.commit()) {
		log("checkpoint failed");
		return Result::CheckpointFailed;
	}
	_committed = _written;
	_pendingBytes = 0;
	_checkpointDirty = false;
	log("checkpoint committed");
	return Result::Ok;
}

auto PersistentFileSink::readCheckpoint() -> Result {
	auto file = QFile(checkpointPath());
	if (QFileInfo(file).isSymLink() || !file.open(QIODevice::ReadOnly)
		|| file.size() <= kDigestSize || file.size() > kMaximumCheckpoint) {
		return Result::InvalidCheckpoint;
	}
	const auto record = file.read(kMaximumCheckpoint + 1);
	if (record.size() <= kDigestSize || record.size() > kMaximumCheckpoint) {
		return Result::InvalidCheckpoint;
	}
	const auto body = QByteArrayView(record).first(record.size() - kDigestSize);
	if (Digest(body) != QByteArrayView(record).last(kDigestSize)) {
		return Result::InvalidCheckpoint;
	}
	auto stream = QDataStream(QByteArray::fromRawData(body.data(), body.size()));
	stream.setVersion(QDataStream::Qt_5_1);
	auto magic = quint32();
	auto version = quint32();
	auto id = QUuid();
	auto size = qint64();
	auto part = quint32();
	auto destination = QByteArray();
	auto identity = QByteArray();
	auto destinationIdentity = QByteArray();
	auto bitmap = QBitArray();
	auto digests = QByteArray();
	stream >> magic >> version >> id >> size >> part >> destination
		>> identity >> destinationIdentity >> bitmap >> digests;
	const auto parts = (_job.size + kPersistentDownloadPartSize - 1)
		/ kPersistentDownloadPartSize;
	if (stream.status() != QDataStream::Ok || !stream.atEnd()
		|| magic != kMagic || version != kVersion || id != _job.id
		|| size != _job.size || part != kPersistentDownloadPartSize
		|| destination != Digest(QDir::cleanPath(_job.destination).toUtf8())
		|| (!destinationIdentity.isEmpty() && (!_job.replace
			|| destinationIdentity.size() != _identity.size()))
		|| bitmap.size() != parts || digests.size() != parts * kDigestSize) {
		return Result::InvalidCheckpoint;
	}
	if (identity != _identity) {
		return Result::Replaced;
	}
	_destinationIdentity = std::move(destinationIdentity);
	_writtenSize = 0;
	_checkpointDirty = false;
	for (auto i = qsizetype(0); i != bitmap.size(); ++i) {
		if (!bitmap.testBit(i)) {
			continue;
		}
		const auto offset = qint64(i) * kPersistentDownloadPartSize;
		const auto length = partLength(i);
		const auto bytes = (_file->size() >= offset + length && _file->seek(offset))
			? _file->read(length)
			: QByteArray();
		const auto hash = QByteArrayView(digests).sliced(i * kDigestSize, kDigestSize);
		if (bytes.size() != length || Digest(bytes) != hash) {
			bitmap.clearBit(i);
			_checkpointDirty = true;
		} else {
			_writtenSize += length;
		}
	}
	_written = _committed = std::move(bitmap);
	_digests = std::move(digests);
	_pendingBytes = 0;
	return Result::Ok;
}

auto PersistentFileSink::suspend() -> Result {
	const auto result = checkpoint();
	_file->close();
	log("suspended");
	return result;
}

bool PersistentFileSink::ownsStaging() const {
	return !_identity.isEmpty() && PathIdentity(_stagingPath) == _identity;
}

auto PersistentFileSink::publish() -> Result {
	if (!_file->isOpen() || _writtenSize != _job.size
		|| _written.count(true) != _written.size() || _file->size() != _job.size) {
		return Result::Incomplete;
	}
	if (const auto result = checkpoint(); result != Result::Ok) {
		return result;
	}
	if (!ownsStaging()) {
		return Result::Replaced;
	}
	const auto replace = !_destinationIdentity.isEmpty();
	if (replace && PathIdentity(_job.destination) != _destinationIdentity) {
		return Result::PublishFailed;
	}
	_file->close();
	if (!PublishFile(_stagingPath, _job.destination, replace)) {
		return Result::PublishFailed;
	}
	_published = true;
	log(QFile::remove(checkpointPath())
		? "published"
		: "published, checkpoint cleanup failed");
	return Result::Ok;
}

auto PersistentFileSink::cancel() -> Result {
	_file->close();
	if (!_created || _published) {
		return Result::Ok;
	}
	if (QFileInfo::exists(_stagingPath)) {
		if (!ownsStaging()) {
			return Result::Replaced;
		}
		if (!QFile::remove(_stagingPath)) {
			return Result::RemoveFailed;
		}
	}
	if (QFileInfo::exists(checkpointPath()) && !QFile::remove(checkpointPath())) {
		return Result::RemoveFailed;
	}
	_created = false;
	log("cancelled");
	return Result::Ok;
}

const QBitArray &PersistentFileSink::committed() const {
	return _committed;
}

qint64 PersistentFileSink::writtenSize() const {
	return _writtenSize;
}

QString PersistentFileSink::stagingPath() const {
	return _stagingPath;
}

QString PersistentFileSink::checkpointPath() const {
	return _stagingPath + u".resume"_s;
}

void PersistentFileSink::log(const char *action) const {
	qInfo() << "Persistent download" << _job.id << action
		<< "committed" << _committed.count(true)
		<< "missing" << (_committed.size() - _committed.count(true));
}

} // namespace Storage
