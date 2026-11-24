/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "storage/file_download.h"
#include "storage/download_manager_mtproto.h"
#include "storage/pending_downloads.h"

class mtpFileLoader final
	: public FileLoader
	, private Storage::DownloadMtprotoTask {
public:
	mtpFileLoader(
		not_null<Main::Session*> session,
		const StorageFileLocation &location,
		Data::FileOrigin origin,
		LocationType type,
		const QString &toFile,
		int64 loadSize,
		int64 fullSize,
		LoadToCacheSetting toCache,
		LoadFromCloudSetting fromCloud,
		bool autoLoading,
		uint8 cacheTag);
	mtpFileLoader(
		not_null<Main::Session*> session,
		const WebFileLocation &location,
		int64 loadSize,
		int64 fullSize,
		LoadFromCloudSetting fromCloud,
		bool autoLoading,
		uint8 cacheTag);
	mtpFileLoader(
		not_null<Main::Session*> session,
		const GeoPointLocation &location,
		int64 loadSize,
		int64 fullSize,
		LoadFromCloudSetting fromCloud,
		bool autoLoading,
		uint8 cacheTag);
	mtpFileLoader(
		not_null<Main::Session*> session,
		const AudioAlbumThumbLocation &location,
		int64 loadSize,
		int64 fullSize,
		LoadFromCloudSetting fromCloud,
		bool autoLoading,
		uint8 cacheTag);
	~mtpFileLoader();

	Data::FileOrigin fileOrigin() const override;
	uint64 objId() const override;
	int64 currentOffset() const override;
	bool canCancel() override;

	void enablePersistent(Storage::PendingDownload job);
	[[nodiscard]] QUuid persistentId() const;
	[[nodiscard]] bool persistentCanSuspend() const;
	[[nodiscard]] bool persistentCanResume() const;
	[[nodiscard]] bool isPersistent() const;
	void suspendPersistent(bool manual = true, Fn<void()> done = nullptr);
	void resumePersistent(const QByteArray &reference);
	void stopPersistentForUnavailableSource();

private:
	struct Persistent;

	bool checkForOpen() override;
	void openPersistent(bool resume);
	void persistentOpened(
		Storage::PersistentFileSink::Result result,
		QBitArray present,
		int64 size);
	void checkpointPersistent();
	void failPersistent(Storage::PersistentFileSink::Result result);
	void publishPersistent();
	bool feedPersistentPart(int64 offset, const QByteArray &bytes);
	void stopPersistentRequests();

	Storage::Cache::Key cacheKey() const override;
	std::optional<MediaKey> fileLocationKey() const override;
	void startLoading() override;
	void startLoadingWithPartial(const QByteArray &data) override;
	void cancelHook() override;

	bool readyToRequest() const override;
	int64 takeNextRequestOffset() override;
	bool feedPart(int64 offset, const QByteArray &bytes) override;
	void cancelOnFail() override;
	bool setWebFileSizeHook(int64 size) override;

	std::unique_ptr<Persistent> _persistent;
	bool _lastComplete = false;
	int64 _nextRequestOffset = 0;

};
