/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "storage/persistent_file_sink.h"
#include "storage/pending_downloads.h"
#include "storage/download_presentation.h"
#include "storage/download_policy.h"
#include "media/streaming/media_streaming_download_attachment.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDataStream>
#include <QtCore/QDebug>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QMap>
#include <QtCore/QProcess>
#include <QtCore/QTemporaryDir>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <numeric>
#include <random>

#ifdef Q_OS_WIN
#include <QtCore/qt_windows.h>
#include <winioctl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {

using namespace Qt::StringLiterals;
using Sink = Storage::PersistentFileSink;
using Result = Sink::Result;
constexpr auto kPart = Storage::kPersistentDownloadPartSize;

void Check(bool condition, const char *what) {
	if (!condition) {
		qFatal("Persistent file sink test failed: %s", what);
	}
}

Sink::Job Job(const QTemporaryDir &dir, QString name, qint64 size) {
	return { QUuid::createUuid(), dir.filePath(name), size };
}

QByteArray Read(const QString &path) {
	auto file = QFile(path);
	Check(file.open(QIODevice::ReadOnly), "open for read");
	return file.readAll();
}

void Put(const QString &path, QByteArrayView bytes) {
	auto file = QFile(path);
	Check(file.open(QIODevice::WriteOnly), "open for write");
	Check(file.write(bytes.data(), bytes.size()) == bytes.size(), "write fixture");
}

QByteArray Sha256(QByteArrayView bytes) {
	return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
}

class FaultFile final : public QFile {
public:
	enum class Fault {
		None,
		Seek,
		ShortWrite,
		DiskFull,
		WrongBytes,
	};
	Fault fault = Fault::None;

	bool makeSparse() {
#ifdef Q_OS_WIN
		const auto native = reinterpret_cast<HANDLE>(_get_osfhandle(handle()));
		auto count = DWORD();
		return DeviceIoControl(native, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &count, nullptr);
#else
		return true;
#endif
	}

	bool seek(qint64 offset) override {
		return (fault != Fault::Seek) && QFile::seek(offset);
	}

protected:
	qint64 writeData(const char *data, qint64 length) override {
		switch (fault) {
		case Fault::WrongBytes: {
			auto changed = QByteArray(data, length);
			changed[0] = char(changed.at(0) ^ 1);
			return QFile::writeData(changed.constData(), changed.size());
		}
		case Fault::ShortWrite:
			return QFile::writeData(data, length / 2);
		case Fault::DiskFull:
			setErrorString(u"Injected disk full"_s);
			return -1;
		default: return QFile::writeData(data, length);
		}
	}
};

void TestRoundTrip(const QTemporaryDir &dir) {
	const auto job = Job(dir, u"round-trip.bin"_s, 3 * kPart + 23);
	const auto first = QByteArray(kPart, 'a');
	const auto second = QByteArray(kPart, 'b');
	const auto third = QByteArray(kPart, 'c');
	const auto tail = QByteArray(23, 'd');
	{
		auto sink = Sink(job);
		Check(sink.open(false) == Result::Ok, "create staging");
		Check(!QFileInfo::exists(job.destination), "final path absent");
		Check(sink.write(2 * kPart, third) == Result::Ok, "out of order");
		Check(sink.write(2 * kPart, third) == Result::Ok, "duplicate");
		Check(sink.write(2 * kPart, first) == Result::InvalidPart, "conflicting duplicate");
		Check(sink.writtenSize() == kPart, "duplicate does not advance");
		Check(!sink.committed().count(true), "write is not a commit");
		Check(sink.write(0, first) == Result::Ok, "write first");
		Check(sink.write(3 * kPart, tail) == Result::Ok, "short final chunk");
		Check(sink.publish() == Result::Incomplete, "hole blocks publication");
		Check(sink.suspend() == Result::Ok, "force checkpoint on suspend");
		Check(sink.committed().count(true) == 3, "three committed chunks");
	}
	auto sink = Sink(job);
	Check(sink.open(true) == Result::Ok, "resume new object");
	auto missing = Storage::PersistentDownloadRequests();
	missing.reset(sink.committed());
	Check(!missing.empty() && missing.take() == kPart && missing.empty(),
		"request only the missing out-of-order offset");
	Check(sink.write(kPart, second) == Result::Ok, "write only missing chunk");
	Check(sink.publish() == Result::Ok, "publish complete file");
	Check(Sha256(Read(job.destination)) == Sha256(first + second + third + tail),
		"published bytes match expected SHA-256");
	Check(!QFileInfo::exists(sink.stagingPath()), "staging removed");
	Check(!QFileInfo::exists(sink.checkpointPath()), "checkpoint removed");
}

void TestFaults(const QTemporaryDir &dir) {
	const auto job = Job(dir, u"faults.bin"_s, 2 * kPart);
	auto fixture = std::make_unique<FaultFile>();
	const auto device = fixture.get();
	auto sink = Sink(job, std::move(fixture));
	const auto data = QByteArray(kPart, 'x');
	Check(sink.open(false) == Result::Ok, "create fault fixture");
	device->fault = FaultFile::Fault::Seek;
	Check(sink.write(0, data) == Result::SeekFailed, "failed seek");
	device->fault = FaultFile::Fault::ShortWrite;
	Check(sink.write(0, data) == Result::WriteFailed, "short write");
	device->fault = FaultFile::Fault::DiskFull;
	Check(sink.write(kPart, data) == Result::WriteFailed, "disk full");
	Check(sink.writtenSize() == 0 && !sink.committed().count(true), "failed writes are missing");
	device->fault = FaultFile::Fault::None;
	Check(sink.write(0, data) == Result::Ok, "retransmit short write");
	Check(sink.checkpoint() == Result::Ok, "commit first part");
	const auto oldCheckpoint = Read(sink.checkpointPath());
#ifdef Q_OS_WIN
	const auto name = QDir::toNativeSeparators(sink.checkpointPath()).toStdWString();
	const auto locked = CreateFileW(name.c_str(), GENERIC_READ, FILE_SHARE_READ,
		nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	Check(locked != INVALID_HANDLE_VALUE, "lock checkpoint replacement");
#endif
	Check(sink.write(kPart, data) == Result::Ok, "write second part");
#ifdef Q_OS_WIN
	Check(sink.checkpoint() == Result::CheckpointFailed, "replace checkpoint fails");
	Check(CloseHandle(locked), "unlock checkpoint");
	Check(sink.committed().count(true) == 1, "failed checkpoint stays uncommitted");
	Check(Read(sink.checkpointPath()) == oldCheckpoint, "old checkpoint survives");
#endif
	Check(sink.checkpoint() == Result::Ok, "retry checkpoint");
	Check(sink.cancel() == Result::Ok, "cancel deletes staging");
	Check(!QFileInfo::exists(job.destination)
		&& !QFileInfo::exists(sink.stagingPath())
		&& !QFileInfo::exists(sink.checkpointPath()), "cancel did not publish");
}

void TestRecovery(const QTemporaryDir &dir) {
	const auto job = Job(dir, u"corruption.bin"_s, 3 * kPart);
	auto sink = Sink(job);
	const auto data = QByteArray(kPart, 'f');
	Check(sink.open(false) == Result::Ok, "create integrity fixture");
	for (auto index = 0; index != 3; ++index) {
		Check(sink.write(qint64(index) * kPart, data) == Result::Ok, "write fixture part");
	}
	Check(sink.suspend() == Result::Ok, "checkpoint fixture");
	{
		auto file = QFile(sink.stagingPath());
		Check(file.open(QIODevice::ReadWrite), "reopen fixture");
		Check(file.seek(kPart) && file.write("z", 1) == 1, "corrupt part");
		Check(file.resize(2 * kPart + 16), "truncate part");
	}
	Check(sink.open(true) == Result::Ok, "recover surviving ranges");
	auto missing = Storage::PersistentDownloadRequests();
	missing.reset(sink.committed());
	Check(missing.take() == kPart && missing.take() == 2 * kPart && missing.empty(),
		"request only corrupt and truncated offsets");
	Check(sink.write(kPart, data) == Result::Ok, "repair corrupt part");
	Check(sink.write(2 * kPart, data) == Result::Ok, "repair truncated part");
	Check(sink.publish() == Result::Ok, "publish repaired bytes");
	Check(Sha256(Read(job.destination)) == Sha256(data + data + data), "repaired SHA-256");
}

void TestReplaced(const QTemporaryDir &dir) {
	const auto job = Job(dir, u"replaced.bin"_s, kPart);
	auto sink = Sink(job);
	Check(sink.open(false) == Result::Ok && sink.suspend() == Result::Ok,
		"checkpoint empty staging");
	const auto moved = dir.filePath(u"other.bin"_s);
	Check(QFile::rename(sink.stagingPath(), moved), "move owned staging");
	Check(sink.open(true) == Result::Missing, "missing is not recreated");
	Check(!QFileInfo::exists(sink.stagingPath()), "staging still missing");
	Put(sink.stagingPath(), "foreign");
	Check(sink.open(true) == Result::Replaced, "reject replaced staging");
	Check(sink.cancel() == Result::Replaced, "leave foreign file untouched");
	Check(Read(sink.stagingPath()) == "foreign", "foreign contents unchanged");
}

void TestDestination(const QTemporaryDir &dir) {
	auto job = Job(dir, u"occupied.bin"_s, 1);
	auto sink = Sink(job);
	const auto original = QByteArray("original");
	Put(job.destination, original);
	Check(sink.open(false) == Result::Ok && sink.write(0, "x") == Result::Ok,
		"prepare finalization");
	Check(sink.publish() == Result::PublishFailed, "no automatic overwrite");
	Check(Read(job.destination) == original, "occupied destination stays intact");
	Check(sink.cancel() == Result::Ok, "clean staging after collision");
	job.id = QUuid::createUuid();
	job.replace = true;
	auto replace = Sink(job);
	Check(replace.open(false) == Result::Ok && replace.write(0, "x") == Result::Ok,
		"prepare approved replacement");
	Check(Read(job.destination) == original, "keep prior file until complete");
	Check(replace.publish() == Result::PublishFailed && Read(job.destination) == original,
		"historical overwrite intent cannot replace an occupied path");
	Check(QFile::rename(job.destination, dir.filePath(u"saved-original.bin"_s)),
		"user moves the prior destination");
	Check(replace.publish() == Result::Ok && Read(job.destination) == "x",
		"retry publishes committed bytes at the freed name");
}

void TestDestinationReplaced(const QTemporaryDir &dir) {
	auto job = Job(dir, u"destination-replaced.bin"_s, 1);
	job.replace = true;
	Put(job.destination, "initial output");
	auto sink = Sink(job);
	Check(sink.open(false) == Result::Ok && sink.write(0, "x") == Result::Ok
		&& sink.suspend() == Result::Ok, "checkpoint approved overwrite");
	Check(QFile::rename(job.destination, dir.filePath(u"other-output.bin"_s)),
		"move approved destination");
	Put(job.destination, "foreign output");
	auto resumed = Sink(job);
	Check(resumed.open(true) == Result::Ok, "restore overwrite identity");
	Check(resumed.publish() == Result::PublishFailed, "reject changed destination");
	Check(Read(job.destination) == "foreign output", "leave changed destination intact");
	Check(resumed.cancel() == Result::Ok, "clean after rejected overwrite");
}

constexpr auto kAbruptExitCode = 37;

void WriteAndExit(const QString &id, const QString &path) {
	auto sink = Sink({ QUuid(id), path, 2 * kPart });
	const auto data = QByteArray(kPart, 'k');
	Check(sink.open(false) == Result::Ok && sink.write(0, data) == Result::Ok
		&& sink.checkpoint() == Result::Ok, "persist one chunk in subprocess");
	Check(sink.write(kPart, data) == Result::Ok, "write without checkpoint");
	std::_Exit(kAbruptExitCode);
}

void TestAbruptExit(const QTemporaryDir &dir) {
	const auto job = Job(dir, u"interrupted.bin"_s, 2 * kPart);
	auto child = QProcess();
	child.start(QCoreApplication::applicationFilePath(), {
		u"--write-and-exit"_s,
		job.id.toString(),
		job.destination,
	});
	Check(child.waitForFinished() && child.exitCode() == kAbruptExitCode,
		"subprocess exited without unwinding");
	auto sink = Sink(job);
	Check(sink.open(true) == Result::Ok, "recover after process exit");
	auto missing = Storage::PersistentDownloadRequests();
	missing.reset(sink.committed());
	Check(sink.writtenSize() == kPart && missing.take() == kPart && missing.empty(),
		"only persisted checkpoint survives process exit");
	Check(sink.cancel() == Result::Ok, "clean interrupted job");
}

void ExitAtPublication(const QUuid &id, const QString &path, bool published) {
	auto sink = Sink({ id, path, 3, false, QByteArray(32, 'q') });
	Check(sink.open(false) == Result::Ok && sink.write(0, "end") == Result::Ok
		&& sink.preparePublish() == Result::Ok, "child reaches durable publication boundary");
	if (published) {
		Check(sink.publish(true) == Result::Ok, "child commits destination");
	}
	std::_Exit(kAbruptExitCode);
}

void TestPublicationExit(const QTemporaryDir &dir) {
	for (const auto after : { false, true }) {
		auto job = Job(dir, after ? u"after.bin"_s : u"before.bin"_s, 3);
		job.checkpointKey = QByteArray(32, 'q');
		auto child = QProcess();
		child.start(QCoreApplication::applicationFilePath(), { u"--publication-exit"_s,
			job.id.toString(), job.destination, after ? u"after"_s : u"before"_s });
		Check(child.waitForFinished() && child.exitCode() == kAbruptExitCode,
			"publication child terminated without destructors");
		auto recovered = Sink(job);
		const auto identity = Storage::PersistentDownloadAliasIdentity(
			after ? job.destination : recovered.stagingPath());
		Check(!identity.isEmpty(), "recover fixture identity");
		if (!after) {
			Check(recovered.inspectPublished(identity) == Result::Missing
				&& recovered.open(true, identity) == Result::Ok && recovered.committedSize() == 3
				&& recovered.publish(true) == Result::Ok, "finish interrupted rename");
		}
		auto final = Sink(job);
		Check(final.inspectPublished(identity) == Result::Ok
			&& final.purge(identity) == Result::Ok && Read(job.destination) == "end",
			"reconcile published file across process exit");
	}
}

void TestInvalidCheckpoint(const QTemporaryDir &dir) {
	const auto job = Job(dir, u"invalid-checkpoint.bin"_s, 1);
	auto sink = Sink(job);
	Check(sink.open(false) == Result::Ok && sink.write(0, "k") == Result::Ok
		&& sink.suspend() == Result::Ok, "prepare invalid checkpoint fixture");
	Put(sink.checkpointPath(), "incomplete checkpoint");
	auto reopened = Sink(job);
	Check(reopened.open(true) == Result::InvalidCheckpoint, "malformed checkpoint refused");
	Check(Read(sink.stagingPath()) == "k", "refused checkpoint leaves bytes intact");
	Check(sink.open(true) == Result::InvalidCheckpoint, "malformed checkpoint remains visible");
}

void TestFinalReadback(const QTemporaryDir &dir) {
	const auto job = Job(dir, u"readback.bin"_s, kPart + 3);
	auto fixture = std::make_unique<FaultFile>();
	const auto device = fixture.get();
	auto sink = Sink(job, std::move(fixture));
	Check(sink.open(false) == Result::Ok, "open verification fixture");
	const auto good = QByteArray(kPart, 'a');
	Check(sink.write(0, good) == Result::Ok, "valid prefix");
	device->fault = FaultFile::Fault::WrongBytes;
	Check(sink.write(kPart, "end") == Result::Ok, "storage reported full write");
	Check(sink.publish() == Result::Incomplete && !QFileInfo::exists(job.destination),
		"successful write with wrong bytes cannot publish");
	Check(sink.suspend() == Result::Ok, "suspend faulty writer");
	const auto stop = std::atomic<bool>(true);
	auto recovered = Sink(job);
	Check(recovered.open(true, sink.identity(), false, false, &stop) == Result::TransferFailed,
		"retirement interrupts verification without touching durable state");
	Check(recovered.open(true, sink.identity()) == Result::Ok
		&& recovered.committedSize() == kPart && !recovered.committed().testBit(1),
		"retry retains only the correct chunk");
	Check(recovered.write(kPart, "end") == Result::Ok
		&& recovered.publish(true, &stop) != Result::Ok && !QFileInfo::exists(job.destination),
		"stopped publication preserves staging");
	Check(recovered.publish() == Result::Ok && Sha256(Read(job.destination)) == Sha256(good + "end"),
		"readback recovery matches original content");
}

void TestAuthenticatedCheckpoint(const QTemporaryDir &dir) {
	auto job = Job(dir, u"authenticated.bin"_s, kPart + 3);
	job.checkpointKey = QByteArray(32, 'a');
	auto sink = Sink(job);
	Check(sink.open(false) == Result::Ok && sink.write(0, QByteArray(kPart, 'h')) == Result::Ok
		&& sink.suspend() == Result::Ok, "write authenticated checkpoint");
	const auto record = Read(sink.checkpointPath());
	Check(!record.contains(job.checkpointKey) && record.at(7) == char(2),
		"v2 sidecar carries no authentication key");
	auto wrong = job;
	wrong.checkpointKey = QByteArray(32, 'b');
	Check(Sink(wrong).open(true, sink.identity()) == Result::InvalidCheckpoint,
		"another registry key cannot resume this job");
	wrong.checkpointKey.clear();
	Check(Sink(wrong).open(true, sink.identity()) == Result::InvalidCheckpoint,
		"legacy policy cannot read authenticated records");
	auto metadata = Sink(job);
	Check(metadata.inspectForResume(sink.identity()) == Result::Ok && metadata.committedSize() == 0,
		"startup metadata inspection grants no verified chunks");
	auto body = record.chopped(32);
	body[7] = char(1);
	Put(sink.checkpointPath(), body + Sha256(body));
	Check(Sink(job).open(true, sink.identity()) == Result::InvalidCheckpoint,
		"registry-owned policy rejects downgrade to a public checksum");
	Put(sink.checkpointPath(), record);
	auto valid = Sink(job);
	Check(valid.open(true, sink.identity()) == Result::Ok && valid.committedSize() == kPart,
		"original authenticated record restores the verified prefix");
	Check(valid.write(kPart, "end") == Result::Ok && valid.publish() == Result::Ok,
		"authenticated publication succeeds");
}

void TestOwnership(const QTemporaryDir &dir) {
	const auto job = Job(dir, u"ownership.bin"_s, 2);
	auto sink = Sink(job);
	Check(sink.open(false) == Result::Ok, "create owned fixture");
	auto other = Sink(job);
	Check(other.open(true, sink.identity()) != Result::Ok, "one staging writer at a time");
#ifdef Q_OS_WIN
	auto writer = QFile(sink.stagingPath());
	Check(!writer.open(QIODevice::ReadWrite | QIODevice::ExistingOnly),
		"active Windows handle excludes another writer");
#endif
	Check(sink.write(0, "ok") == Result::Ok, "write verified data");
	Check(QFile::rename(sink.checkpointPath(), dir.filePath(u"saved-checkpoint"_s)),
		"move checkpoint before replacement");
	Put(sink.checkpointPath(), "foreign checkpoint");
	Check(sink.checkpoint() == Result::CheckpointFailed && sink.committedSize() == 0,
		"ambiguous checkpoint cannot be overwritten");
	Check(sink.cancel() == Result::RemoveFailed && Read(sink.checkpointPath()) == "foreign checkpoint",
		"terminal cleanup leaves foreign sidecar intact");

	const auto linkJob = Job(dir, u"linked.bin"_s, 1);
	auto linked = Sink(linkJob);
	Check(linked.open(false) == Result::Ok && linked.write(0, "x") == Result::Ok
		&& linked.suspend() == Result::Ok, "create paused link fixture");
	const auto alias = dir.filePath(u"other-name.bin"_s);
#ifdef Q_OS_WIN
	Check(CreateHardLinkW(QDir::toNativeSeparators(alias).toStdWString().c_str(),
		QDir::toNativeSeparators(linked.stagingPath()).toStdWString().c_str(), nullptr),
		"create NTFS hard-link fixture");
#else
	Check(::link(QFile::encodeName(linked.stagingPath()).constData(),
		QFile::encodeName(alias).constData()) == 0, "create hard-link fixture");
#endif
	Check(Storage::PersistentDownloadAliasIdentity(alias) == linked.identity(),
		"destination guard recognizes an alternate name");
	Check(linked.open(true, linked.identity()) == Result::Replaced
		&& linked.purge(linked.identity()) == Result::Replaced && Read(alias) == "x",
		"refuse multiply linked stage for recovery and deletion");
	Check(QFile::remove(alias) && linked.purge(linked.identity()) == Result::Ok,
		"cleanup succeeds once the alias is removed");
}

void TestLinkedDirectory(const QTemporaryDir &dir) {
	const auto target = dir.filePath(u"link-target"_s);
	const auto link = dir.filePath(u"linked-directory"_s);
	Check(QDir().mkdir(target), "create link target directory");
#ifdef Q_OS_WIN
	const auto directoryLink = QDir::toNativeSeparators(link).toStdWString();
	const auto actualDirectory = QDir::toNativeSeparators(target).toStdWString();
	if (!CreateSymbolicLinkW(directoryLink.c_str(), actualDirectory.c_str(),
			SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
		const auto error = GetLastError();
		if (error == ERROR_PRIVILEGE_NOT_HELD || error == ERROR_INVALID_PARAMETER) {
			qWarning() << "Skipping directory symlink fixture, Windows error" << error;
			return;
		}
		Check(false, "create directory symlink on test volume");
	}
#else
	Check(::symlink(QFile::encodeName(target).constData(), QFile::encodeName(link).constData()) == 0,
		"create directory symlink on test volume");
#endif
	const auto job = Job(dir, u"linked-directory/denied.bin"_s, 1);
	auto sink = Sink(job);
	Check(!Storage::SafePersistentDownloadPath(job.destination) && sink.open(false) == Result::Replaced,
		"linked ancestor prevents stage creation");
	Check(QDir(target).isEmpty(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot),
		"linked target remains untouched");
#ifdef Q_OS_WIN
	Check(RemoveDirectoryW(directoryLink.c_str()), "remove fixture link");
#else
	Check(::unlink(QFile::encodeName(link).constData()) == 0, "remove fixture link");
#endif
}

void TestShuffledTransfer(const QTemporaryDir &dir) {
	constexpr auto count = 73;
	const auto job = Job(dir, u"shuffled.bin"_s, (count - 1) * qint64(kPart) + 11);
	auto order = std::array<int, count>();
	std::iota(order.begin(), order.end(), 0);
	auto generator = std::mt19937(373);
	std::shuffle(order.begin(), order.end(), generator);
	auto sink = Sink(job);
	Check(sink.open(false) == Result::Ok, "shuffled stage");
	auto expected = QCryptographicHash(QCryptographicHash::Sha256);
	for (auto i = 0; i < count; ++i) {
		expected.addData(QByteArray(i + 1 == count ? 11 : kPart, char(i)));
	}
	for (const auto i : order) {
		const auto part = QByteArray(i + 1 == count ? 11 : kPart, char(i));
		Check(sink.write(qint64(i) * kPart, part) == Result::Ok
			&& sink.write(qint64(i) * kPart, part) == Result::Ok, "out-of-order duplicate");
	}
	Check(sink.publish() == Result::Ok && Sha256(Read(job.destination)) == expected.result(),
		"shuffled output matches sequential SHA-256");
}

void TestLargeOffset(const QTemporaryDir &dir) {
	const auto offset = qint64(1) << 31;
	const auto job = Job(dir, u"large.bin"_s, offset + 7);
	auto fixture = std::make_unique<FaultFile>();
	const auto device = fixture.get();
	auto sink = Sink(job, std::move(fixture));
	Check(sink.open(false) == Result::Ok, "create 64-bit fixture");
	if (!device->makeSparse()) {
		qFatal("Sparse file support is required for the 64-bit offset test");
	}
	Check(sink.write(offset, "lastend") == Result::Ok, "write beyond signed boundary");
	Check(sink.suspend() == Result::Ok && sink.open(true) == Result::Ok,
		"durable 64-bit offset");
	Check(sink.writtenSize() == 7 && sink.committed().testBit(offset / kPart)
		&& sink.committed().count(true) == 1, "no false coverage across large hole");
	Check(sink.cancel() == Result::Ok, "delete large staging");
}

void TestRequestGeneration() {
	auto requests = Storage::PersistentDownloadRequests();
	auto committed = QBitArray(3);
	requests.reset(committed);
	const auto old = requests.generation();
	Check(requests.take() == 0 && requests.take() == kPart
		&& requests.outstanding() == 2, "two in-flight writes");
	Check(requests.written(old), "complete first write");
	committed.setBit(0);
	requests.invalidate();
	Check(!requests.written(old), "ignore suspended acknowledgement");
	requests.reset(committed);
	const auto current = requests.generation();
	Check(requests.take() == kPart && requests.outstanding() == 1,
		"resume dropped in-flight chunk");
	Check(!requests.written(old) && requests.outstanding() == 1,
		"late write cannot consume resumed request");
	Check(requests.written(current) && requests.take() == 2 * kPart,
		"continue after current acknowledgement");
	requests.invalidate();
	Check(!requests.written(current) && !requests.outstanding(),
		"ignore late write after cancellation");
}

void TestBatching(const QTemporaryDir &dir) {
	const auto job = Job(dir, u"batch.bin"_s, 65 * qint64(kPart));
	auto sink = Sink(job);
	const auto data = QByteArray(kPart, 'z');
	Check(sink.open(false) == Result::Ok, "create batch fixture");
	for (auto index = 0; index != 63; ++index) {
		Check(sink.write(qint64(index) * kPart, data) == Result::Ok, "fill below threshold");
	}
	Check(!sink.committed().count(true), "under threshold stays uncommitted");
	Check(sink.write(63 * qint64(kPart), data) == Result::Ok, "cross threshold");
	Check(sink.committed().count(true) == 64, "threshold checkpoint saved 8 MiB");
	Check(sink.cancel() == Result::Ok, "clean batch fixture");
}

void TestRegistry(const QTemporaryDir &dir) {
	using State = Storage::PendingDownloadState;
	const auto owner = quint64(100123);
	const auto testOwner = owner | 0x0100'0000'0000'0000ULL;
	const auto job = Storage::PendingDownload{
		.file = Job(dir, u"registry.bin"_s, (qint64(1) << 31) + 7),
		.stagingIdentity = QByteArray(24, 'i'),
		.peer = 200,
		.message = 42,
		.channelAccess = 77,
		.document = 300,
		.displayName = u"test document"_s,
		.started = 1000,
		.updated = 2000,
		.state = State::Paused,
	};
	auto keyed = job;
	keyed.file.id = QUuid::createUuid();
	keyed.file.checkpointKey = QByteArray(32, 'k');
	const auto mixed = Storage::SerializePendingDownloads(owner, { job, keyed });
	const auto loaded = Storage::ParsePendingDownloads(owner, mixed);
	Check(loaded && loaded->size() == 2 && loaded->front().file.checkpointKey.isEmpty()
		&& loaded->back().file.checkpointKey == keyed.file.checkpointKey
		&& Storage::SerializePendingDownloads(owner, *loaded) == mixed,
		"legacy and authenticated jobs share an encrypted registry payload");
	Check(!Storage::ParsePendingDownloads(owner + 1, mixed)
		&& !Storage::ParsePendingDownloads(owner, mixed.chopped(1)), "keyed registry bounds and account");
	const auto data = Storage::SerializePendingDownloads(owner, { job });
	for (auto i = qsizetype(0); i < data.size(); ++i) {
		Check(!Storage::ParsePendingDownloads(owner, data.first(i)), "reject every incomplete registry");
	}
	auto duplicate = data;
	duplicate[19] = char(2);
	duplicate.append(data.sliced(20));
	Check(!Storage::ParsePendingDownloads(owner, duplicate), "reject duplicate identity on input");
	const auto read = Storage::ParsePendingDownloads(owner, data);
	Check(read && read->size() == 1, "load one registry entry");
	Check(Storage::SerializePendingDownloads(owner, *read) == data,
		"registry complete round trip");
	Check(!Storage::ParsePendingDownloads(owner + 1, data), "reject different account");
	Check(!Storage::ParsePendingDownloads(testOwner, data), "reject different network");
	Check(!Storage::ParsePendingDownloads(owner, data.chopped(1)), "reject truncated registry");
	Check(!Storage::ParsePendingDownloads(owner, data + 'x'), "reject trailing data");
	auto unsupported = data;
	unsupported[7] = char(99);
	Check(!Storage::ParsePendingDownloads(owner, unsupported), "reject unsupported registry version");
	auto corrupt = data;
	corrupt[0] = char(0);
	Check(!Storage::ParsePendingDownloads(owner, corrupt), "reject corrupted registry header");
	auto oversized = data;
	auto stream = QDataStream(&oversized, QIODevice::ReadWrite);
	Check(stream.device()->seek(36), "seek to version 1 destination length");
	stream << quint32(0x40000000);
	Check(!Storage::ParsePendingDownloads(owner, oversized), "reject oversized field before allocation");
	Check(Storage::SerializePendingDownloads(owner, { job, job }).isEmpty(), "reject duplicate job ID");
	Check(!Storage::PendingDownloadTransition(State::Initializing, State::Finalizing)
		&& Storage::PendingDownloadTransition(State::Initializing, State::Downloading)
		&& Storage::PendingDownloadTransition(State::Downloading, State::Paused)
		&& Storage::PendingDownloadTransition(State::Downloading, State::Interrupted)
		&& Storage::PendingDownloadTransition(State::Paused, State::Downloading)
		&& Storage::PendingDownloadTransition(State::Interrupted, State::Retryable)
		&& Storage::PendingDownloadTransition(State::Retryable, State::Unavailable)
		&& !Storage::PendingDownloadTransition(State::Unavailable, State::Downloading)
		&& Storage::PendingDownloadTransition(State::Unavailable, State::Cancelling)
		&& !Storage::PendingDownloadTransition(State::Cancelling, State::Downloading),
		"allow recovery edges and reject terminal resurrection");

	const auto path = dir.filePath(u"registry/atomic.bin"_s);
	auto lock = Storage::LockPendingDownloadRegistry(path);
	Check(bool(lock) && !Storage::LockPendingDownloadRegistry(path), "one registry owner");
	lock.reset();
	Check(bool(Storage::LockPendingDownloadRegistry(path)), "registry reusable after owner exits");
	Check(!Storage::ValidPersistentDownloadPath(u"relative.bin"_s)
		&& !Storage::ValidPersistentDownloadPath(dir.path() + u"/../other.bin"_s),
		"reject relative and unnormalized paths");
#ifdef Q_OS_WIN
	for (const auto path : { u"C:/sample.bin:stream", u"C:/COM1.txt", u"C:/name. ",
			u"//?/C:/sample.bin", u"//./device", u"C:/NUL/archive.bin" }) {
		Check(!Storage::ValidPersistentDownloadPath(QString::fromUtf16(path)), "reject ambiguous name");
	}
#endif
	const auto oldPayload = QByteArray("first opaque test payload");
	const auto newPayload = QByteArray("new opaque test payload");
	Check(Storage::ReplacePendingDownloadRegistry(path, oldPayload), "create checked registry");
	Check(!Storage::ReplacePendingDownloadRegistry(path, QByteArray()), "reject invalid replacement");
#ifdef Q_OS_WIN
	const auto name = QDir::toNativeSeparators(path).toStdWString();
	const auto locked = CreateFileW(name.c_str(), GENERIC_READ, FILE_SHARE_READ,
		nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	Check(locked != INVALID_HANDLE_VALUE, "lock previous registry");
	Check(!Storage::ReplacePendingDownloadRegistry(path, newPayload), "atomic replacement fails");
	Check(CloseHandle(locked), "unlock previous registry");
#endif
	Check(Read(path) == oldPayload, "previous registry still valid");
	Check(Storage::ReplacePendingDownloadRegistry(path, newPayload)
		&& Read(path) == newPayload, "replacement succeeds after retry");
}

void TestCreationRecovery(const QTemporaryDir &dir) {
	const auto job = Job(dir, u"reserved.bin"_s, 2 * kPart);
	auto identity = QByteArray();
	{
		auto staged = Sink(job);
		Check(!QFileInfo::exists(staged.stagingPath())
			&& !QFileInfo::exists(staged.checkpointPath()), "registry reservation has no artifact");
		Check(staged.open(false, {}, true) == Result::Ok, "create before staging identity commit");
		identity = staged.identity();
		Check(!identity.isEmpty() && !QFileInfo::exists(staged.checkpointPath()),
			"initial checkpoint deferred");
	}
	{
		auto unknown = Sink(job);
		Check(unknown.open(false, {}, true) != Result::Ok
			&& unknown.purge({}) == Result::Replaced,
			"unregistered staging identity cannot be adopted or removed");
		Check(QFileInfo::exists(unknown.stagingPath()), "unknown stage is preserved");
	}
	{
		auto known = Sink(job);
		Check(known.open(true, identity, false, true) == Result::Ok
			&& known.committed().count(true) == 0, "registered empty stage can initialize checkpoint");
		Check(known.suspend() == Result::Ok, "preserve initialized stage");
	}
	{
		auto recovered = Sink(job);
		Check(recovered.open(true, identity) == Result::Ok
			&& recovered.committed().count(true) == 0, "reopen first checkpoint after restart");
		Check(recovered.purge(identity) == Result::Ok, "finish interrupted cancellation");
	}
	auto missing = Sink(job);
	Check(missing.open(true, identity) == Result::Missing
		&& !QFileInfo::exists(missing.stagingPath()), "never recreate known missing staging");
	Check(missing.purge(identity) == Result::Ok, "cancellation cleanup is idempotent");
}

void TestPublicationRecovery(const QTemporaryDir &dir) {
	const auto job = Job(dir, u"recover-publish.bin"_s, kPart + 3);
	const auto head = QByteArray(kPart, 'p');
	const auto body = head + "end";
	auto identity = QByteArray();
	{
		auto stage = Sink(job);
		Check(stage.open(false) == Result::Ok
			&& stage.write(0, head) == Result::Ok
			&& stage.write(kPart, "end") == Result::Ok
			&& stage.preparePublish() == Result::Ok, "crash boundary before rename");
		identity = stage.identity();
	}
	{
		auto recovered = Sink(job);
		Check(recovered.inspectPublished(identity) == Result::Missing,
			"never report completion before rename");
		Check(recovered.open(true, identity) == Result::Ok
			&& recovered.committed().count(true) == 2, "all offsets already committed");
		auto requests = Storage::PersistentDownloadRequests();
		requests.reset(recovered.committed());
		Check(requests.empty(), "publication retry needs no network bytes");
		Check(recovered.publish(true) == Result::Ok
			&& QFileInfo::exists(recovered.checkpointPath()), "retain proof after rename");
	}
	{
		auto after = Sink(job);
		Check(after.inspectPublished(identity) == Result::Ok, "crash boundary after rename");
		Check(Sha256(Read(job.destination)) == Sha256(body), "recovered final SHA-256");
		Check(after.purge(identity) == Result::Ok && !QFileInfo::exists(after.checkpointPath()),
			"durable history permits sidecar removal");
		Check(after.matchesPublished(identity) && QFileInfo::exists(job.destination),
			"idempotent cleanup retains completed file");
	}
	Check(QFile::rename(job.destination, dir.filePath(u"keep-complete.bin"_s)),
		"move verified completed fixture");
	Put(job.destination, body);
	auto unrelated = Sink(job);
	Check(unrelated.inspectPublished(identity) == Result::Replaced
		&& !unrelated.matchesPublished(identity), "same bytes with different identity are unrelated");
}

void TestStreamingAttachment() {
	using namespace Media::Streaming;
	auto file = DownloadAttachment();
	auto remote = PartRequestOwners();
	auto order = SharedRequestOrder();
	using Consumer = PartRequestOwners::Consumer;
	using Offset = PartRequestOwners::Offset;
	for (auto i = 0; i != 4; ++i) {
		Check(!order.takeDownload(true, true), "playback has the first four shared scheduling turns");
	}
	Check(order.takeDownload(true, true) && order.takeDownload(false, true)
		&& !order.takeDownload(true, false), "download is serviced during continued playback");
	const auto start = file.attach();
	const auto seek = quint32(1) << 31;
	const auto last = seek + 3 * kPart;
	remote.add(Consumer::Playback, seek);
	remote.add(Consumer::Playback, seek + kPart);
	Check(file.request(start, last) && file.request(start, seek),
		"first attachment accepts nonzero, noncontiguous and descending offsets");
	Check(!file.request(start, last), "duplicate attachment request is ignored");
	remote.add(Consumer::Download, last);
	remote.add(Consumer::Download, seek);
	Check(file.detach(start), "pause detaches its generation");
	Check(remote.detach(Consumer::Download) == std::set<Offset>{ last },
		"pause cancels only the download-only network offset");
	Check(remote.contains(Consumer::Playback, seek)
		&& remote.contains(Consumer::Playback, seek + kPart),
		"playback and seek requests survive pausing the download");

	const auto resumed = file.attach();
	Check(resumed != start && file.request(resumed, seek) && file.request(resumed, last),
		"reattach starts a new generation");
	Check(!file.consume(start, seek) && !file.detach(start) && !file.request(start, last),
		"queued delivery, detach and request from past generation are rejected");
	remote.add(Consumer::Download, seek);
	remote.add(Consumer::Download, last);
	Check(remote.detach(Consumer::Playback) == std::set<Offset>{ seek + kPart },
		"stopping playback leaves shared and download-only offsets scheduled");
	Check(remote.contains(Consumer::Download, seek), "shared request survives closing playback");
	Check(file.consume(resumed, last) && !file.consume(resumed, last),
		"one accepted delivery per offset per attachment");
	Check(remote.release(Consumer::Download, last)
		&& !remote.release(Consumer::Download, last), "transport accounting released once");
	Check(file.detach(resumed) && !file.consume(resumed, seek),
		"late delivery cannot apply after terminal cancellation");
	Check(remote.detach(Consumer::Download) == std::set<Offset>{ seek },
		"the last remaining download offset is released");
}

void TestStreamedCheckpoint(const QTemporaryDir &dir) {
	using namespace Media::Streaming;
	const auto fixture = Job(dir, u"streamed-chunks.bin"_s, 2 * kPart + 7);
	const auto first = QByteArray(kPart, 'a');
	const auto second = QByteArray(kPart, 'b');
	const auto tail = QByteArray(7, 'c');
	auto attachment = DownloadAttachment();
	const auto initial = attachment.attach();
	Check(attachment.request(initial, 0)
		&& attachment.request(initial, kPart)
		&& attachment.request(initial, 2 * kPart), "request initial streaming batch");
	auto identity = QByteArray();
	{
		auto sink = Sink(fixture);
		Check(sink.open(false) == Result::Ok, "create streaming staging file");
		identity = sink.identity();
		Check(IsCompletePart(fixture.size, 2 * kPart, tail.size(), kPart)
			&& attachment.consume(initial, 2 * kPart)
			&& sink.write(2 * kPart, tail) == Result::Ok, "accept cached short tail out of order");
		Check(!IsCompletePart(fixture.size, kPart, 7, kPart)
			&& attachment.pending(initial, kPart), "invalid cached chunk cannot satisfy a request");
		Check(!attachment.consume(initial, 2 * kPart)
			&& attachment.consume(initial, 0)
			&& sink.write(0, first) == Result::Ok, "ignore duplicate before writing");
		Check(sink.committed().count(true) == 0 && sink.suspend() == Result::Ok,
			"written streaming bytes become durable after checkpoint");
		Check(sink.committedSize() == kPart + tail.size(), "durable bytes include short tail");
	}
	Check(attachment.detach(initial), "suspend streaming batch");
	auto sink = Sink(fixture);
	Check(sink.open(true, identity) == Result::Ok, "reopen streaming checkpoint");
	Check(sink.committedSize() == kPart + tail.size(), "reopen reports verified durable bytes");
	auto offsets = Storage::PersistentDownloadRequests();
	offsets.reset(sink.committed());
	const auto resumed = attachment.attach();
	const auto missing = offsets.take();
	Check(missing == kPart && offsets.empty()
		&& attachment.request(resumed, missing), "resume only the missing middle offset");
	Check(!attachment.consume(initial, missing) && attachment.consume(resumed, missing)
		&& sink.write(missing, second) == Result::Ok, "write only the current attachment delivery");
	Check(sink.publish() == Result::Ok
		&& Sha256(Read(fixture.destination)) == Sha256(first + second + tail),
		"persistent streaming fixture publishes the expected SHA-256");
}

void TestStreamingMaximumOffsets() {
	using namespace Media::Streaming;
	const auto limit = qint64(4000) * 1024 * 1024;
	const auto boundary = qint64(1) << 31;
	const auto size = limit - 11;
	const auto last = limit - kPart;
	auto covered = QBitArray(limit / kPart, true);
	for (const auto offset : { boundary - kPart, boundary, boundary + kPart, last }) {
		covered.clearBit(offset / kPart);
	}
	auto requests = Storage::PersistentDownloadRequests();
	requests.reset(std::move(covered));
	for (const auto offset : { boundary - kPart, boundary, boundary + kPart, last }) {
		Check(requests.take() == offset, "preserve 64-bit signed-boundary and maximum offsets");
		Check(IsCompletePart(size, offset, (offset == last) ? kPart - 11 : kPart, kPart),
			"accept exact actual size at large offset");
	}
	Check(requests.empty() && !IsCompletePart(size, last, kPart, kPart)
		&& !IsCompletePart(size, limit, 0, kPart), "reject oversized final part and past end");
}

void TestDownloadActions() {
	using Status = Storage::DownloadStatus;
	using Action = Storage::DownloadAction;
	const auto allows = [](Status status, Action action) {
		return Storage::AllowsDownloadAction(true, status, action);
	};
	Check(allows(Status::Downloading, Action::Pause)
		&& allows(Status::Resolving, Action::Pause)
		&& allows(Status::Resuming, Action::Pause), "pause active and queued downloads");
	Check(allows(Status::Paused, Action::Resume)
		&& allows(Status::Interrupted, Action::Resume)
		&& allows(Status::Waiting, Action::Resume)
		&& allows(Status::Failed, Action::Resume), "resume and retry eligible states");
	Check(!allows(Status::Pausing, Action::Resume)
		&& !allows(Status::Verifying, Action::Resume)
		&& !allows(Status::Unavailable, Action::Resume)
		&& !allows(Status::Finalizing, Action::Cancel), "transitional action restrictions");
	for (const auto action : { Action::Pause, Action::Resume, Action::Cancel }) {
		Check(!allows(Status::Completed, action) && !allows(Status::Cancelling, action)
			&& !Storage::AllowsDownloadAction(false, Status::Downloading, action),
			"completed, cancelled and unsupported rows are ineligible");
	}
	Check(Storage::PendingDownloadTransition(
		Storage::PendingDownloadState::Interrupted,
		Storage::PendingDownloadState::Paused), "queued interruption can become manual pause");

	const auto first = Storage::PendingDownloadId{ 42, QUuid::createUuid() };
	const auto second = Storage::PendingDownloadId{ 42, QUuid::createUuid() };
	const auto other = Storage::PendingDownloadId{ 43, first.job };
	const auto snapshot = Storage::ScopedDownloadIds(42, { first, other, second, first, {} });
	Check(snapshot.size() == 2 && snapshot.front() == first && snapshot.back() == second,
		"stable selected identities are deduplicated within the requested account");
	Check(Storage::ScopedDownloadIds(43, snapshot).empty()
		&& Storage::ScopedDownloadIds(42, {}).empty(), "changed account and empty selection");
	auto states = std::array{ Status::Downloading, Status::Paused };
	const auto count = [&](Action action) {
		return std::count_if(states.begin(), states.end(), [&](Status state) {
			return allows(state, action);
		});
	};
	Check(count(Action::Pause) == 1 && count(Action::Resume) == 1
		&& count(Action::Cancel) == 2, "mixed selection supports only eligible actions");
	states[0] = Status::Completed;
	states[1] = Status::Cancelling;
	Check(count(Action::Pause) == 0 && count(Action::Resume) == 0
		&& count(Action::Cancel) == 0, "revalidating before dispatch skips completed and cancelled jobs");
}

void TestDownloadPresentation() {
	using Status = Storage::DownloadStatus;
	using Filter = Storage::DownloadFilter;
	auto matches = [](Status status, Filter filter, QStringView name, QStringView query) {
		return Storage::MatchesDownloadFilter(status, filter)
			&& Storage::MatchesDownloadName(name, query);
	};
	const auto filename = u"RÉSUMÉ α (Final).MP4"_s;
	Check(matches(Status::Paused, Filter::Paused, filename, u"résumé α"_s)
		&& matches(Status::Paused, Filter::All, filename, u").mp4"_s)
		&& !matches(Status::Paused, Filter::Completed, filename, u"mp4"_s)
		&& !matches(Status::Downloading, Filter::Active, filename, u".zip"_s),
		"case-insensitive Unicode and punctuation match combined with status");
	Check(Storage::MatchesDownloadFilter(Status::Verifying, Filter::Active)
		&& Storage::MatchesDownloadFilter(Status::Waiting, Filter::Interrupted)
		&& Storage::MatchesDownloadFilter(Status::Unavailable, Filter::Failed),
		"transitional filter buckets");
	auto found = 0;
	for (auto i = 0; i != 4096; ++i) {
		found += matches((i % 2) ? Status::Completed : Status::Interrupted,
			Filter::Completed, filename, u".MP4"_s);
	}
	Check(found == 2048, "large mixed history filter yields the expected count");

	auto total = Storage::DownloadByteTotals();
	const auto gib = qint64(1) << 30;
	total.add(3 * gib, 4 * gib);
	total.add(0, gib);
	total.add(gib, 0);
	Check(total.written == 3 * gib && total.total == 5 * gib
		&& total.percent() == 60, "aggregate uses byte weights and skips unknown sizes");
	auto rate = Storage::DownloadRate();
	rate.accept(kPart, 1000);
	Check(rate.rate(1200) == 0, "speed waits for a sample window");
	rate.accept(kPart, 1500);
	const auto measured = rate.rate(1500);
	Check(measured == 2 * kPart
		&& Storage::DownloadEta(3 * gib, 4 * gib, measured) == (gib / measured),
		"speed and ETA use monotonic samples and 64-bit remaining bytes");
	Check(rate.rate(3500) == 0 && !Storage::DownloadEta(0, 4 * gib, 0),
		"stale or missing estimates are hidden");
	rate.accept(kPart, 4000);
	Check(rate.rate(4000) == 0, "transfer after idle starts a fresh sample");
}

void TestDownloadPreferences() {
	using Preference = Storage::DownloadPreference;
	auto prefs = QMap<QByteArray, QByteArray>();
	const auto key = [](Preference preference) {
		const auto name = Storage::DownloadPreferenceKey(preference);
		return QByteArray(name.data(), name.size());
	};
	const auto read = [&] {
		return Storage::ReadDownloadPreferences([&](std::string_view name) {
			return prefs.value(QByteArray(name.data(), name.size()));
		});
	};
	const auto defaults = read();
	Check(!defaults.enabled && !defaults.startupResume
		&& defaults.confirmCancellation && defaults.pendingIndicator,
		"pre-Milestone 5 profile receives explicit defaults");

	prefs.insert("unrelated.preference", "unchanged");
	prefs.insert(key(Preference::Enabled), Storage::DownloadBooleanValue(true));
	prefs.insert(key(Preference::StartupResume), Storage::DownloadBooleanValue(true));
	prefs.insert(key(Preference::ConfirmCancellation), Storage::DownloadBooleanValue(false));
	prefs.insert(key(Preference::PendingIndicator), Storage::DownloadBooleanValue(false));

	auto serialized = QByteArray();
	{
		auto stream = QDataStream(&serialized, QIODevice::WriteOnly);
		stream << quint32(prefs.size());
		for (auto i = prefs.cbegin(); i != prefs.cend(); ++i) {
			stream << i.key() << i.value();
		}
		Check(stream.status() == QDataStream::Ok, "write preference map fixture");
	}
	prefs.clear();
	{
		auto stream = QDataStream(serialized);
		auto count = quint32();
		stream >> count;
		for (auto i = quint32(); i != count; ++i) {
			auto savedKey = QByteArray();
			auto value = QByteArray();
			stream >> savedKey >> value;
			prefs.insert(savedKey, value);
		}
		Check(stream.status() == QDataStream::Ok && stream.atEnd(), "read preference map fixture");
	}
	const auto saved = read();
	Check(saved.enabled && saved.startupResume
		&& !saved.confirmCancellation && !saved.pendingIndicator
		&& prefs.value("unrelated.preference") == "unchanged",
		"settings round-trip preserves unrelated preferences");

	prefs.insert(key(Preference::Enabled), "true");
	prefs.insert(key(Preference::ConfirmCancellation), "2");
	const auto invalid = read();
	Check(!invalid.enabled && invalid.startupResume && invalid.confirmCancellation,
		"malformed entries affect only their own defaults");
	for (const auto preference : { Preference::Enabled, Preference::StartupResume,
			Preference::ConfirmCancellation, Preference::PendingIndicator }) {
		prefs.remove(key(preference));
	}
	const auto reset = read();
	Check(!reset.enabled && !reset.startupResume && reset.confirmCancellation
		&& reset.pendingIndicator && prefs.value("unrelated.preference") == "unchanged",
		"removing settings restores only download defaults");
}

void TestDownloadOverridesAndPolicy() {
	using Override = Storage::DownloadOverride;
	using State = Storage::PendingDownloadState;
	const auto parse = Storage::ParseDownloadOverride;
	Check(parse(false, "1") == Override::Absent
		&& parse(true, "0") == Override::Disabled
		&& parse(true, "1") == Override::Enabled
		&& parse(true, "") == Override::Invalid
		&& parse(true, " 1") == Override::Invalid
		&& parse(true, "true") == Override::Invalid,
		"overrides distinguish missing, explicit off, explicit on, and invalid");
	const auto persisted = Storage::DownloadPreferences{ .enabled = true, .startupResume = true };
	const auto disabled = Storage::ResolveDownloadActivation(persisted, {
		.enabled = Override::Disabled,
		.startupResume = Override::Enabled,
	});
	const auto invalid = Storage::ResolveDownloadActivation(persisted, {
		.enabled = Override::Invalid,
		.startupResume = Override::Invalid,
	});
	const auto cleared = Storage::ResolveDownloadActivation(persisted, {});
	const auto manual = Storage::ResolveDownloadActivation({}, { .enabled = Override::Enabled });
	const auto startupOff = Storage::ResolveDownloadActivation(persisted, {
		.startupResume = Override::Disabled,
	});
	Check(!disabled.enabled && !disabled.startupResume
		&& invalid.enabled && invalid.startupResume && cleared.enabled && cleared.startupResume
		&& manual.enabled && !manual.startupResume && startupOff.enabled && !startupOff.startupResume,
		"valid overrides take precedence without rewriting preferences");
	Check(!Storage::ResolveDownloadActivation({ .startupResume = true }, {}).startupResume,
		"the saved startup choice can exist while the effective feature is off");
	Check(!Storage::DownloadOverrideValue(Override::Absent).value_or(false)
		&& !Storage::DownloadOverrideValue(Override::Invalid).value_or(false)
		&& Storage::DownloadOverrideValue(Override::Enabled).value_or(false),
		"tracing is explicit and defaults to off");
	Check(Storage::PendingDownloadMayAutoResume(State::Downloading)
		&& Storage::PendingDownloadMayAutoResume(State::Interrupted)
		&& Storage::PendingDownloadMayAutoResume(State::Finalizing)
		&& !Storage::PendingDownloadMayAutoResume(State::Paused)
		&& !Storage::PendingDownloadMayAutoResume(State::Unavailable)
		&& !Storage::PendingDownloadMayAutoResume(State::Cancelling),
		"restoring jobs preserves manual pause and terminal intent");

	Check(Storage::ConfirmDownloadCancellation(true, false, true)
		&& Storage::ConfirmDownloadCancellation(true, true, false)
		&& !Storage::ConfirmDownloadCancellation(true, false, false)
		&& !Storage::ConfirmDownloadCancellation(false, false, true),
		"individual preference and mandatory bulk confirmation are independent");
	Check(Storage::ShowDownloadIndicatorEntry(false, true, false)
		&& Storage::ShowDownloadIndicatorEntry(true, true, false)
		&& Storage::ShowDownloadIndicatorEntry(true, false, true)
		&& !Storage::ShowDownloadIndicatorEntry(true, false, false),
		"hide idle pending entries without hiding active and ordinary transfers");

	Check(Storage::IsPendingDownloadArtifactName(u".64gram-003d241d-d0e8-43a8-9c35-63b2d944274b.part"_s)
		&& Storage::IsPendingDownloadArtifactName(u".64GRAM-003D241D-D0E8-43A8-9C35-63B2D944274B.PART.RESUME"_s)
		&& !Storage::IsPendingDownloadArtifactName(u"movie.part"_s)
		&& !Storage::IsPendingDownloadArtifactName(u".64gram-example.part"_s),
		"saved download names cannot claim the application-owned staging namespace");
#ifdef Q_OS_WIN
	Check(Storage::IsPendingDownloadArtifactName(u".64gram-003d241d-d0e8-43a8-9c35-63b2d944274b.part. "_s)
		&& Storage::IsPendingDownloadArtifactName(u".64gram-003d241d-d0e8-43a8-9c35-63b2d944274b.part:other"_s),
		"Windows alternate filename spellings cannot claim staging files");
#endif
}

} // namespace

int main(int argc, char **argv) {
	const auto app = QCoreApplication(argc, argv);
	const auto args = QCoreApplication::arguments();
	if (args.size() == 4 && args[1] == u"--write-and-exit"_s) {
		WriteAndExit(args[2], args[3]);
	}
	if (args.size() == 5 && args[1] == u"--publication-exit"_s) {
		ExitAtPublication(QUuid(args[2]), args[3], args[4] == u"after"_s);
	}
	const auto dir = QTemporaryDir(QDir(QFileInfo(QDir::tempPath()).canonicalFilePath())
		.filePath(u"64gram-sink-XXXXXX"_s));
	Check(dir.isValid(), "temporary directory");
	TestRoundTrip(dir);
	TestFaults(dir);
	TestRecovery(dir);
	TestReplaced(dir);
	TestDestination(dir);
	TestDestinationReplaced(dir);
	TestAbruptExit(dir);
	TestInvalidCheckpoint(dir);
	TestLargeOffset(dir);
	TestFinalReadback(dir);
	TestOwnership(dir);
	TestLinkedDirectory(dir);
	TestAuthenticatedCheckpoint(dir);
	TestShuffledTransfer(dir);
	TestBatching(dir);
	TestRequestGeneration();
	TestRegistry(dir);
	TestCreationRecovery(dir);
	TestPublicationRecovery(dir);
	TestPublicationExit(dir);
	TestStreamingAttachment();
	TestStreamedCheckpoint(dir);
	TestStreamingMaximumOffsets();
	TestDownloadActions();
	TestDownloadPresentation();
	TestDownloadPreferences();
	TestDownloadOverridesAndPolicy();
	qInfo("Persistent file sink tests passed.");
	return EXIT_SUCCESS;
}
