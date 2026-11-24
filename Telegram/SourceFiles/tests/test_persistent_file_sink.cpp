/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "storage/persistent_file_sink.h"
#include "storage/pending_downloads.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDataStream>
#include <QtCore/QDebug>
#include <QtCore/QFileInfo>
#include <QtCore/QProcess>
#include <QtCore/QTemporaryDir>

#include <cstdlib>

#ifdef Q_OS_WIN
#include <QtCore/QDir>
#include <QtCore/qt_windows.h>
#include <winioctl.h>
#include <io.h>
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
	};
	Fault fault = Fault::None;

	bool open(OpenMode mode) override {
		if (!QFile::open(mode)) {
			return false;
		}
#ifdef Q_OS_WIN
		const auto native = reinterpret_cast<HANDLE>(_get_osfhandle(handle()));
		auto count = DWORD();
		sparse = DeviceIoControl(
			native, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &count, nullptr);
#endif
		return true;
	}

	bool seek(qint64 offset) override {
		return (fault != Fault::Seek) && QFile::seek(offset);
	}

	bool sparse = true;

protected:
	qint64 writeData(const char *data, qint64 length) override {
		switch (fault) {
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
	Check(replace.publish() == Result::Ok && Read(job.destination) == "x",
		"publish approved replacement");
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

void TestLargeOffset(const QTemporaryDir &dir) {
	const auto offset = qint64(1) << 31;
	const auto job = Job(dir, u"large.bin"_s, offset + 7);
	auto fixture = std::make_unique<FaultFile>();
	const auto device = fixture.get();
	auto sink = Sink(job, std::move(fixture));
	Check(sink.open(false) == Result::Ok, "create 64-bit fixture");
	if (!device->sparse) {
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
	const auto data = Storage::SerializePendingDownloads(owner, { job });
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

} // namespace

int main(int argc, char **argv) {
	const auto app = QCoreApplication(argc, argv);
	const auto args = QCoreApplication::arguments();
	if (args.size() == 4 && args[1] == u"--write-and-exit"_s) {
		WriteAndExit(args[2], args[3]);
	}
	const auto dir = QTemporaryDir();
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
	TestBatching(dir);
	TestRequestGeneration();
	TestRegistry(dir);
	TestCreationRecovery(dir);
	TestPublicationRecovery(dir);
	qInfo("Persistent file sink tests passed.");
	return EXIT_SUCCESS;
}
