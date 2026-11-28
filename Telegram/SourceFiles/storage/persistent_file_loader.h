/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "storage/file_download.h"
#include "storage/pending_downloads.h"

namespace Storage {

class PersistentFileLoader : public FileLoader {
public:
	~PersistentFileLoader() override;

	int64 currentOffset() const override;
	bool canCancel() override;

	void enablePersistent(PendingDownload job);
	[[nodiscard]] QUuid persistentId() const;
	[[nodiscard]] bool persistentCanSuspend() const;
	[[nodiscard]] bool persistentCanResume() const;
	[[nodiscard]] bool isPersistent() const;
	[[nodiscard]] std::optional<DownloadTransferView> persistentTransferView() const;
	void suspendPersistent(bool manual = true, Fn<void()> done = nullptr);
	void resumePersistent(const QByteArray &reference);
	void stopPersistentForUnavailableSource();
	void releasePersistentSource(
		Fn<void(PersistentFileSink::Result, QByteArray, int64)> done);
	void interruptForRetirement();

protected:
	PersistentFileLoader(
		not_null<Main::Session*> session,
		const QString &toFile,
		int64 loadSize,
		int64 fullSize,
		LocationType locationType,
		LoadToCacheSetting toCache,
		LoadFromCloudSetting fromCloud,
		bool autoLoading,
		uint8 cacheTag);

	bool startPersistent();
	bool cancelPersistent();
	bool preservePersistentOnDestruction();
	[[nodiscard]] bool persistentCanRequest(int limit) const;
	[[nodiscard]] int64 takePersistentOffset();
	bool feedPersistentPart(int64 offset, const QByteArray &bytes, bool network = true);
	void failPersistent(PersistentFileSink::Result result);

private:
	struct Persistent;

	bool checkForOpen() override;
	virtual const char *persistentBackend() const = 0;
	virtual void requestPersistentParts() = 0;
	virtual void detachPersistentTransport() = 0;
	virtual void persistentPartWritten(int64 offset) = 0;
	virtual void updatePersistentReference(const QByteArray &reference) = 0;

	void openPersistent(bool resume);
	void persistentOpened(
		PersistentFileSink::Result result,
		QBitArray present,
		int64 size);
	void checkpointPersistent();
	void publishPersistent();
	void stopPersistentRequests();
	void notifyPersistentProgress(bool transition);

	std::unique_ptr<Persistent> _persistent;

};

} // namespace Storage
