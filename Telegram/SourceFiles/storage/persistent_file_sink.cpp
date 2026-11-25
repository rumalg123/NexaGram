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
#include <QtCore/QScopeGuard>
#include <QtCore/QtEndian>

#include <algorithm>
#include <cerrno>
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
	const auto info = QFileInfo(file);
	return (info.isFile() && !info.isSymLink()
		&& file.open(QIODevice::ReadOnly | QIODevice::ExistingOnly))
		? Identity(file)
		: QByteArray();
}

bool FlushFile(QFileDevice &file) {
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
		MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0)) != 0;
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

bool FlushPersistentDownloadFile(QFileDevice &file) {
	return FlushFile(file);
}

bool SafePersistentDownloadPath(const QString &path) {
	auto current = QFileInfo(path).absoluteFilePath();
	while (true) {
		const auto info = QFileInfo(current);
		if (info.isSymLink()) {
			return false;
		}
#ifdef Q_OS_WIN
		const auto native = QDir::toNativeSeparators(current).toStdWString();
		const auto attributes = GetFileAttributesW(native.c_str());
		if (attributes != INVALID_FILE_ATTRIBUTES
			&& (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
			return false;
		}
#endif
		const auto parent = info.absolutePath();
		if (current == parent) {
			return true;
		}
		current = parent;
	}
}

bool PersistentDownloadPathMissing(const QString &path) {
#ifdef Q_OS_WIN
	const auto native = QDir::toNativeSeparators(path).toStdWString();
	if (GetFileAttributesW(native.c_str()) != INVALID_FILE_ATTRIBUTES) {
		return false;
	}
	const auto error = GetLastError();
	return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
#else
	struct stat info = {};
	return lstat(QFile::encodeName(path).constData(), &info) != 0 && errno == ENOENT;
#endif
}

bool FitsPersistentDownloadField(QDataStream &stream, quint32 limit, bool bitCount) {
	const auto device = stream.device();
	const auto header = device->peek(sizeof(quint32));
	if (stream.status() != QDataStream::Ok || header.size() != sizeof(quint32)) {
		return false;
	}
	const auto length = qFromBigEndian<quint32>(header.constData());
	if (!bitCount && length == 0xFFFFFFFFU) {
		return true;
	}
	const auto bytes = bitCount ? ((quint64(length) + 7) / 8) : quint64(length);
	return length <= limit && device->bytesAvailable() >= qint64(bytes + header.size());
}

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

auto PersistentFileSink::open(
		bool resume,
		QByteArrayView expectedIdentity,
		bool deferCheckpoint,
		bool allowEmptyRecovery) -> Result {
	if (_job.id.isNull() || !QFileInfo(_job.destination).isAbsolute()
		|| _job.size <= 0 || _job.size > kMaximumSize || _published
		|| _file->isOpen()) {
		return Result::InvalidJob;
	}
	if (!SafePersistentDownloadPath(_stagingPath)
		|| !SafePersistentDownloadPath(checkpointPath())
		|| !SafePersistentDownloadPath(_job.destination)) {
		return Result::Replaced;
	}
	if (!resume && !PersistentDownloadPathMissing(checkpointPath())) {
		return Result::Replaced;
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
	if (identity.isEmpty() || (!expectedIdentity.isEmpty()
		&& identity != expectedIdentity)) {
		result = Result::Replaced;
	} else if (resume) {
		auto previousIdentity = std::exchange(_identity, identity);
		result = readCheckpoint();
		if (result == Result::InvalidCheckpoint && allowEmptyRecovery
			&& !expectedIdentity.isEmpty()
			&& PersistentDownloadPathMissing(checkpointPath()) && !_file->size()) {
			_destinationIdentity.clear();
			initializeCoverage();
			result = checkpoint();
		}
		if (result != Result::Ok) {
			_identity = std::move(previousIdentity);
		}
	} else {
		_identity = identity;
		initializeCoverage();
		if (!deferCheckpoint) {
			result = checkpoint();
			if (result != Result::Ok && cancel() == Result::Ok) {
				_checkpointDirty = false;
			}
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

void PersistentFileSink::initializeCoverage() {
	const auto parts = (_job.size + kPersistentDownloadPartSize - 1)
		/ kPersistentDownloadPartSize;
	_written = QBitArray(parts);
	_committed = QBitArray(parts);
	_digests = QByteArray(parts * kDigestSize, '\0');
	_writtenSize = 0;
	_committedSize = 0;
	_pendingBytes = 0;
	_created = true;
	_checkpointDirty = true;
}

const QByteArray &PersistentFileSink::identity() const {
	return _identity;
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
	if (!_file->isOpen() || !ownsStaging() || !FlushFile(*_file)) {
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
	if (stream.status() != QDataStream::Ok
		|| !SafePersistentDownloadPath(checkpointPath())
		|| !file.open(QIODevice::WriteOnly)
		|| file.write(body) != body.size() || file.write(digest) != digest.size()
		|| !FlushFile(file) || !file.commit()) {
		log("checkpoint failed");
		return Result::CheckpointFailed;
	}
	_committed = _written;
	_committedSize = _writtenSize;
	_pendingBytes = 0;
	_checkpointDirty = false;
	log("checkpoint committed");
	return Result::Ok;
}

auto PersistentFileSink::readCheckpoint(bool verify) -> Result {
	auto file = QFile(checkpointPath());
	if (!SafePersistentDownloadPath(checkpointPath()) || !QFileInfo(file).isFile()
		|| !file.open(QIODevice::ReadOnly | QIODevice::ExistingOnly)
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
	const auto parts = (_job.size + kPersistentDownloadPartSize - 1)
		/ kPersistentDownloadPartSize;
	stream >> magic >> version >> id >> size >> part;
	if (!FitsPersistentDownloadField(stream, kDigestSize)) {
		return Result::InvalidCheckpoint;
	}
	stream >> destination;
	if (!FitsPersistentDownloadField(stream, 24)) {
		return Result::InvalidCheckpoint;
	}
	stream >> identity;
	if (!FitsPersistentDownloadField(stream, 24)) {
		return Result::InvalidCheckpoint;
	}
	stream >> destinationIdentity;
	if (!FitsPersistentDownloadField(stream, quint32(parts), true)) {
		return Result::InvalidCheckpoint;
	}
	stream >> bitmap;
	if (!FitsPersistentDownloadField(stream, quint32(parts * kDigestSize))) {
		return Result::InvalidCheckpoint;
	}
	stream >> digests;
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
	if (!verify) {
		return Result::Ok;
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
	_committedSize = _writtenSize;
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

auto PersistentFileSink::preparePublish() -> Result {
	if (!_file->isOpen() || _writtenSize != _job.size
		|| _written.count(true) != _written.size() || _file->size() != _job.size) {
		return Result::Incomplete;
	}
	return checkpoint();
}

auto PersistentFileSink::publish(bool keepCheckpoint) -> Result {
	if (const auto result = preparePublish(); result != Result::Ok) {
		return result;
	}
	if (!SafePersistentDownloadPath(_stagingPath)
		|| !SafePersistentDownloadPath(_job.destination) || !ownsStaging()) {
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
	log((keepCheckpoint || QFile::remove(checkpointPath()))
		? "published"
		: "published, checkpoint cleanup failed");
	return Result::Ok;
}

auto PersistentFileSink::inspectPublished(QByteArrayView expectedIdentity)
-> Result {
	if (expectedIdentity.isEmpty() || _file->isOpen()
		|| !SafePersistentDownloadPath(_job.destination)
		|| !SafePersistentDownloadPath(checkpointPath())) {
		return Result::Replaced;
	}
	_file->setFileName(_job.destination);
	const auto restore = qScopeGuard([&] {
		_file->close();
		_file->setFileName(_stagingPath);
	});
	if (!QFileInfo(_job.destination).isFile()) {
		return PersistentDownloadPathMissing(_job.destination)
			? Result::Missing : Result::Replaced;
	}
	if (!_file->open(QIODevice::ReadOnly | QIODevice::ExistingOnly)) {
		return QFileInfo::exists(_job.destination) ? Result::WriteFailed : Result::Missing;
	}
	_identity = Identity(*_file);
	if (_identity != expectedIdentity) {
		return Result::Replaced;
	}
	const auto result = readCheckpoint();
	if (result != Result::Ok) {
		return result;
	}
	return (_writtenSize == _job.size && _file->size() == _job.size)
		? Result::Ok
		: Result::Incomplete;
}

bool PersistentFileSink::matchesPublished(QByteArrayView expectedIdentity) const {
	return !expectedIdentity.isEmpty()
		&& SafePersistentDownloadPath(_job.destination)
		&& PathIdentity(_job.destination) == expectedIdentity
		&& QFileInfo(_job.destination).size() == _job.size;
}

bool PersistentFileSink::removePublishedCheckpoint() {
	if (PersistentDownloadPathMissing(checkpointPath())) {
		return true;
	}
	return SafePersistentDownloadPath(checkpointPath())
		&& readCheckpoint(false) == Result::Ok
		&& QFile::remove(checkpointPath());
}

auto PersistentFileSink::purge(QByteArrayView expectedIdentity) -> Result {
	if (!SafePersistentDownloadPath(_stagingPath)
		|| !SafePersistentDownloadPath(checkpointPath())
		|| !QFileInfo(_stagingPath).absoluteDir().exists()) {
		return Result::RemoveFailed;
	}
	if (expectedIdentity.isEmpty()) {
		return (PersistentDownloadPathMissing(_stagingPath)
			&& PersistentDownloadPathMissing(checkpointPath()))
			? Result::Ok
			: Result::Replaced;
	}
	_identity = expectedIdentity.toByteArray();
	_created = true;
	return cancel();
}

auto PersistentFileSink::cancel() -> Result {
	_file->close();
	if (!_created || _published) {
		return Result::Ok;
	}
	if (!PersistentDownloadPathMissing(_stagingPath)) {
		if (!SafePersistentDownloadPath(_stagingPath) || !ownsStaging()) {
			return Result::Replaced;
		}
		if (!QFile::remove(_stagingPath)) {
			return Result::RemoveFailed;
		}
	}
	if (!removePublishedCheckpoint()) {
		return Result::RemoveFailed;
	}
	_created = false;
	log("cancelled");
	return Result::Ok;
}

const QBitArray &PersistentFileSink::committed() const {
	return _committed;
}

DownloadIssue DownloadIssueFromSink(PersistentFileSink::Result result) {
	using Result = PersistentFileSink::Result;
	switch (result) {
	case Result::Ok: return DownloadIssue::None;
	case Result::Missing: return DownloadIssue::MissingFile;
	case Result::Replaced: return DownloadIssue::ChangedFile;
	case Result::InvalidCheckpoint:
	case Result::CheckpointFailed: return DownloadIssue::Checkpoint;
	case Result::SeekFailed:
	case Result::WriteFailed:
	case Result::RemoveFailed: return DownloadIssue::Storage;
	case Result::PublishFailed: return DownloadIssue::Destination;
	default: return DownloadIssue::Transfer;
	}
}

qint64 PersistentFileSink::committedSize() const {
	return _committedSize;
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
	if (qEnvironmentVariableIntValue("TDESKTOP_PERSISTENT_DOWNLOAD_TRACE") == 1) {
		qInfo() << "Persistent download" << _job.id << action
			<< "committed" << _committed.count(true)
			<< "missing" << (_committed.size() - _committed.count(true));
	}
}

} // namespace Storage
