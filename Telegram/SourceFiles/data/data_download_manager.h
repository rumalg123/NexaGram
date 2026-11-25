/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/timer.h"
#include "base/weak_ptr.h"
#include "storage/pending_downloads.h"

#include <atomic>

namespace Ui {
struct DownloadBarProgress;
struct DownloadBarContent;
} // namespace Ui

namespace Main {
class Session;
} // namespace Main

namespace Data {

// Used in serialization!
enum class DownloadType {
	Document,
	Photo,
};

// unixtime * 1000.
using DownloadDate = int64;

[[nodiscard]] inline TimeId DateFromDownloadDate(DownloadDate date) {
	return date / 1000;
}

struct DownloadId {
	uint64 objectId = 0;
	DownloadType type = DownloadType::Document;
};

struct DownloadProgress {
	int64 ready = 0;
	int64 total = 0;
};
inline constexpr bool operator==(
		const DownloadProgress &a,
		const DownloadProgress &b) {
	return (a.ready == b.ready) && (a.total == b.total);
}

struct DownloadObject {
	not_null<HistoryItem*> item;
	DocumentData *document = nullptr;
	PhotoData *photo = nullptr;
};

struct DownloadedId {
	DownloadId download;
	DownloadDate started = 0;
	QString path;
	int64 size = 0;
	FullMsgId itemId;
	uint64 peerAccessHash = 0;

	std::unique_ptr<DownloadObject> object;
};

struct DownloadingId {
	DownloadObject object;
	DownloadDate started = 0;
	QString path;
	Storage::PendingDownloadId pending;
	Fn<void()> externalCancel;
	int64 ready = 0;
	int64 total = 0;
	bool hiddenByView = false;
	bool done = false;
};

struct PendingDownloadView {
	Storage::PendingDownloadId id;
	QString filename;
	QString accountName;
	QString sourceName;
	QString destination;
	int64 total = 0;
	Storage::DownloadTransferView transfer;
	bool canPause = false;
	bool canResume = false;
	bool canCancel = false;

	[[nodiscard]] bool allows(Storage::DownloadAction action) const {
		switch (action) {
		case Storage::DownloadAction::Pause: return canPause;
		case Storage::DownloadAction::Resume: return canResume;
		case Storage::DownloadAction::Cancel: return canCancel;
		}
		return false;
	}
};

struct DownloadActionResult {
	int accepted = 0;
	int skipped = 0;
	int failed = 0;
};

class DownloadManager final : public base::has_weak_ptr {
public:
	DownloadManager();
	~DownloadManager();

	[[nodiscard]] bool empty() const;

	void trackSession(not_null<Main::Session*> session);
	[[nodiscard]] std::optional<Storage::PendingDownload> reservePending(
		not_null<DocumentData*> document,
		FullMsgId origin,
		const QString &destination,
		bool replace);
	[[nodiscard]] bool resumePending(not_null<DocumentData*> document, FullMsgId origin);
	[[nodiscard]] bool identifyPending(
		not_null<Main::Session*> session, const QUuid &id, QByteArray identity);
	[[nodiscard]] bool updatePending(
		not_null<Main::Session*> session, const QUuid &id, Storage::PendingDownloadState state);
	void completePending(not_null<Main::Session*> session, const QUuid &id);
	void pendingCancelled(
		not_null<Main::Session*> session, const QUuid &id,
		Storage::PersistentFileSink::Result result);
	[[nodiscard]] Storage::PendingDownloadId pendingIdForRow(
		not_null<const HistoryItem*> item) const;
	[[nodiscard]] std::optional<PendingDownloadView> pendingView(
		const Storage::PendingDownloadId &id) const;
	[[nodiscard]] std::vector<PendingDownloadView> pendingViews(uint64 account) const;
	[[nodiscard]] HistoryItem *pendingSourceItem(const Storage::PendingDownloadId &id) const;
	[[nodiscard]] rpl::producer<> pendingViewsChanged() const;
	[[nodiscard]] rpl::producer<> pendingProgressChanged() const;
	[[nodiscard]] rpl::producer<uint64> pendingCleanupDeferred() const;
	[[nodiscard]] bool pendingBatchActive(uint64 account) const;
	void pendingLoaderUpdated(bool transition, Storage::PendingDownloadId id = {});
	void actOnPending(const Storage::PendingDownloadId &id, Storage::DownloadAction action);
	void actOnPendingBatch(
		uint64 account,
		std::vector<Storage::PendingDownloadId> ids,
		Storage::DownloadAction action,
		Fn<void(DownloadActionResult)> done);
	[[nodiscard]] bool pendingRow(not_null<const HistoryItem*> item) const;
	void cancelPendingRow(not_null<const HistoryItem*> item);
	[[nodiscard]] bool persistentQuitPending();
	void retirePending(not_null<Main::Session*> session);
	void itemVisibilitiesUpdated(not_null<Main::Session*> session);

	[[nodiscard]] DownloadDate computeNextStartDate();

	void addLoading(DownloadObject object);
	void addLoaded(
		DownloadObject object,
		const QString &path,
		DownloadDate started);

	struct ExternalLoadingState {
		int64 ready = 0;
		int64 total = 0;
		bool done = false;
	};
	[[nodiscard]] not_null<HistoryItem*> generateExternalItem(
		not_null<DocumentData*> document);
	void addLoadingExternal(
		DownloadObject object,
		const QString &path,
		int64 total,
		Fn<void()> cancel,
		Storage::PendingDownloadId pending = {},
		DownloadDate started = 0);
	void updateLoadingExternal(
		not_null<const HistoryItem*> item,
		int64 ready,
		int64 total);
	[[nodiscard]] auto loadingExternalState(
		not_null<const HistoryItem*> item)
	-> std::optional<ExternalLoadingState>;
	void cancelLoadingExternal(not_null<const HistoryItem*> item);
	void finishLoadingExternal(
		not_null<const HistoryItem*> item,
		const QString &path);
	void removeLoadingExternal(not_null<const HistoryItem*> item);

	void clearIfFinished();
	void deleteFiles(const std::vector<GlobalMsgId> &ids);
	void deleteAll();
	[[nodiscard]] bool loadedHasNonCloudFile() const;

	[[nodiscard]] auto loadingList() const
		-> ranges::any_view<const DownloadingId*, ranges::category::input>;
	[[nodiscard]] rpl::producer<> loadingListChanges() const;
	[[nodiscard]] auto loadingProgressValue() const
		-> rpl::producer<DownloadProgress>;

	[[nodiscard]] bool loadingInProgress(
		Main::Session *onlyInSession = nullptr) const;
	void loadingStopWithConfirmation(
		Fn<void()> callback,
		Main::Session *onlyInSession = nullptr);

	[[nodiscard]] auto loadedList()
		-> ranges::any_view<const DownloadedId*, ranges::category::input>;
	[[nodiscard]] auto loadedAdded() const
		-> rpl::producer<not_null<const DownloadedId*>>;
	[[nodiscard]] auto loadedRemoved() const
		-> rpl::producer<not_null<const HistoryItem*>>;
	[[nodiscard]] rpl::producer<> loadedResolveDone() const;

private:
	struct DeleteFilesDescriptor;
	struct PendingBatch;
	struct PendingRuntime {
		HistoryItem *row = nullptr;
		DocumentData *active = nullptr;
		std::shared_ptr<std::atomic<bool>> stopPublication;
		bool inspected = false;
		bool inspectionFailed = false;
		bool resolve = false;
		bool start = false;
		bool resolving = false;
		bool rowDirty = false;
		bool displayingRate = false;
		int64 inspectedBytes = 0;
		Storage::DownloadIssue issue = Storage::DownloadIssue::None;
	};
	struct SessionData {
		Storage::PendingDownloadList pending;
		base::flat_map<QUuid, PendingRuntime> pendingRuntime;
		bool pendingReady = false;
		std::vector<DownloadedId> downloaded;
		std::vector<DownloadingId> downloading;
		int resolveNeeded = 0;
		int resolveSentRequests = 0;
		int resolveSentTotal = 0;
		rpl::lifetime lifetime;
	};

	void loadPending(not_null<Main::Session*> session);
	[[nodiscard]] Main::Session *pendingSession(uint64 account) const;
	[[nodiscard]] PendingDownloadView makePendingView(
		not_null<Main::Session*> session,
		const Storage::PendingDownload &job,
		const PendingRuntime &runtime) const;
	enum class PendingActionOutcome { Accepted, Skipped, Failed };
	PendingActionOutcome applyPendingAction(
		const Storage::PendingDownloadId &id, Storage::DownloadAction action);
	void continuePendingBatch(uint64 account);
	void flushPendingViews();
	void scanPending();
	void resolvePending();
	void publishRecoveredPending(
		not_null<Main::Session*> session,
		const Storage::PendingDownload &job);
	void requestPending(not_null<Main::Session*> session, const QUuid &id);
	void cancelPending(not_null<Main::Session*> session, const QUuid &id);
	void erasePending(not_null<Main::Session*> session, const QUuid &id);
	void pendingSourceRemoved(SessionData &data, std::vector<DownloadingId>::iterator i);
	void showPendingRow(not_null<Main::Session*> session, const Storage::PendingDownload &job);
	void hidePendingRow(not_null<Main::Session*> session, const QUuid &id);
	[[nodiscard]] bool savePending(
		not_null<Main::Session*> session, Storage::PendingDownloadList changed);
	[[nodiscard]] const Storage::PendingDownload *findPending(
		not_null<Main::Session*> session, const QUuid &id) const;
	[[nodiscard]] bool hasCompletedPending(
		not_null<Main::Session*> session, const Storage::PendingDownload &job) const;
	[[nodiscard]] QUuid pendingRowId(not_null<const HistoryItem*> item) const;
	void check(not_null<const HistoryItem*> item);
	void check(not_null<DocumentData*> document);
	void check(
		SessionData &data,
		std::vector<DownloadingId>::iterator i);
	[[nodiscard]] auto lookupExternal(not_null<const HistoryItem*> item)
		-> std::pair<SessionData*, std::vector<DownloadingId>::iterator>;
	void changed(not_null<const HistoryItem*> item);
	void removed(not_null<const HistoryItem*> item);
	void detach(SessionData &data, DownloadedId &id);
	void untrack(not_null<Main::Session*> session);
	void remove(
		SessionData &data,
		std::vector<DownloadingId>::iterator i);
	void cancel(
		SessionData &data,
		std::vector<DownloadingId>::iterator i);
	void clearLoading();

	[[nodiscard]] SessionData &sessionData(not_null<Main::Session*> session);
	[[nodiscard]] const SessionData &sessionData(
		not_null<Main::Session*> session) const;
	[[nodiscard]] SessionData &sessionData(
		not_null<const HistoryItem*> item);
	[[nodiscard]] SessionData &sessionData(not_null<DocumentData*> document);

	void resolve(not_null<Main::Session*> session, SessionData &data);
	void resolveRequestsFinished(
		not_null<Main::Session*> session,
		SessionData &data);
	void checkFullResolveDone();

	[[nodiscard]] not_null<HistoryItem*> regenerateItem(
		const DownloadObject &previous);
	[[nodiscard]] not_null<HistoryItem*> generateFakeItem(
		not_null<DocumentData*> document);
	[[nodiscard]] not_null<HistoryItem*> generateItem(
		HistoryItem *previousItem,
		DocumentData *document,
		PhotoData *photo);
	void generateEntry(not_null<Main::Session*> session, DownloadedId &id);

	[[nodiscard]] HistoryItem *lookupLoadingItem(
		Main::Session *onlyInSession) const;
	void loadingStop(Main::Session *onlyInSession);

	void finishFilesDelete(DeleteFilesDescriptor &&descriptor);
	void writePostponed(not_null<Main::Session*> session);
	[[nodiscard]] Fn<std::optional<QByteArray>()> serializator(
		not_null<Main::Session*> session) const;
	[[nodiscard]] std::vector<DownloadedId> deserialize(
		not_null<Main::Session*> session) const;

	base::flat_map<not_null<Main::Session*>, SessionData> _sessions;
	base::flat_set<not_null<const HistoryItem*>> _loading;
	base::flat_set<not_null<DocumentData*>> _loadingDocuments;
	base::flat_set<not_null<const HistoryItem*>> _loadingDone;
	base::flat_set<not_null<const HistoryItem*>> _loaded;
	base::flat_set<not_null<HistoryItem*>> _generated;
	base::flat_set<not_null<DocumentData*>> _generatedDocuments;

	TimeId _lastStartedBase = 0;
	int _lastStartedAdded = 0;

	rpl::event_stream<> _loadingListChanges;
	rpl::variable<DownloadProgress> _loadingProgress;

	rpl::event_stream<not_null<const DownloadedId*>> _loadedAdded;
	rpl::event_stream<not_null<const HistoryItem*>> _loadedRemoved;
	rpl::variable<bool> _loadedResolveDone;

	base::Timer _clearLoadingTimer;
	base::Timer _pendingQuitTimer;
	base::Timer _pendingViewTimer;
	base::flat_map<uint64, std::shared_ptr<PendingBatch>> _pendingBatches;
	rpl::event_stream<> _pendingViewsChanged;
	rpl::event_stream<> _pendingProgressChanged;
	rpl::event_stream<uint64> _pendingCleanupDeferred;
	bool _pendingViewsDirty = false;
	bool _pendingScanBusy = false;
	bool _pendingResolveBusy = false;
	bool _pendingQuitting = false;
	int _pendingQuitWorkers = 0;

};

[[nodiscard]] auto MakeDownloadBarProgress()
-> rpl::producer<Ui::DownloadBarProgress>;

[[nodiscard]] rpl::producer<Ui::DownloadBarContent> MakeDownloadBarContent();

} // namespace Data
