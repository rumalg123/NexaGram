/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "storage/streamed_file_downloader.h"

#include "media/streaming/media_streaming_loader.h"
#include "media/streaming/media_streaming_reader.h"

namespace Storage {
namespace {

using namespace Media::Streaming;

constexpr auto kPartSize = Loader::kPartSize;
constexpr auto kRequestPartsCount = 32;
static_assert(kPartSize == kPersistentDownloadPartSize);

} // namespace

StreamedFileDownloader::StreamedFileDownloader(
	not_null<Main::Session*> session,

	uint64 objectId,
	MTP::DcId dcId,
	Data::FileOrigin origin,
	Cache::Key cacheKey,
	std::optional<MediaKey> fileLocationKey,
	std::shared_ptr<Reader> reader,

	// For FileLoader
	const QString &toFile,
	int64 size,
	LocationType locationType,
	LoadToCacheSetting toCache,
	LoadFromCloudSetting fromCloud,
	bool autoLoading,
	uint8 cacheTag)
: PersistentFileLoader(
	session,
	toFile,
	size,
	size,
	locationType,
	toCache,
	fromCloud,
	autoLoading,
	cacheTag)
, _objectId(objectId)
, _origin(origin)
, _cacheKey(cacheKey)
, _fileLocationKey(fileLocationKey)
, _reader(std::move(reader))
, _partsCount((size + kPartSize - 1) / kPartSize) {
	_partIsSaved.resize(_partsCount, false);

	_reader->partsForDownloader(
	) | rpl::on_next([=](const Reader::DownloadPart &delivery) {
		if (delivery.attachment != _attachment) {
			return;
		}
		const auto &part = delivery.part;
		if (isPersistent()) {
			if (!part.valid(_fullSize)) {
				failPersistent(PersistentFileSink::Result::TransferFailed);
			} else {
				feedPersistentPart(part.offset, part.bytes, delivery.downloadNetwork);
			}
		} else if (!part.valid(_fullSize)) {
			cancel(FailureReason::OtherFailure);
		} else {
			savePart(part);
		}
	}, _lifetime);
}

StreamedFileDownloader::~StreamedFileDownloader() {
	if (!preservePersistentOnDestruction() && !_finished) {
		cancel();
	} else {
		detachReader();
	}
}

const char *StreamedFileDownloader::persistentBackend() const {
	return "backend streaming";
}

void StreamedFileDownloader::attachReader() {
	if (!_attachment) {
		_attachment = _reader->attachDownloader(this, persistentId());
	}
}

void StreamedFileDownloader::detachReader() {
	if (const auto attachment = std::exchange(_attachment, 0)) {
		_reader->cancelForDownloader(attachment);
	}
}

void StreamedFileDownloader::requestPersistentParts() {
	attachReader();
	while (persistentCanRequest(kRequestPartsCount)) {
		_reader->loadForDownloader(_attachment, takePersistentOffset());
	}
	_reader->continueDownloaderFromMainThread();
}

void StreamedFileDownloader::detachPersistentTransport() {
	detachReader();
}

void StreamedFileDownloader::persistentPartWritten(int64 offset) {
	_reader->doneForDownloader(_attachment, offset);
	requestPersistentParts();
}

void StreamedFileDownloader::updatePersistentReference(const QByteArray &reference) {
	_reader->updateDownloadReference(reference);
}

uint64 StreamedFileDownloader::objId() const {
	return _objectId;
}

Data::FileOrigin StreamedFileDownloader::fileOrigin() const {
	return _origin;
}

void StreamedFileDownloader::requestParts() {
	attachReader();
	while (!_finished
		&& _nextPartIndex < _partsCount
		&& _partsRequested < kRequestPartsCount) {
		requestPart();
	}
	_reader->continueDownloaderFromMainThread();
}

void StreamedFileDownloader::requestPart() {
	Expects(!_finished);

	const auto index = std::find(
		begin(_partIsSaved) + _nextPartIndex,
		end(_partIsSaved),
		false
	) - begin(_partIsSaved);
	if (index == _partsCount) {
		_nextPartIndex = _partsCount;
		return;
	}
	_nextPartIndex = index + 1;
	_reader->loadForDownloader(_attachment, index * kPartSize);
	++_partsRequested;
}

QByteArray StreamedFileDownloader::readLoadedPart(int64 offset) {
	Expects(offset >= 0 && offset < _fullSize);
	Expects(!(offset % kPartSize));

	const auto index = (offset / kPartSize);
	return !isPersistent() && _partIsSaved[index]
		? readLoadedPartBack(offset, kPartSize)
		: QByteArray();
}

Storage::Cache::Key StreamedFileDownloader::cacheKey() const {
	return _cacheKey;
}

std::optional<MediaKey> StreamedFileDownloader::fileLocationKey() const {
	return _fileLocationKey;
}

void StreamedFileDownloader::cancelHook() {
	if (cancelPersistent()) {
		return;
	}
	_partsRequested = 0;
	_nextPartIndex = 0;

	detachReader();
}

void StreamedFileDownloader::startLoading() {
	if (!startPersistent()) {
		requestParts();
	}
}

void StreamedFileDownloader::savePart(const LoadedPart &part) {
	Expects(part.offset >= 0 && part.offset < _reader->size());
	Expects(part.offset % kPartSize == 0);

	if (_finished || _cancelled) {
		return;
	}

	const auto offset = part.offset;
	const auto index = offset / kPartSize;
	Assert(index >= 0 && index < _partsCount);
	if (_partIsSaved[index]) {
		return;
	}
	_partIsSaved[index] = true;
	++_partsSaved;

	if (index < _nextPartIndex) {
		--_partsRequested;
	}
	if (!writeResultPart(offset, bytes::make_span(part.bytes))) {
		return;
	}
	_reader->doneForDownloader(_attachment, offset);
	if (_partsSaved == _partsCount) {
		detachReader();
		finalizeResult();
	} else {
		requestParts();
		notifyAboutProgress();
	}
}

} // namespace Storage
