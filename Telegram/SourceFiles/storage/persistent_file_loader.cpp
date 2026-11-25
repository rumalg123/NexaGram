/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "storage/persistent_file_loader.h"

#include "core/application.h"
#include "data/data_download_manager.h"
#include "main/main_session.h"
#include "platform/platform_file_utilities.h"

#include <crl/crl_object_on_queue.h>
#include <QtCore/QDebug>
#include <QtCore/QFileInfo>

#include <atomic>

namespace Storage {
namespace {

using Sink = PersistentFileSink;
using PendingState = PendingDownloadState;
constexpr auto kCheckpointInterval = 2 * crl::time(1000);

} // namespace

struct PersistentFileLoader::Persistent {
	enum class State {
		Idle,
		Opening,
		Running,
		Suspending,
		Suspended,
		Failed,
		Publishing,
	};

	explicit Persistent(Storage::PendingDownload record, Fn<void()> checkpoint)
	: job(std::move(record))
	, id(job.file.id)
	, worker(Sink::Job(job.file))
	, timer(std::move(checkpoint)) {
	}

	Storage::PendingDownload job;
	const QUuid id;
	const bool trace = qEnvironmentVariableIntValue(
		"TDESKTOP_PERSISTENT_DOWNLOAD_TRACE") == 1;
	crl::object_on_queue<Sink> worker;
	base::Timer timer;
	std::shared_ptr<std::atomic<bool>> cancelled
		= std::make_shared<std::atomic<bool>>(false);
	State state = State::Idle;
	Storage::PersistentDownloadRequests requests;
	int64 ready = 0;
	int64 committed = 0;
	DownloadRate rate;
	DownloadIssue issue = DownloadIssue::None;
	bool checkpointQueued = false;
};

PersistentFileLoader::PersistentFileLoader(
		not_null<Main::Session*> session,
		const QString &toFile,
		int64 loadSize,
		int64 fullSize,
		LocationType locationType,
		LoadToCacheSetting toCache,
		LoadFromCloudSetting fromCloud,
		bool autoLoading,
		uint8 cacheTag)
: FileLoader(session, toFile, loadSize, fullSize, locationType,
	toCache, fromCloud, autoLoading, cacheTag) {
}

PersistentFileLoader::~PersistentFileLoader() = default;

bool PersistentFileLoader::startPersistent() {
	if (!_persistent) {
		return false;
	}
	if (_persistent->state == Persistent::State::Idle) {
		openPersistent(!_persistent->job.stagingIdentity.isEmpty());
	}
	return true;
}

bool PersistentFileLoader::persistentCanRequest(int limit) const {
	return persistentCanSuspend()
		&& !_persistent->requests.empty()
		&& _persistent->requests.outstanding() < limit;
}

int64 PersistentFileLoader::takePersistentOffset() {
	const auto offset = _persistent->requests.take();
	if (_persistent->trace) {
		qInfo() << "Persistent download" << _persistent->id
			<< "request offset" << offset << persistentBackend();
	}
	return offset;
}

bool PersistentFileLoader::preservePersistentOnDestruction() {
	if (!_persistent) {
		return false;
	} else if (_finished) {
		return true;
	}
	const auto publishing = (_persistent->state == Persistent::State::Publishing);
	_persistent->timer.cancel();
	if (!publishing) {
		_persistent->worker.with([](Sink &sink) {
			const auto result = sink.suspend();
			if (result != Sink::Result::Ok) {
				qInfo() << "Persistent download teardown checkpoint failed"
					<< int(result);
			}
		});
	}
	_finished = true;
	return true;
}

void PersistentFileLoader::enablePersistent(PendingDownload job) {
	Expects(!_persistent && !_fileIsOpen && _toCache == LoadToFileOnly);
	_persistent = std::make_unique<Persistent>(
		std::move(job),
		[=] { checkpointPersistent(); });
	TracePendingDownload(_persistent->id, persistentBackend());
}

QUuid PersistentFileLoader::persistentId() const {
	return _persistent ? _persistent->id : QUuid();
}

bool PersistentFileLoader::checkForOpen() {
	return _persistent || FileLoader::checkForOpen();
}

int64 PersistentFileLoader::currentOffset() const {
	return _persistent ? _persistent->ready : FileLoader::currentOffset();
}

bool PersistentFileLoader::canCancel() {
	return !_persistent || (_persistent->state != Persistent::State::Publishing
		&& Core::App().downloadManager().updatePending(
			&session(),
			_persistent->id,
			PendingState::Cancelling));
}

bool PersistentFileLoader::persistentCanSuspend() const {
	return _persistent && _persistent->state == Persistent::State::Running;
}

bool PersistentFileLoader::isPersistent() const {
	return _persistent != nullptr;
}

bool PersistentFileLoader::persistentCanResume() const {
	return _persistent && (_persistent->state == Persistent::State::Suspended
		|| _persistent->state == Persistent::State::Failed);
}

std::optional<DownloadTransferView> PersistentFileLoader::persistentTransferView() const {
	if (!_persistent) {
		return std::nullopt;
	}
	using State = Persistent::State;
	const auto &state = *_persistent;
	const auto status = [&] {
		switch (state.state) {
		case State::Idle: return DownloadStatus::Preparing;
		case State::Opening:
			return state.job.stagingIdentity.isEmpty()
				? DownloadStatus::Preparing
				: DownloadStatus::Verifying;
		case State::Running: return DownloadStatus::Downloading;
		case State::Suspending: return DownloadStatus::Pausing;
		case State::Suspended: return (state.job.state == PendingState::Paused)
			? DownloadStatus::Paused : DownloadStatus::Interrupted;
		case State::Failed: return (state.job.state == PendingState::Unavailable)
			? DownloadStatus::Unavailable : DownloadStatus::Failed;
		case State::Publishing: return DownloadStatus::Finalizing;
		}
		return DownloadStatus::Preparing;
	}();
	return DownloadTransferView{
		.status = status,
		.issue = state.issue,
		.written = state.ready,
		.committed = state.committed,
		.bytesPerSecond = (status == DownloadStatus::Downloading)
			? state.rate.rate(crl::now()) : 0,
	};
}

void PersistentFileLoader::notifyPersistentProgress(bool transition) {
	Core::App().downloadManager().pendingLoaderUpdated(
		transition, { session().uniqueId(), _persistent->id });
	notifyAboutProgress();
}

void PersistentFileLoader::resumePersistent(const QByteArray &reference) {
	if (persistentCanResume()) {
		updatePersistentReference(reference);
		openPersistent(!_persistent->job.stagingIdentity.isEmpty());
	}
}

void PersistentFileLoader::stopPersistentForUnavailableSource() {
	if (!_persistent || _persistent->state == Persistent::State::Publishing) {
		return;
	}
	_persistent->job.state = PendingState::Unavailable;
	failPersistent(Sink::Result::TransferFailed);
}

void PersistentFileLoader::openPersistent(bool resume) {
	auto &state = *_persistent;
	state.state = Persistent::State::Opening;
	state.issue = DownloadIssue::None;
	state.rate = {};
	Core::App().downloadManager().pendingLoaderUpdated(true);
	state.requests.invalidate();
	const auto generation = state.requests.generation();
	auto done = crl::guard(static_cast<FileLoader*>(this), [=](
			Sink::Result result,
			QByteArray identity,
			QBitArray present,
			int64 size) {
		if (generation != _persistent->requests.generation()) {
			return;
		}
		if (result != Sink::Result::Ok || resume) {
			persistentOpened(result, std::move(present), size);
			return;
		}
		auto &state = *_persistent;
		auto &manager = Core::App().downloadManager();
		if (!manager.identifyPending(&session(), state.id, identity)) {
			state.worker.with([id = state.id](Sink &sink) {
				const auto result = sink.cancel();
				TracePendingDownload(id, "unregistered staging cleanup", int(result));
			});
			failPersistent(Sink::Result::CheckpointFailed);
			return;
		}
		state.job.stagingIdentity = std::move(identity);
		auto checkpointed = crl::guard(static_cast<FileLoader*>(this), [=](
				Sink::Result result,
				QBitArray present) {
			if (generation == _persistent->requests.generation()) {
				persistentOpened(result, std::move(present), 0);
			}
		});
		state.worker.with([=, cancelled = state.cancelled](Sink &sink) mutable {
			if (cancelled->load()) {
				return;
			}
			const auto result = sink.checkpoint();
			crl::on_main([checkpointed = std::move(checkpointed), result,
					present = sink.committed()]() mutable {
				checkpointed(result, std::move(present));
			});
		});
	});
	state.worker.with([=, cancelled = state.cancelled,
			identity = state.job.stagingIdentity,
			initializing = state.job.state == PendingState::Initializing](Sink &sink) mutable {
		if (cancelled->load()) {
			return;
		}
		const auto result = sink.open(resume, identity, !resume, initializing);
		crl::on_main([done = std::move(done), result, identity = sink.identity(),
				present = sink.committed(), size = sink.writtenSize()]() mutable {
			done(result, std::move(identity), std::move(present), size);
		});
	});
}

void PersistentFileLoader::persistentOpened(
		Sink::Result result,
		QBitArray present,
		int64 size) {
	if (result != Sink::Result::Ok) {
		failPersistent(result);
		return;
	}
	auto &state = *_persistent;
	if (!Core::App().downloadManager().updatePending(
			&session(), state.id, PendingState::Downloading)) {
		failPersistent(Sink::Result::CheckpointFailed);
		return;
	}
	state.job.state = PendingState::Downloading;
	if (state.trace && present.count(true) > 0) {
		for (auto first = qsizetype(0); first < present.size();) {
			if (!present.testBit(first)) {
				++first;
				continue;
			}
			auto end = first + 1;
			while (end < present.size() && present.testBit(end)) {
				++end;
			}
			qInfo() << "Persistent download" << state.id << "committed offsets"
				<< first * int64(kPersistentDownloadPartSize)
				<< (end - 1) * int64(kPersistentDownloadPartSize);
			first = end;
		}
	}
	state.requests.reset(std::move(present));
	state.ready = state.committed = size;
	state.state = Persistent::State::Running;
	if (Core::Quitting()) {
		suspendPersistent(false);
		return;
	}
	state.timer.callEach(kCheckpointInterval);
	const auto weak = base::make_weak(static_cast<FileLoader*>(this));
	notifyPersistentProgress(true);
	if (weak && persistentCanSuspend()) {
		if (size == _fullSize) {
			publishPersistent();
		} else {
			requestPersistentParts();
		}
	}
}

void PersistentFileLoader::stopPersistentRequests() {
	auto &state = *_persistent;
	state.requests.invalidate();
	state.timer.cancel();
	state.checkpointQueued = false;
	detachPersistentTransport();
}

void PersistentFileLoader::suspendPersistent(bool manual, Fn<void()> finished) {
	if (!persistentCanSuspend()) {
		if (finished) {
			finished();
		}
		return;
	}
	auto &state = *_persistent;
	const auto intent = manual ? PendingState::Paused : PendingState::Interrupted;
	const auto saved = Core::App().downloadManager().updatePending(&session(), state.id, intent);
	if (!saved && manual) {
		if (finished) {
			finished();
		}
		return;
	}
	if (saved) {
		state.job.state = intent;
	}
	state.state = Persistent::State::Suspending;
	Core::App().downloadManager().pendingLoaderUpdated(true);
	stopPersistentRequests();
	const auto generation = state.requests.generation();
	auto done = crl::guard(static_cast<FileLoader*>(this), [=, finished = std::move(finished)](
			Sink::Result result,
			int64 size,
			int64 committed) {
		if (finished) {
			finished();
		}
		if (_persistent->requests.generation() != generation) {
			return;
		}
		Storage::TracePendingDownload(_persistent->id, "suspend checkpoint", int(result));
		_persistent->ready = size;
		_persistent->committed = committed;
		_persistent->issue = DownloadIssueFromSink(result);
		_persistent->state = (result == Sink::Result::Ok)
			? Persistent::State::Suspended
			: Persistent::State::Failed;
		notifyPersistentProgress(true);
	});
	state.worker.with([=, cancelled = state.cancelled](Sink &sink) mutable {
		if (cancelled->load()) {
			return;
		}
		const auto result = sink.suspend();
		crl::on_main([done = std::move(done), result,
				size = sink.writtenSize(), committed = sink.committedSize()]() mutable {
			done(result, size, committed);
		});
	});
}

void PersistentFileLoader::checkpointPersistent() {
	auto &state = *_persistent;
	if (!persistentCanSuspend() || state.checkpointQueued) {
		return;
	}
	state.checkpointQueued = true;
	const auto generation = state.requests.generation();
	auto done = crl::guard(static_cast<FileLoader*>(this), [=](Sink::Result result, int64 committed) {
		if (_persistent->requests.generation() != generation) {
			return;
		}
		_persistent->checkpointQueued = false;
		_persistent->committed = committed;
		if (result != Sink::Result::Ok) {
			failPersistent(result);
		} else {
			Core::App().downloadManager().pendingLoaderUpdated(
				false, { session().uniqueId(), _persistent->id });
		}
	});
	state.worker.with([=, cancelled = state.cancelled](Sink &sink) mutable {
		if (cancelled->load()) {
			return;
		}
		const auto result = sink.checkpoint();
		crl::on_main([done = std::move(done), result, committed = sink.committedSize()]() mutable {
			done(result, committed);
		});
	});
}

void PersistentFileLoader::failPersistent(Sink::Result result) {
	auto &state = *_persistent;
	stopPersistentRequests();
	state.state = Persistent::State::Failed;
	state.issue = DownloadIssueFromSink(result);
	auto &manager = Core::App().downloadManager();
	const auto retry = (state.job.state == PendingState::Initializing
		|| state.job.state == PendingState::Paused
		|| state.job.state == PendingState::Unavailable
		|| state.job.state == PendingState::Finalizing)
		? state.job.state
		: PendingState::Retryable;
	if (!manager.updatePending(&session(), state.id, retry)) {
		Storage::TracePendingDownload(state.id, "retry state write failed");
	}
	state.worker.with([cancelled = state.cancelled](Sink &sink) {
		if (!cancelled->load()) {
			const auto result = sink.suspend();
			if (result != Sink::Result::Ok) {
				qInfo() << "Persistent download could not checkpoint on failure"
					<< int(result);
			}
		}
	});
	qInfo() << "Persistent download" << state.id << "failed" << int(result);
	notifyPersistentProgress(true);
}

void PersistentFileLoader::publishPersistent() {
	auto &state = *_persistent;
	state.state = Persistent::State::Publishing;
	Core::App().downloadManager().pendingLoaderUpdated(true);
	stopPersistentRequests();
	auto prepared = crl::guard(static_cast<FileLoader*>(this), [=](Sink::Result result) {
		if (result != Sink::Result::Ok
			|| !Core::App().downloadManager().updatePending(
				&session(), _persistent->id, PendingState::Finalizing)) {
			failPersistent(result == Sink::Result::Ok
				? Sink::Result::CheckpointFailed : result);
			return;
		}
		_persistent->job.state = PendingState::Finalizing;
		auto published = crl::guard(static_cast<FileLoader*>(this), [=](Sink::Result result) {
			if (result != Sink::Result::Ok) {
				failPersistent(result);
				return;
			}
			Platform::File::PostprocessDownloaded(QFileInfo(_filename).absoluteFilePath());
			const auto strong = base::make_weak(&session());
			const auto id = _persistent->id;
			if (finalizeResult()) {
				if (const auto session = strong.get()) {
					Core::App().downloadManager().completePending(session, id);
				}
			}
		});
		_persistent->worker.with([published = std::move(published)](Sink &sink) mutable {
			const auto result = sink.publish(true);
			crl::on_main([published = std::move(published), result]() mutable {
				published(result);
			});
		});
	});
	state.worker.with([prepared = std::move(prepared)](Sink &sink) mutable {
		const auto result = sink.preparePublish();
		crl::on_main([prepared = std::move(prepared), result]() mutable {
			prepared(result);
		});
	});
}

bool PersistentFileLoader::feedPersistentPart(
		int64 offset,
		const QByteArray &bytes,
		bool network) {
	if (!persistentCanSuspend()) {
		return false;
	}
	auto &state = *_persistent;
	const auto generation = state.requests.generation();
	auto done = crl::guard(static_cast<FileLoader*>(this), [=](
			Sink::Result result,
			int64 size,
			int64 committed) {
		if (_persistent->requests.generation() != generation) {
			return;
		}
		if (result != Sink::Result::Ok) {
			failPersistent(result);
			return;
		}
		auto &state = *_persistent;
		if (!state.requests.written(generation)) {
			return;
		}
		if (state.trace) {
			qInfo() << "Persistent download" << state.id
				<< "accepted write offset" << offset << "written bytes" << size;
		}
		if (network && size > state.ready) {
			state.rate.accept(size - state.ready, crl::now());
		}
		state.ready = size;
		state.committed = committed;
		if (size == _fullSize && !state.requests.outstanding()) {
			publishPersistent();
			return;
		}
		const auto weak = base::make_weak(static_cast<FileLoader*>(this));
		notifyPersistentProgress(false);
		if (weak && persistentCanSuspend()) {
			persistentPartWritten(offset);
		}
	});
	auto owned = (bytes.capacity() < bytes.size())
		? QByteArray(bytes.constData(), bytes.size())
		: bytes;
	state.worker.with([=, cancelled = state.cancelled,
			owned = std::move(owned)](Sink &sink) mutable {
		if (cancelled->load()) {
			return;
		}
		const auto result = sink.write(offset, owned);
		crl::on_main([done = std::move(done), result,
				size = sink.writtenSize(), committed = sink.committedSize()]() mutable {
			done(result, size, committed);
		});
	});
	return true;
}

bool PersistentFileLoader::cancelPersistent() {
	if (_persistent) {
		auto &state = *_persistent;
		state.state = Persistent::State::Failed;
		state.cancelled->store(true);
		stopPersistentRequests();
		auto done = crl::guard(&session(), [session = &session(), id = state.id](
				Sink::Result result) {
			Core::App().downloadManager().pendingCancelled(session, id, result);
		});
		state.worker.with([id = state.id, identity = state.job.stagingIdentity,
				done = std::move(done)](Sink &sink) mutable {
			const auto result = identity.isEmpty() ? sink.cancel() : sink.purge(identity);
			Storage::TracePendingDownload(id, "cancel cleanup", int(result));
			crl::on_main([done = std::move(done), result]() mutable {
				done(result);
			});
		});
		return true;
	}
	return false;
}

} // namespace Storage
