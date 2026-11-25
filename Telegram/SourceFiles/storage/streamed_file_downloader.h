/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "data/data_file_origin.h"
#include "storage/cache/storage_cache_types.h"
#include "storage/persistent_file_loader.h"

namespace Media {
namespace Streaming {
class Reader;
struct LoadedPart;
} // namespace Streaming
} // namespace Media

namespace Storage {

class StreamedFileDownloader final : public PersistentFileLoader {
public:
	StreamedFileDownloader(
		not_null<Main::Session*> session,

		uint64 objectId,
		MTP::DcId dcId,
		Data::FileOrigin origin,
		Cache::Key cacheKey,
		std::optional<MediaKey> fileLocationKey,
		std::shared_ptr<Media::Streaming::Reader> reader,

		// For FileLoader
		const QString &toFile,
		int64 size,
		LocationType locationType,
		LoadToCacheSetting toCache,
		LoadFromCloudSetting fromCloud,
		bool autoLoading,
		uint8 cacheTag);
	~StreamedFileDownloader();

	uint64 objId() const override;
	Data::FileOrigin fileOrigin() const override;

	QByteArray readLoadedPart(int64 offset);

private:
	const char *persistentBackend() const override;
	void requestPersistentParts() override;
	void detachPersistentTransport() override;
	void persistentPartWritten(int64 offset) override;
	void updatePersistentReference(const QByteArray &reference) override;

	void startLoading() override;
	Cache::Key cacheKey() const override;
	std::optional<MediaKey> fileLocationKey() const override;
	void cancelHook() override;
	void requestParts();
	void requestPart();
	void attachReader();
	void detachReader();

	void savePart(const Media::Streaming::LoadedPart &part);

	uint64 _objectId = 0;
	Data::FileOrigin _origin;
	Cache::Key _cacheKey;
	std::optional<MediaKey> _fileLocationKey;
	std::shared_ptr<Media::Streaming::Reader> _reader;
	uint64 _attachment = 0;

	std::vector<bool> _partIsSaved; // vector<bool> :D
	mutable int _nextPartIndex = 0;
	int _partsCount = 0;
	int _partsRequested = 0;
	int _partsSaved = 0;

	rpl::lifetime _lifetime;

};

} // namespace Storage
