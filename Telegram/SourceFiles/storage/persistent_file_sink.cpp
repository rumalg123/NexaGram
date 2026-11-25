/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "storage/persistent_file_sink.h"

#include "storage/download_policy.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDataStream>
#include <QtCore/QDebug>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QElapsedTimer>
#include <QtCore/QSaveFile>
#include <QtCore/QMessageAuthenticationCode>
#include <QtCore/QScopeGuard>
#include <QtCore/QtEndian>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <optional>
#include <utility>

#ifdef Q_OS_WIN
#include <QtCore/qt_windows.h>
#include <io.h>
#else
#include <sys/file.h>
#include <sys/stat.h>
#if defined(__linux__)
#include <sys/syscall.h>
#include <linux/fs.h>
#endif
#include <unistd.h>
#endif

namespace Storage {
namespace {

using namespace Qt::StringLiterals;

constexpr auto kMagic = quint32(0x36475044);
constexpr auto kLegacyVersion = quint32(1);
constexpr auto kAuthenticatedVersion = quint32(2);
constexpr auto kDigestSize = 32;
constexpr auto kMaximumSize = qint64(4000) * 1024 * 1024;
constexpr auto kCheckpointBytes = qint64(8) * 1024 * 1024;
constexpr auto kMaximumCheckpoint = kMaximumSize / kPersistentDownloadPartSize
	* (kDigestSize + 1) + 512;

QString StagingPath(const PersistentFileSink::Job &job) {
	return QFileInfo(job.destination).absoluteDir().filePath(
		u".64gram-"_s + job.id.toString(QUuid::WithoutBraces) + u".part"_s);
}

QByteArray Digest(QByteArrayView bytes) {
	return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
}

QByteArray CheckpointTag(QByteArrayView body, const QByteArray &key) {
	return key.isEmpty() ? Digest(body)
		: QMessageAuthenticationCode::hash(body, QByteArrayView(key), QCryptographicHash::Sha256);
}

enum class FileAccess { Read, Inspect, Create, Reopen, Remove };

bool OpenFile(QFile &file, const QString &path, FileAccess access) {
	const auto write = access == FileAccess::Create || access == FileAccess::Reopen;
	const auto mode = (write ? QIODevice::ReadWrite : QIODevice::ReadOnly)
		| QIODevice::Unbuffered;
#ifdef Q_OS_WIN
	const auto name = QDir::toNativeSeparators(path).toStdWString();
	const auto rights = GENERIC_READ | (write ? GENERIC_WRITE : 0)
		| ((write || access == FileAccess::Remove) ? DELETE : 0);
	const auto sharing = (access == FileAccess::Read)
		? FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE
		: FILE_SHARE_READ;
	const auto handle = CreateFileW(name.c_str(), rights, sharing, nullptr,
		access == FileAccess::Create ? CREATE_NEW : OPEN_EXISTING,
		FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
	if (handle == INVALID_HANDLE_VALUE) {
		return false;
	}
	const auto fd = _open_osfhandle(reinterpret_cast<intptr_t>(handle),
		_O_BINARY | (write ? _O_RDWR : _O_RDONLY));
	if (fd < 0) {
		CloseHandle(handle);
		return false;
	}
#else
	const auto flags = O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK
		| (write ? O_RDWR : O_RDONLY)
		| ((access == FileAccess::Create) ? O_CREAT | O_EXCL : 0);
	const auto fd = ::open(QFile::encodeName(path).constData(), flags, 0600);
	if (fd < 0) {
		return false;
	}
	const auto lock = (access == FileAccess::Inspect) ? LOCK_SH : LOCK_EX;
	if (access != FileAccess::Read && flock(fd, lock | LOCK_NB)) {
		::close(fd);
		return false;
	}
#endif
	if (file.open(fd, mode, QFileDevice::AutoCloseHandle)) {
		return true;
	}
#ifdef Q_OS_WIN
	_close(fd);
#else
	::close(fd);
#endif
	return false;
}

#ifdef Q_OS_WIN
QByteArray HandleIdentity(HANDLE handle, bool singleLink) {
	auto info = BY_HANDLE_FILE_INFORMATION{};
	if (!GetFileInformationByHandle(handle, &info)
		|| (!info.nFileIndexLow && !info.nFileIndexHigh)
		|| (singleLink && info.nNumberOfLinks != 1)
		|| (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
		return {};
	}
	auto bytes = QByteArray();
	auto stream = QDataStream(&bytes, QIODevice::WriteOnly);
	stream << quint64(info.dwVolumeSerialNumber)
		<< quint64((quint64(info.nFileIndexHigh) << 32) | info.nFileIndexLow)
		<< quint64((quint64(info.ftCreationTime.dwHighDateTime) << 32)
			| info.ftCreationTime.dwLowDateTime);
	return bytes;
}
#else
QByteArray StatIdentity(const struct stat &info, bool singleLink) {
	if (!S_ISREG(info.st_mode) || !info.st_ino || (singleLink && info.st_nlink != 1)) {
		return {};
	}
	auto bytes = QByteArray();
	auto stream = QDataStream(&bytes, QIODevice::WriteOnly);
	stream << quint64(info.st_dev) << quint64(info.st_ino);
	return bytes;
}
#endif

QByteArray Identity(QFile &file, bool singleLink = true) {
#ifdef Q_OS_WIN
	return HandleIdentity(reinterpret_cast<HANDLE>(_get_osfhandle(file.handle())), singleLink);
#else
	struct stat info = {};
	return fstat(file.handle(), &info) ? QByteArray() : StatIdentity(info, singleLink);
#endif
}

QByteArray PathIdentity(const QString &path, bool singleLink = true) {
#ifdef Q_OS_WIN
	const auto name = QDir::toNativeSeparators(path).toStdWString();
	const auto handle = CreateFileW(name.c_str(), FILE_READ_ATTRIBUTES,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
		OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
	if (handle == INVALID_HANDLE_VALUE) {
		return {};
	}
	const auto close = qScopeGuard([&] { CloseHandle(handle); });
	return HandleIdentity(handle, singleLink);
#else
	struct stat info = {};
	return lstat(QFile::encodeName(path).constData(), &info)
		? QByteArray() : StatIdentity(info, singleLink);
#endif
}

bool RemoveOpened(QFile &file, const QString &path) {
	const auto identity = Identity(file);
	if (identity.isEmpty() || PathIdentity(path) != identity) {
		return false;
	}
#ifdef Q_OS_WIN
	auto info = FILE_DISPOSITION_INFO{ TRUE };
	return SetFileInformationByHandle(reinterpret_cast<HANDLE>(_get_osfhandle(file.handle())),
		FileDispositionInfo, &info, sizeof(info)) != 0;
#else
	return ::unlink(QFile::encodeName(path).constData()) == 0;
#endif
}

#ifndef Q_OS_WIN
std::optional<std::array<qint64, 5>> ContentStamp(QFile &file) {
	struct stat info = {};
	if (fstat(file.handle(), &info) || info.st_nlink != 1) {
		return std::nullopt;
	}
#ifdef Q_OS_MAC
	const auto modified = info.st_mtimespec;
	const auto changed = info.st_ctimespec;
#else
	const auto modified = info.st_mtim;
	const auto changed = info.st_ctim;
#endif
	return std::array<qint64, 5>{ info.st_size, modified.tv_sec, modified.tv_nsec,
		changed.tv_sec, changed.tv_nsec };
}
#endif

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

bool PublishFile(
		QFile &file,
		const QString &from,
		const QString &to,
		const std::atomic<bool> *stop = nullptr) {
	const auto directory = QFileInfo(to).absolutePath();
#ifdef Q_OS_WIN
	const auto parentName = QDir::toNativeSeparators(directory).toStdWString();
	const auto parent = CreateFileW(parentName.c_str(), FILE_READ_ATTRIBUTES,
		FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
		FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
	if (parent == INVALID_HANDLE_VALUE) {
		return false;
	}
	const auto close = qScopeGuard([&] { CloseHandle(parent); });
	const auto identity = Identity(file);
	if (identity.isEmpty() || !SafePersistentDownloadPath(to) || PathIdentity(from) != identity) {
		return false;
	}
	const auto target = QDir::toNativeSeparators(to).toStdWString();
	const auto length = DWORD(target.size() * sizeof(wchar_t));
	auto storage = QByteArray(sizeof(FILE_RENAME_INFO) + length, '\0');
	const auto info = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
	info->FileNameLength = length;
	std::memcpy(info->FileName, target.data(), length);
	return (!stop || !stop->load())
		&& SetFileInformationByHandle(reinterpret_cast<HANDLE>(_get_osfhandle(file.handle())),
		FileRenameInfo, info, DWORD(storage.size())) != 0;
#else
	const auto parent = ::open(QFile::encodeName(directory).constData(),
		O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (parent < 0) {
		return false;
	}
	const auto close = qScopeGuard([&] { ::close(parent); });
	const auto source = QFile::encodeName(QFileInfo(from).fileName());
	const auto target = QFile::encodeName(QFileInfo(to).fileName());
	struct stat info = {};
	const auto identity = Identity(file);
	if (identity.isEmpty() || fstatat(parent, source.constData(), &info, AT_SYMLINK_NOFOLLOW)
		|| StatIdentity(info, true) != identity) {
		return false;
	}
	if (stop && stop->load()) {
		return false;
	}
#if defined(SYS_renameat2) && defined(RENAME_NOREPLACE)
	return syscall(SYS_renameat2, parent, source.constData(),
		parent, target.constData(), RENAME_NOREPLACE) == 0;
#elif defined(Q_OS_MAC)
	return renameatx_np(parent, source.constData(), parent, target.constData(), RENAME_EXCL) == 0;
#else
	if (linkat(parent, source.constData(), parent, target.constData(), 0) != 0) {
		return false;
	}
	return unlinkat(parent, source.constData(), 0) == 0;
#endif
#endif
}

bool CreateCheckpoint(const QString &path, const QByteArray &body, const QByteArray &tag) {
	const auto temp = path + u"."_s + QUuid::createUuid().toString(QUuid::WithoutBraces) + u".tmp"_s;
	auto file = QFile(temp);
	if (!SafePersistentDownloadPath(temp) || !OpenFile(file, temp, FileAccess::Create)) {
		return false;
	}
	if (file.write(body) == body.size() && file.write(tag) == tag.size()
		&& FlushFile(file) && PublishFile(file, temp, path)) {
		return true;
	}
	RemoveOpened(file, temp);
	return false;
}

} // namespace

bool FlushPersistentDownloadFile(QFileDevice &file) {
	return FlushFile(file);
}

QByteArray PersistentDownloadAliasIdentity(const QString &path) {
	return PathIdentity(path, false);
}

bool ValidPersistentDownloadPath(const QString &path) {
	if (path.isEmpty() || path.contains(QChar(0)) || !QDir::isAbsolutePath(path)
		|| path.startsWith(u':') || path != QDir::cleanPath(path)) {
		return false;
	}
#ifdef Q_OS_WIN
	if (path.startsWith(u"//?/"_s) || path.startsWith(u"//./"_s)
		|| path.contains(u'\\') || path.startsWith(u'/') != path.startsWith(u"//"_s)) {
		return false;
	}
	const auto unc = path.startsWith(u"//"_s);
	if ((!unc && (path.size() < 3 || !path[0].isLetter() || path[1] != u':' || path[2] != u'/'))
		|| path.indexOf(u':', unc ? 0 : 2) >= 0) {
		return false;
	}
	const auto components = QStringView(path).mid(unc ? 2 : 3).split(u'/');
	if (unc && components.size() < 3) {
		return false;
	}
	for (const auto component : components) {
		if (component.isEmpty() || component.endsWith(u'.') || component.endsWith(u' ')) {
			return false;
		}
		const auto name = component.toString().section(u'.', 0, 0).trimmed().toUpper();
		if (name == u"CON"_s || name == u"PRN"_s || name == u"AUX"_s || name == u"NUL"_s
			|| name == u"CONIN$"_s || name == u"CONOUT$"_s
			|| (name.size() == 4 && (name.startsWith(u"COM"_s) || name.startsWith(u"LPT"_s))
				&& (name.back().isDigit() || QStringView(u"\u00B9\u00B2\u00B3").contains(name.back())))) {
			return false;
		}
	}
#endif
	return !QFileInfo(path).fileName().isEmpty();
}

bool SafePersistentDownloadPath(const QString &path) {
	if (!ValidPersistentDownloadPath(path)) {
		return false;
	}
	auto current = QFileInfo(path).absoluteFilePath();
	while (true) {
		const auto info = QFileInfo(current);
		if (info.isSymLink()) {
			return false;
		}
#ifdef Q_OS_WIN
		const auto native = QDir::toNativeSeparators(current).toStdWString();
		const auto attributes = GetFileAttributesW(native.c_str());
		if (attributes == INVALID_FILE_ATTRIBUTES) {
			const auto error = GetLastError();
			if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
				return false;
			}
		} else if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
			return false;
		}
#else
		struct stat entry = {};
		if (lstat(QFile::encodeName(current).constData(), &entry)) {
			if (errno != ENOENT) {
				return false;
			}
		} else if (S_ISLNK(entry.st_mode)) {
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
	if (!device) {
		return false;
	}
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
, _stagingPath(StagingPath(_job)) {
	_file->setFileName(_stagingPath);
}

auto PersistentFileSink::open(
		bool resume,
		QByteArrayView expectedIdentity,
		bool deferCheckpoint,
		bool allowEmptyRecovery,
		const std::atomic<bool> *stop) -> Result {
	if (_job.id.isNull() || (!_job.checkpointKey.isEmpty() && _job.checkpointKey.size() != kDigestSize)
		|| !ValidPersistentDownloadPath(_job.destination)
		|| IsPendingDownloadArtifactName(QFileInfo(_job.destination).fileName())
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
	if (!OpenFile(*_file, _stagingPath, resume ? FileAccess::Reopen : FileAccess::Create)) {
		return (resume && !info.exists()) ? Result::Missing : Result::WriteFailed;
	}
	const auto identity = Identity(*_file);
	auto result = Result::Ok;
	if (identity.isEmpty() || (!expectedIdentity.isEmpty()
		&& identity != expectedIdentity)) {
		result = Result::Replaced;
	} else if (resume) {
		auto previousIdentity = std::exchange(_identity, identity);
		result = readCheckpoint(true, nullptr, stop);
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
	_checkpointDigest.clear();
	_checkpointIdentity.clear();
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
	auto timer = QElapsedTimer();
	if (PersistentDownloadPerformanceLog()) {
		timer.start();
	}
	if (!_file->isOpen() || !ownsStaging() || !FlushFile(*_file)) {
		log("data flush failed");
		return Result::CheckpointFailed;
	}
	const auto flushMs = timer.isValid() ? timer.elapsed() : 0;
	auto body = QByteArray();
	auto stream = QDataStream(&body, QIODevice::WriteOnly);
	stream.setVersion(QDataStream::Qt_5_1);
	const auto version = _job.checkpointKey.isEmpty() ? kLegacyVersion : kAuthenticatedVersion;
	stream << kMagic << version << _job.id << _job.size
		<< quint32(kPersistentDownloadPartSize)
		<< Digest(QDir::cleanPath(_job.destination).toUtf8())
		<< _identity << _destinationIdentity << _written << _digests;
	const auto digest = CheckpointTag(body, _job.checkpointKey);
	const auto path = checkpointPath();
	const auto persist = [&] {
		if (stream.status() != QDataStream::Ok || !SafePersistentDownloadPath(path)) {
			return false;
		} else if (_checkpointDigest.isEmpty()) {
			return CreateCheckpoint(path, body, digest);
		}
		auto file = QSaveFile(path);
		file.setDirectWriteFallback(false);
		return file.open(QIODevice::WriteOnly)
			&& file.write(body) == body.size() && file.write(digest) == digest.size()
			&& FlushFile(file) && readCheckpoint(false) == Result::Ok && file.commit();
	};
	if (!persist()) {
		log("checkpoint failed");
		return Result::CheckpointFailed;
	}
	_checkpointIdentity = PathIdentity(path);
	if (_checkpointIdentity.isEmpty()) {
		return Result::CheckpointFailed;
	}
	_checkpointDigest = digest;
	if (timer.isValid()) {
		qInfo() << "Download perf" << _job.id << "checkpoint bytes" << body.size() + digest.size()
			<< "unique bytes" << _pendingBytes << "flush ms" << flushMs << "total ms" << timer.elapsed();
	}
	_committed = _written;
	_committedSize = _writtenSize;
	_pendingBytes = 0;
	_checkpointDirty = false;
	log("checkpoint committed");
	return Result::Ok;
}

auto PersistentFileSink::readCheckpoint(
		bool verify,
		QFile *opened,
		const std::atomic<bool> *stop) -> Result {
	if (_job.size <= 0 || _job.size > kMaximumSize || _job.id.isNull()
		|| (!_job.checkpointKey.isEmpty() && _job.checkpointKey.size() != kDigestSize)) {
		return Result::InvalidJob;
	}
	auto local = QFile(checkpointPath());
	auto &file = opened ? *opened : local;
	if (!SafePersistentDownloadPath(checkpointPath())
		|| (!opened && !OpenFile(file, checkpointPath(), FileAccess::Read))
		|| file.size() <= kDigestSize || file.size() > kMaximumCheckpoint) {
		return Result::InvalidCheckpoint;
	}
	const auto sidecar = Identity(file);
	if (sidecar.isEmpty() || (!verify && !_checkpointIdentity.isEmpty() && sidecar != _checkpointIdentity)) {
		return Result::Replaced;
	}
	const auto record = file.read(kMaximumCheckpoint + 1);
	if (record.size() <= kDigestSize || record.size() > kMaximumCheckpoint) {
		return Result::InvalidCheckpoint;
	}
	const auto body = QByteArrayView(record).first(record.size() - kDigestSize);
	const auto digest = CheckpointTag(body, _job.checkpointKey);
	if (digest != QByteArrayView(record).last(kDigestSize)
		|| (!verify && !_checkpointDigest.isEmpty() && _checkpointDigest != digest)) {
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
		|| magic != kMagic || version != (_job.checkpointKey.isEmpty() ? kLegacyVersion : kAuthenticatedVersion)
		|| id != _job.id
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
	const auto recordedCount = bitmap.count(true);
	auto verified = qint64();
	const auto result = verifyParts(bitmap, digests, verified, stop);
	if (result != Result::Ok) {
		return result;
	}
	_destinationIdentity = std::move(destinationIdentity);
	_checkpointIdentity = sidecar;
	_checkpointDigest = digest;
	_checkpointDirty = recordedCount != bitmap.count(true);
	_written = _committed = std::move(bitmap);
	_writtenSize = _committedSize = verified;
	_digests = std::move(digests);
	_pendingBytes = 0;
	return Result::Ok;
}

auto PersistentFileSink::verifyParts(
		QBitArray &bitmap,
		const QByteArray &digests,
		qint64 &verified,
		const std::atomic<bool> *stop) -> Result {
	auto timer = QElapsedTimer();
	if (PersistentDownloadPerformanceLog()) {
		timer.start();
	}
	const auto size = _file->size();
	if (size < 0) {
		return Result::WriteFailed;
	}
	auto buffer = QByteArray(kPersistentDownloadPartSize, Qt::Uninitialized);
	auto hash = QCryptographicHash(QCryptographicHash::Sha256);
	for (auto i = qsizetype(0); i != bitmap.size(); ++i) {
		if (stop && stop->load(std::memory_order_relaxed)) {
			return Result::TransferFailed;
		} else if (!bitmap.testBit(i)) {
			continue;
		}
		const auto offset = qint64(i) * kPersistentDownloadPartSize;
		const auto length = partLength(i);
		if (offset + length > size) {
			bitmap.clearBit(i);
			continue;
		} else if (_file->pos() != offset && !_file->seek(offset)) {
			return Result::SeekFailed;
		} else if (_file->read(buffer.data(), length) != length) {
			return Result::WriteFailed;
		}
		hash.reset();
		hash.addData(QByteArrayView(buffer).first(length));
		if (hash.resultView() == QByteArrayView(digests).sliced(i * kDigestSize, kDigestSize)) {
			verified += length;
		} else {
			bitmap.clearBit(i);
		}
	}
	if (timer.isValid()) {
		qInfo() << "Download perf" << _job.id << "verify valid bytes" << verified
			<< "ms" << timer.elapsed();
	}
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

auto PersistentFileSink::publish(bool keepCheckpoint, const std::atomic<bool> *stop) -> Result {
	auto timer = QElapsedTimer();
	if (PersistentDownloadPerformanceLog()) {
		timer.start();
	}
	const auto measured = qScopeGuard([&] {
		if (timer.isValid()) {
			qInfo() << "Download perf" << _job.id << "publication ok" << _published
				<< "ms" << timer.elapsed();
		}
	});
	if (const auto result = preparePublish(); result != Result::Ok) {
		return result;
	}
	if (!SafePersistentDownloadPath(_stagingPath)
		|| !SafePersistentDownloadPath(_job.destination) || !ownsStaging()) {
		return Result::Replaced;
	}
#ifndef Q_OS_WIN
	const auto stamp = ContentStamp(*_file);
#endif
	auto present = _committed;
	auto verified = qint64();
	const auto checked = verifyParts(present, _digests, verified, stop);
	if (checked != Result::Ok) {
		return checked;
	} else if (verified != _job.size || _file->size() != _job.size) {
		return Result::Incomplete;
	} else if (!ownsStaging() || !SafePersistentDownloadPath(_job.destination)
		|| readCheckpoint(false) != Result::Ok || (stop && stop->load())) {
		return Result::Replaced;
	}
#ifndef Q_OS_WIN
	if (!stamp || ContentStamp(*_file) != stamp) {
		return Result::Replaced;
	}
#endif
	if (!PublishFile(*_file, _stagingPath, _job.destination, stop)) {
		return Result::PublishFailed;
	}
	_file->close();
	_published = true;
	log((keepCheckpoint || removePublishedCheckpoint())
		? "published"
		: "published, checkpoint cleanup failed");
	return Result::Ok;
}

auto PersistentFileSink::inspectForResume(QByteArrayView expectedIdentity) -> Result {
	if (expectedIdentity.isEmpty() || !SafePersistentDownloadPath(_stagingPath)) {
		return Result::Replaced;
	}
	_identity = PathIdentity(_stagingPath);
	if (_identity.isEmpty() || _identity != expectedIdentity) {
		return PersistentDownloadPathMissing(_stagingPath) ? Result::Missing : Result::Replaced;
	}
	return readCheckpoint(false);
}

auto PersistentFileSink::inspectPublished(
		QByteArrayView expectedIdentity,
		const std::atomic<bool> *stop) -> Result {
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
	if (!OpenFile(*_file, _job.destination, FileAccess::Inspect)) {
		return QFileInfo::exists(_job.destination) ? Result::WriteFailed : Result::Missing;
	}
	_identity = Identity(*_file);
#ifndef Q_OS_WIN
	struct stat info = {};
	const auto linked = _identity.isEmpty() && !fstat(_file->handle(), &info)
		&& info.st_nlink == 2 && Identity(*_file, false) == expectedIdentity
		&& SafePersistentDownloadPath(_stagingPath)
		&& PathIdentity(_stagingPath, false) == expectedIdentity;
	if (linked) {
		_identity = expectedIdentity.toByteArray();
	}
#endif
	if (_identity != expectedIdentity) {
		return Result::Replaced;
	}
	const auto result = readCheckpoint(true, nullptr, stop);
	if (result != Result::Ok) {
		return result;
	}
	if (_writtenSize != _job.size || _file->size() != _job.size) {
		return Result::Incomplete;
	} else if (PathIdentity(_job.destination, false) != expectedIdentity) {
		return Result::Replaced;
	}
#ifndef Q_OS_WIN
	if (linked && (PathIdentity(_job.destination, false) != _identity
		|| PathIdentity(_stagingPath, false) != _identity
		|| ::unlink(QFile::encodeName(_stagingPath).constData()))) {
		return Result::RemoveFailed;
	}
#endif
	return Result::Ok;
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
	auto file = QFile(checkpointPath());
	return SafePersistentDownloadPath(checkpointPath())
		&& OpenFile(file, checkpointPath(), FileAccess::Remove)
		&& readCheckpoint(false, &file) == Result::Ok
		&& RemoveOpened(file, checkpointPath());
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
		auto file = QFile(_stagingPath);
		if (!SafePersistentDownloadPath(_stagingPath)
			|| !OpenFile(file, _stagingPath, FileAccess::Remove)
			|| _identity.isEmpty() || Identity(file) != _identity) {
			return Result::Replaced;
		}
		if (!RemoveOpened(file, _stagingPath)) {
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

QString PersistentFileSink::CheckpointPath(const Job &job) {
	return StagingPath(job) + u".resume"_s;
}

QString PersistentFileSink::stagingPath() const {
	return _stagingPath;
}

QString PersistentFileSink::checkpointPath() const {
	return _stagingPath + u".resume"_s;
}

void PersistentFileSink::log(const char *action) const {
	if (DownloadOverrideValue(PersistentDownloadOverrides().trace).value_or(false)) {
		qInfo() << "Persistent download" << _job.id << action
			<< "committed" << _committed.count(true)
			<< "missing" << (_committed.size() - _committed.count(true));
	}
}

} // namespace Storage
