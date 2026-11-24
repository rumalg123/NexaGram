/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "storage/file_download_mtproto.h"

#include "data/data_document.h"
#include "data/data_download_manager.h"
#include "core/application.h"
#include "data/data_file_origin.h"
#include "storage/cache/storage_cache_types.h"
#include "main/main_session.h"
#include "apiwrap.h"
#include "mtproto/mtp_instance.h"
#include "mtproto/mtproto_config.h"
#include "mtproto/mtproto_auth_key.h"
#include "platform/platform_file_utilities.h"

#include <crl/crl_object_on_queue.h>

#include <QtCore/QDebug>
#include <QtCore/QFileInfo>

#include <atomic>

namespace {

using Sink = Storage::PersistentFileSink;
using PendingState = Storage::PendingDownloadState;

constexpr auto kCheckpointInterval = 2 * crl::time(1000);
constexpr auto kMaximumPendingParts = 128;
static_assert(Storage::kPersistentDownloadPartSize == Storage::kDownloadPartSize);

} // namespace

struct mtpFileLoader::Persistent {
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
	bool checkpointQueued = false;
};

mtpFileLoader::mtpFileLoader(
	not_null<Main::Session*> session,
	const StorageFileLocation &location,
	Data::FileOrigin origin,
	LocationType type,
	const QString &to,
	int64 loadSize,
	int64 fullSize,
	LoadToCacheSetting toCache,
	LoadFromCloudSetting fromCloud,
	bool autoLoading,
	uint8 cacheTag)
: FileLoader(
	session,
	to,
	loadSize,
	fullSize,
	type,
	toCache,
	fromCloud,
	autoLoading,
	cacheTag)
, DownloadMtprotoTask(&session->downloader(), location, origin) {
}

mtpFileLoader::mtpFileLoader(
	not_null<Main::Session*> session,
	const WebFileLocation &location,
	int64 loadSize,
	int64 fullSize,
	LoadFromCloudSetting fromCloud,
	bool autoLoading,
	uint8 cacheTag)
: FileLoader(
	session,
	QString(),
	loadSize,
	fullSize,
	UnknownFileLocation,
	LoadToCacheAsWell,
	fromCloud,
	autoLoading,
	cacheTag)
, DownloadMtprotoTask(
	&session->downloader(),
	session->serverConfig().webFileDcId,
	{ location }) {
}

mtpFileLoader::mtpFileLoader(
	not_null<Main::Session*> session,
	const GeoPointLocation &location,
	int64 loadSize,
	int64 fullSize,
	LoadFromCloudSetting fromCloud,
	bool autoLoading,
	uint8 cacheTag)
: FileLoader(
	session,
	QString(),
	loadSize,
	fullSize,
	UnknownFileLocation,
	LoadToCacheAsWell,
	fromCloud,
	autoLoading,
	cacheTag)
, DownloadMtprotoTask(
	&session->downloader(),
	session->serverConfig().webFileDcId,
	{ location }) {
}

mtpFileLoader::mtpFileLoader(
	not_null<Main::Session*> session,
	const AudioAlbumThumbLocation &location,
	int64 loadSize,
	int64 fullSize,
	LoadFromCloudSetting fromCloud,
	bool autoLoading,
	uint8 cacheTag)
: FileLoader(
	session,
	QString(),
	loadSize,
	fullSize,
	UnknownFileLocation,
	LoadToCacheAsWell,
	fromCloud,
	autoLoading,
	cacheTag)
, DownloadMtprotoTask(
	&session->downloader(),
	session->serverConfig().webFileDcId,
	{ location }) {
}

mtpFileLoader::~mtpFileLoader() {
	if (_finished) {
		return;
	} else if (!_persistent) {
		cancel();
		return;
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
}

void mtpFileLoader::enablePersistent(Storage::PendingDownload job) {
	Expects(!_persistent && !_fileIsOpen && _toCache == LoadToFileOnly);
	_persistent = std::make_unique<Persistent>(
		std::move(job),
		[=] { checkpointPersistent(); });
}

QUuid mtpFileLoader::persistentId() const {
	return _persistent ? _persistent->id : QUuid();
}

bool mtpFileLoader::checkForOpen() {
	return _persistent || FileLoader::checkForOpen();
}

int64 mtpFileLoader::currentOffset() const {
	return _persistent ? _persistent->ready : FileLoader::currentOffset();
}

bool mtpFileLoader::canCancel() {
	return !_persistent || (_persistent->state != Persistent::State::Publishing
		&& Core::App().downloadManager().updatePending(
			&session(),
			_persistent->id,
			PendingState::Cancelling));
}

bool mtpFileLoader::persistentCanSuspend() const {
	return _persistent && _persistent->state == Persistent::State::Running;
}

bool mtpFileLoader::isPersistent() const {
	return _persistent != nullptr;
}

bool mtpFileLoader::persistentCanResume() const {
	return _persistent && (_persistent->state == Persistent::State::Suspended
		|| _persistent->state == Persistent::State::Failed);
}

void mtpFileLoader::resumePersistent(const QByteArray &reference) {
	if (persistentCanResume()) {
		updateFileReference(reference);
		openPersistent(!_persistent->job.stagingIdentity.isEmpty());
	}
}

void mtpFileLoader::stopPersistentForUnavailableSource() {
	if (!_persistent || _persistent->state == Persistent::State::Publishing) {
		return;
	}
	_persistent->job.state = PendingState::Unavailable;
	failPersistent(Sink::Result::TransferFailed);
}

void mtpFileLoader::openPersistent(bool resume) {
	auto &state = *_persistent;
	state.state = Persistent::State::Opening;
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
			state.worker.with([](Sink &sink) {
				const auto result = sink.cancel();
				Storage::TracePendingDownload({}, "unregistered staging cleanup", int(result));
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

void mtpFileLoader::persistentOpened(
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
				<< first * int64(Storage::kDownloadPartSize)
				<< (end - 1) * int64(Storage::kDownloadPartSize);
			first = end;
		}
	}
	state.requests.reset(std::move(present));
	state.ready = size;
	state.state = Persistent::State::Running;
	if (Core::Quitting()) {
		suspendPersistent(false);
		return;
	}
	state.timer.callEach(kCheckpointInterval);
	const auto weak = base::make_weak(static_cast<FileLoader*>(this));
	notifyAboutProgress();
	if (weak && persistentCanSuspend()) {
		if (size == _fullSize) {
			publishPersistent();
		} else {
			addToQueue();
		}
	}
}

void mtpFileLoader::stopPersistentRequests() {
	auto &state = *_persistent;
	state.requests.invalidate();
	state.timer.cancel();
	state.checkpointQueued = false;
	removeFromQueue();
	cancelAllRequests();
	session().downloader().checkSendNextAfterSuccess(dcId());
}

void mtpFileLoader::suspendPersistent(bool manual, Fn<void()> finished) {
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
	stopPersistentRequests();
	const auto generation = state.requests.generation();
	auto done = crl::guard(static_cast<FileLoader*>(this), [=, finished = std::move(finished)](
			Sink::Result result,
			int64 size) {
		if (finished) {
			finished();
		}
		if (_persistent->requests.generation() != generation) {
			return;
		}
		Storage::TracePendingDownload(_persistent->id, "suspend checkpoint", int(result));
		_persistent->ready = size;
		_persistent->state = (result == Sink::Result::Ok)
			? Persistent::State::Suspended
			: Persistent::State::Failed;
		notifyAboutProgress();
	});
	state.worker.with([=, cancelled = state.cancelled](Sink &sink) mutable {
		if (cancelled->load()) {
			return;
		}
		const auto result = sink.suspend();
		crl::on_main([done = std::move(done), result,
				size = sink.writtenSize()]() mutable {
			done(result, size);
		});
	});
}

void mtpFileLoader::checkpointPersistent() {
	auto &state = *_persistent;
	if (!persistentCanSuspend() || state.checkpointQueued) {
		return;
	}
	state.checkpointQueued = true;
	const auto generation = state.requests.generation();
	auto done = crl::guard(static_cast<FileLoader*>(this), [=](Sink::Result result) {
		if (_persistent->requests.generation() != generation) {
			return;
		}
		_persistent->checkpointQueued = false;
		if (result != Sink::Result::Ok) {
			failPersistent(result);
		}
	});
	state.worker.with([=, cancelled = state.cancelled](Sink &sink) mutable {
		if (cancelled->load()) {
			return;
		}
		const auto result = sink.checkpoint();
		crl::on_main([done = std::move(done), result]() mutable {
			done(result);
		});
	});
}

void mtpFileLoader::failPersistent(Sink::Result result) {
	auto &state = *_persistent;
	stopPersistentRequests();
	state.state = Persistent::State::Failed;
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
	notifyAboutProgress();
}

void mtpFileLoader::publishPersistent() {
	auto &state = *_persistent;
	state.state = Persistent::State::Publishing;
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

bool mtpFileLoader::feedPersistentPart(int64 offset, const QByteArray &bytes) {
	if (!persistentCanSuspend()) {
		return false;
	}
	auto &state = *_persistent;
	const auto generation = state.requests.generation();
	auto done = crl::guard(static_cast<FileLoader*>(this), [=](
			Sink::Result result,
			int64 size) {
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
		state.ready = size;
		if (size == _fullSize && !state.requests.outstanding()) {
			publishPersistent();
			return;
		}
		const auto weak = base::make_weak(static_cast<FileLoader*>(this));
		notifyAboutProgress();
		if (weak && persistentCanSuspend()) {
			session().downloader().checkSendNextAfterSuccess(dcId());
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
				size = sink.writtenSize()]() mutable {
			done(result, size);
		});
	});
	return true;
}

Data::FileOrigin mtpFileLoader::fileOrigin() const {
	return DownloadMtprotoTask::fileOrigin();
}

uint64 mtpFileLoader::objId() const {
	return DownloadMtprotoTask::objectId();
}

bool mtpFileLoader::readyToRequest() const {
	if (_persistent) {
		const auto &state = *_persistent;
		return persistentCanSuspend() && !state.requests.empty()
			&& state.requests.outstanding() < kMaximumPendingParts;
	}
	return !_finished
		&& !_lastComplete
		&& (_fullSize != 0 || !haveSentRequests())
		&& (!_fullSize || _nextRequestOffset < _loadSize);
}

int64 mtpFileLoader::takeNextRequestOffset() {
	Expects(readyToRequest());

	if (_persistent) {
		auto &state = *_persistent;
		const auto offset = state.requests.take();
		if (state.trace) {
			qInfo() << "Persistent download" << state.id << "request offset" << offset;
		}
		return offset;
	}
	const auto result = _nextRequestOffset;
	_nextRequestOffset += Storage::kDownloadPartSize;
	return result;
}

bool mtpFileLoader::feedPart(int64 offset, const QByteArray &bytes) {
	if (_persistent) {
		return feedPersistentPart(offset, bytes);
	}
	const auto buffer = bytes::make_span(bytes);
	if (!writeResultPart(offset, buffer)) {
		return false;
	}
	if (buffer.empty() || (buffer.size() % 1024)) { // bad next offset
		_lastComplete = true;
	}
	const auto finished = !haveSentRequests()
		&& (_lastComplete || (_fullSize && _nextRequestOffset >= _loadSize));
	if (finished) {
		removeFromQueue();
		if (!finalizeResult()) {
			return false;
		}
	} else {
		notifyAboutProgress();
	}
	return true;
}

void mtpFileLoader::cancelOnFail() {
	if (_persistent) {
		failPersistent(Sink::Result::TransferFailed);
		return;
	}
	cancel(FailureReason::OtherFailure);
}

bool mtpFileLoader::setWebFileSizeHook(int64 size) {
	if (!_fullSize || _fullSize == size) {
		_fullSize = _loadSize = size;
		return true;
	}
	LOG(("MTP Error: "
		"Bad size provided by bot for webDocument: %1, real: %2"
		).arg(_fullSize
		).arg(size));
	cancel(FailureReason::OtherFailure);
	return false;
}

void mtpFileLoader::startLoading() {
	if (_persistent) {
		if (_persistent->state == Persistent::State::Idle) {
			openPersistent(!_persistent->job.stagingIdentity.isEmpty());
		}
		return;
	}
	addToQueue();
}

void mtpFileLoader::startLoadingWithPartial(const QByteArray &data) {
	Expects(data.startsWith("partial:"));

	constexpr auto kPrefix = 8;
	const auto parts = (data.size() - kPrefix) / Storage::kDownloadPartSize;
	const auto use = parts * int64(Storage::kDownloadPartSize);
	if (use > 0) {
		_nextRequestOffset = use;
		feedPart(0, QByteArray::fromRawData(data.data() + kPrefix, use));
	}
	startLoading();
}

void mtpFileLoader::cancelHook() {
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
		return;
	}
	cancelAllRequests();
}

Storage::Cache::Key mtpFileLoader::cacheKey() const {
	return v::match(location().data, [&](const WebFileLocation &location) {
		return Data::WebDocumentCacheKey(location);
	}, [&](const GeoPointLocation &location) {
		return Data::GeoPointCacheKey(location);
	}, [&](const StorageFileLocation &location) {
		return location.cacheKey();
	}, [&](const AudioAlbumThumbLocation &location) {
		return Data::AudioAlbumThumbCacheKey(location);
	});
}

std::optional<MediaKey> mtpFileLoader::fileLocationKey() const {
	if (_locationType != UnknownFileLocation) {
		return mediaKey(_locationType, dcId(), objId());
	}
	return std::nullopt;
}
