/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "storage/file_download_mtproto.h"

#include "data/data_document.h"
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

	explicit Persistent(Sink::Job job, Fn<void()> checkpoint)
	: id(job.id)
	, worker(std::move(job))
	, timer(std::move(checkpoint)) {
	}

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
	bool opened = false;
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

void mtpFileLoader::enablePersistent(bool replace) {
	Expects(!_persistent && !_fileIsOpen && _toCache == LoadToFileOnly);
	_persistent = std::make_unique<Persistent>(
		Sink::Job{ QUuid::createUuid(), _filename, _fullSize, replace },
		[=] { checkpointPersistent(); });
}

bool mtpFileLoader::checkForOpen() {
	return _persistent || FileLoader::checkForOpen();
}

int64 mtpFileLoader::currentOffset() const {
	return _persistent ? _persistent->ready : FileLoader::currentOffset();
}

bool mtpFileLoader::canCancel() const {
	return !_persistent || _persistent->state != Persistent::State::Publishing;
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

void mtpFileLoader::resumePersistent() {
	if (persistentCanResume()) {
		openPersistent(_persistent->opened);
	}
}

void mtpFileLoader::openPersistent(bool resume) {
	auto &state = *_persistent;
	state.state = Persistent::State::Opening;
	state.requests.invalidate();
	const auto generation = state.requests.generation();
	auto done = crl::guard(static_cast<FileLoader*>(this), [=](
			Sink::Result result,
			QBitArray present,
			int64 size) {
		if (generation != _persistent->requests.generation()) {
			return;
		}
		if (result != Sink::Result::Ok) {
			failPersistent(result);
			return;
		}
		auto &state = *_persistent;
		state.opened = true;
		state.requests.reset(std::move(present));
		state.ready = size;
		state.state = Persistent::State::Running;
		state.timer.callEach(kCheckpointInterval);
		const auto weak = base::make_weak(static_cast<FileLoader*>(this));
		notifyAboutProgress();
		if (!weak) {
			return;
		}
		if (size == _fullSize && persistentCanSuspend()) {
			publishPersistent();
		} else if (persistentCanSuspend()) {
			addToQueue();
		}
	});
	state.worker.with([=, cancelled = state.cancelled](Sink &sink) mutable {
		if (cancelled->load()) {
			return;
		}
		const auto result = sink.open(resume);
		crl::on_main([done = std::move(done), result,
				present = sink.committed(), size = sink.writtenSize()]() mutable {
			done(result, std::move(present), size);
		});
	});
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

void mtpFileLoader::suspendPersistent() {
	if (!persistentCanSuspend()) {
		return;
	}
	auto &state = *_persistent;
	state.state = Persistent::State::Suspending;
	stopPersistentRequests();
	const auto generation = state.requests.generation();
	auto done = crl::guard(static_cast<FileLoader*>(this), [=](
			Sink::Result result,
			int64 size) {
		if (_persistent->requests.generation() != generation) {
			return;
		}
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
	state.state = Persistent::State::Failed;
	stopPersistentRequests();
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
	auto done = crl::guard(static_cast<FileLoader*>(this), [=](Sink::Result result) {
		if (result != Sink::Result::Ok) {
			failPersistent(result);
			return;
		}
		Platform::File::PostprocessDownloaded(QFileInfo(_filename).absoluteFilePath());
		finalizeResult();
	});
	state.worker.with([done = std::move(done)](Sink &sink) mutable {
		const auto result = sink.publish();
		crl::on_main([done = std::move(done), result]() mutable {
			done(result);
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
			openPersistent(false);
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
		state.worker.with([id = state.id](Sink &sink) {
			const auto result = sink.cancel();
			if (result != Sink::Result::Ok) {
				qInfo() << "Persistent download" << id << "cleanup failed" << int(result);
			}
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