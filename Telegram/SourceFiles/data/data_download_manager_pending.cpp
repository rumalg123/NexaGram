/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "data/data_download_manager.h"

#include "apiwrap.h"
#include "base/call_delayed.h"
#include "base/random.h"
#include "base/unixtime.h"
#include "core/application.h"
#include "core/file_location.h"
#include "data/data_channel.h"
#include "data/data_document.h"
#include "data/data_file_origin.h"
#include "data/data_media_types.h"
#include "data/data_session.h"
#include "data/data_types.h"
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "platform/platform_file_utilities.h"
#include "storage/storage_account.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>

#include <algorithm>
#include <type_traits>
#include <utility>

namespace Data {
namespace {

using Job = Storage::PendingDownload;
using State = Storage::PendingDownloadState;
using Sink = Storage::PersistentFileSink;

FullMsgId SourceId(const Job &job) {
	return { PeerId(job.peer), MsgId(job.message) };
}

bool SameDestination(const QString &a, const QString &b) {
#ifdef Q_OS_WIN
	return a.compare(b, Qt::CaseInsensitive) == 0;
#else
	return a == b;
#endif
}

auto FindJob(auto &jobs, const QUuid &id) {
	return std::find_if(begin(jobs), end(jobs), [&](const Job &job) {
		return job.file.id == id;
	});
}

std::optional<bool> PeerAllowsSaving(
		const MTPVector<MTPChat> &peers,
		PeerId peer) {
	if (peerIsUser(peer)) {
		return true;
	}
	for (const auto &entry : peers.v) {
		auto matches = false;
		const auto allowed = entry.match([&](const auto &data) -> std::optional<bool> {
			using Type = std::decay_t<decltype(data)>;
			if constexpr (std::is_same_v<Type, MTPDchat>
				|| std::is_same_v<Type, MTPDchatEmpty>
				|| std::is_same_v<Type, MTPDchatForbidden>) {
				matches = peerFromChat(data.vid().v) == peer;
			} else {
				matches = peerFromChannel(data.vid().v) == peer;
			}
			if constexpr (std::is_same_v<Type, MTPDchannel>) {
				return data.is_min() ? std::nullopt : std::optional(!data.is_noforwards());
			} else if constexpr (std::is_same_v<Type, MTPDcommunity>) {
				return data.is_min() ? std::nullopt : std::optional(true);
			} else if constexpr (std::is_same_v<Type, MTPDchat>) {
				return !data.is_noforwards() && !data.is_deactivated() && !data.is_left();
			} else {
				return false;
			}
		});
		if (matches) {
			return allowed;
		}
	}
	return std::nullopt;
}

std::optional<QByteArray> SourceReference(
		const MTPmessages_Messages &result,
		const Job &job) {
	return result.match([](const MTPDmessages_messagesNotModified &)
	-> std::optional<QByteArray> {
		return std::nullopt;
	}, [&](const auto &data) -> std::optional<QByteArray> {
		for (const auto &message : data.vmessages().v) {
			if (IdFromMessage(message) != MsgId(job.message)
				|| PeerFromMessage(message) != PeerId(job.peer)
				|| message.type() != mtpc_message) {
				continue;
			}
			const auto &entry = message.c_message();
			const auto media = entry.vmedia();
			if (entry.is_noforwards() || entry.vttl_period()
				|| !media || media->type() != mtpc_messageMediaDocument) {
				continue;
			}
			const auto &file = media->c_messageMediaDocument();
			const auto document = file.vdocument();
			if (file.vttl_seconds() || !document || document->type() != mtpc_document) {
				continue;
			}
			const auto matches = [&](const MTPDocument &candidate) {
				return candidate.type() == mtpc_document
					&& candidate.c_document().vid().v == job.document
					&& candidate.c_document().vsize().v == job.file.size;
			};
			const auto selected = matches(*document)
				? &*document
				: [&]() -> const MTPDocument* {
					const auto variants = file.valt_documents();
					if (!variants) {
						return nullptr;
					}
					const auto &documents = variants->v;
					const auto i = std::find_if(
						documents.cbegin(),
						documents.cend(),
						matches);
					return (i == documents.cend()) ? nullptr : &*i;
				}();
			if (selected) {
				const auto permission = PeerAllowsSaving(data.vchats(), PeerId(job.peer));
				if (!permission) {
					return QByteArray();
				}
				return *permission
					? std::make_optional(selected->c_document().vfile_reference().v)
					: std::nullopt;
			}
		}
		return std::nullopt;
	});
}

DocumentData *PendingSourceDocument(HistoryItem *item, const Job &job) {
	const auto media = item ? item->media() : nullptr;
	const auto primary = media ? media->document() : nullptr;
	if (!primary || primary->id == job.document) {
		return primary;
	}
	const auto &variants = primary->resolveQualities(item);
	const auto i = ranges::find_if(variants, [&](const auto &variant) {
		return variant->id == job.document;
	});
	return (i == end(variants)) ? nullptr : i->get();
}

struct ScanResult {
	Sink::Result result = Sink::Result::Ok;
	int64 ready = 0;
	bool published = false;
	bool completeStage = false;
};

ScanResult InspectJob(const Job &job, bool alreadyCompleted) {
	auto sink = Sink(job.file);
	if (job.state == State::Cancelling) {
		return { .result = sink.purge(job.stagingIdentity) };
	} else if (job.state == State::Finalizing) {
		const auto published = (alreadyCompleted && Storage::PersistentDownloadPathMissing(
				sink.checkpointPath()))
			? sink.matchesPublished(job.stagingIdentity)
			: (sink.inspectPublished(job.stagingIdentity) == Sink::Result::Ok);
		if (published) {
			Platform::File::PostprocessDownloaded(job.file.destination);
			return { .ready = job.file.size, .published = true };
		}
	}
	if (job.stagingIdentity.isEmpty()) {
		return { .result = (Storage::PersistentDownloadPathMissing(sink.stagingPath())
				&& Storage::PersistentDownloadPathMissing(sink.checkpointPath()))
			? Sink::Result::Missing : Sink::Result::Replaced };
	}
	const auto result = sink.open(true, job.stagingIdentity);
	const auto ready = sink.writtenSize();
	return {
		.result = result,
		.ready = ready,
		.completeStage = job.state == State::Finalizing && result == Sink::Result::Ok
			&& ready == job.file.size,
	};
}

} // namespace

const Storage::PendingDownload *DownloadManager::findPending(
		not_null<Main::Session*> session,
		const QUuid &id) const {
	const auto owner = _sessions.find(session);
	if (id.isNull() || owner == end(_sessions)) {
		return nullptr;
	}
	const auto i = FindJob(owner->second.pending, id);
	return i == end(owner->second.pending) ? nullptr : &*i;
}

bool DownloadManager::savePending(
		not_null<Main::Session*> session,
		Storage::PendingDownloadList changed) {
	const auto i = _sessions.find(session);
	if (i == end(_sessions) || !i->second.pendingReady) {
		return false;
	}
	const auto result = session->local().pendingDownloadFile(
		session->uniqueId()).write(changed);
	Storage::TracePendingDownload({}, result ? "registry saved" : "registry save failed");
	if (result) {
		i->second.pending = std::move(changed);
	}
	return result;
}

void DownloadManager::loadPending(not_null<Main::Session*> session) {
	if (!Storage::PendingDownloadsEnabled()) {
		return;
	}
	const auto file = session->local().pendingDownloadFile(session->uniqueId());
	const auto strong = base::make_weak(session.get());
	auto done = crl::guard(this, [=](std::optional<Storage::PendingDownloadList> jobs) {
		const auto session = strong.get();
		if (!session || !_sessions.contains(session) || _pendingQuitting) {
			return;
		}
		Storage::TracePendingDownload({}, jobs ? "registry loaded, account validated"
			: "registry invalid, preserved");
		if (!jobs) {
			return;
		}
		auto &data = sessionData(session);
		data.pendingReady = true;
		data.pending = *jobs;
		for (const auto &job : *jobs) {
			data.pendingRuntime.emplace(job.file.id, PendingRuntime());
			showPendingRow(session, job);
		}
		scanPending();
	});
	crl::async([file, done = std::move(done)]() mutable {
		auto jobs = file.read();
		crl::on_main([done = std::move(done), jobs = std::move(jobs)]() mutable {
			done(std::move(jobs));
		});
	});
}

std::optional<Storage::PendingDownload> DownloadManager::reservePending(
		not_null<DocumentData*> document,
		FullMsgId origin,
		const QString &destination,
		bool replace) {
	const auto session = &document->session();
	const auto item = session->data().message(origin);
	if (!item || _pendingQuitting || !sessionData(session).pendingReady) {
		return std::nullopt;
	}
	const auto clean = QDir::cleanPath(QFileInfo(destination).absoluteFilePath());
	for (const auto &[owner, data] : _sessions) {
		for (const auto &job : data.pending) {
			if (SameDestination(job.file.destination, clean)
				|| (owner == session && (job.document == document->id
					|| SourceId(job) == origin))) {
				Storage::TracePendingDownload(job.file.id, "duplicate job refused");
				return std::nullopt;
			}
		}
	}
	const auto peer = item->history()->peer;
	const auto channel = peer->asChannel();
	const auto started = computeNextStartDate();
	const auto job = Job{
		.file = { QUuid::createUuid(), clean, document->size, replace },
		.peer = origin.peer.value,
		.message = origin.msg.bare,
		.channelAccess = channel ? quint64(channel->accessHash()) : quint64(0),
		.document = document->id,
		.displayName = document->filename().left(512),
		.started = started,
		.updated = started,
	};
	auto changed = sessionData(session).pending;
	changed.push_back(job);
	if (!savePending(session, std::move(changed))) {
		return std::nullopt;
	}
	auto &runtime = sessionData(session).pendingRuntime[job.file.id];
	runtime.active = document;
	runtime.inspected = true;
	showPendingRow(session, job);
	Storage::TracePendingDownload(job.file.id, "job registered");
	return job;
}

bool DownloadManager::identifyPending(
		not_null<Main::Session*> session,
		const QUuid &id,
		QByteArray identity) {
	auto changed = sessionData(session).pending;
	const auto i = FindJob(changed, id);
	if (i == end(changed) || i->state != State::Initializing || identity.isEmpty()
		|| !i->stagingIdentity.isEmpty()) {
		return false;
	}
	i->stagingIdentity = std::move(identity);
	i->updated = computeNextStartDate();
	return savePending(session, std::move(changed));
}

bool DownloadManager::updatePending(
		not_null<Main::Session*> session,
		const QUuid &id,
		State state) {
	const auto jobId = id;
	const auto found = findPending(session, jobId);
	if (!found || !Storage::PendingDownloadTransition(found->state, state)) {
		return false;
	} else if (found->state == state) {
		return true;
	}
	auto changed = sessionData(session).pending;
	const auto i = FindJob(changed, jobId);
	i->state = state;
	i->updated = computeNextStartDate();
	const auto saved = savePending(session, std::move(changed));
	Storage::TracePendingDownload(jobId, saved ? "job state" : "job state write failed", int(state));
	if (saved) {
		pendingLoaderUpdated(true);
	}
	return saved;
}

void DownloadManager::showPendingRow(
		not_null<Main::Session*> session,
		const Job &record) {
	const auto job = record;
	if (job.state == State::Cancelling) {
		return;
	}
	auto &runtime = sessionData(session).pendingRuntime[job.file.id];
	if (runtime.row) {
		return;
	}
	const auto name = QFileInfo(job.file.destination).fileName();
	const auto document = session->data().document(
		base::RandomValue<DocumentId>(),
		0,
		QByteArray(),
		TimeId(job.started / 1000),
		QVector<MTPDocumentAttribute>(1, MTP_documentAttributeFilename(MTP_string(name))),
		u"application/octet-stream"_q,
		InlineImageLocation(),
		ImageWithLocation(),
		ImageWithLocation(),
		false,
		0,
		job.file.size);
	const auto row = generateExternalItem(document);
	runtime.row = row;
	const auto id = Storage::PendingDownloadId{ session->uniqueId(), job.file.id };
	addLoadingExternal({ .item = row, .document = document }, {}, 0,
		[=] { actOnPending(id, Storage::DownloadAction::Cancel); }, id, job.started);
	pendingLoaderUpdated(true);
}

void DownloadManager::hidePendingRow(
		not_null<Main::Session*> session,
		const QUuid &id) {
	const auto i = sessionData(session).pendingRuntime.find(id);
	if (i == end(sessionData(session).pendingRuntime) || !i->second.row) {
		return;
	}
	const auto row = i->second.row;
	i->second.row = nullptr;
	const auto [data, entry] = lookupExternal(row);
	if (data) {
		remove(*data, entry);
	}
	pendingLoaderUpdated(true);
}

QUuid DownloadManager::pendingRowId(not_null<const HistoryItem*> item) const {
	const auto i = _sessions.find(&item->history()->session());
	if (i != end(_sessions)) {
		for (const auto &[id, runtime] : i->second.pendingRuntime) {
			if (runtime.row == item.get()) {
				return id;
			}
		}
	}
	return {};
}

Storage::PendingDownloadId DownloadManager::pendingIdForRow(
		not_null<const HistoryItem*> item) const {
	const auto job = pendingRowId(item);
	return job.isNull()
		? Storage::PendingDownloadId()
		: Storage::PendingDownloadId{ item->history()->session().uniqueId(), job };
}

bool DownloadManager::pendingRow(not_null<const HistoryItem*> item) const {
	return !pendingRowId(item).isNull();
}

void DownloadManager::cancelPendingRow(not_null<const HistoryItem*> item) {
	actOnPending(pendingIdForRow(item), Storage::DownloadAction::Cancel);
}

Main::Session *DownloadManager::pendingSession(uint64 account) const {
	for (const auto &[session, data] : _sessions) {
		if (session->uniqueId() == account) {
			return session.get();
		}
	}
	return nullptr;
}

PendingDownloadView DownloadManager::makePendingView(
		not_null<Main::Session*> session,
		const Job &job,
		const PendingRuntime &runtime) const {
	using Status = Storage::DownloadStatus;
	using Issue = Storage::DownloadIssue;
	using Action = Storage::DownloadAction;
	auto result = PendingDownloadView{
		.id = { session->uniqueId(), job.file.id },
		.filename = QFileInfo(job.file.destination).fileName(),
		.accountName = session->user()->name(),
		.destination = job.file.destination,
		.total = job.file.size,
	};
	if (const auto source = session->data().message(SourceId(job))) {
		result.sourceName = source->history()->peer->name();
	}
	if (runtime.active && runtime.active->persistentDownloadId() == job.file.id) {
		if (const auto view = runtime.active->persistentTransferView()) {
			result.transfer = *view;
		}
	} else {
		result.transfer.written = result.transfer.committed = runtime.inspectedBytes;
		switch (job.state) {
		case State::Initializing:
			result.transfer.status = runtime.active ? Status::Preparing : Status::Interrupted;
			break;
		case State::Downloading:
		case State::Interrupted: result.transfer.status = Status::Interrupted; break;
		case State::Paused: result.transfer.status = Status::Paused; break;
		case State::Retryable: result.transfer.status = Status::Failed; break;
		case State::Unavailable: result.transfer.status = Status::Unavailable; break;
		case State::Finalizing: result.transfer.status = Status::Failed; break;
		case State::Cancelling: result.transfer.status = Status::Cancelling; break;
		}
	}
	if (runtime.issue != Issue::None) {
		result.transfer.issue = runtime.issue;
	}
	if (job.state == State::Cancelling) {
		result.transfer.status = Status::Cancelling;
	} else if (job.state == State::Unavailable) {
		result.transfer.status = Status::Unavailable;
	} else if (runtime.stopPublication) {
		result.transfer.status = Status::Finalizing;
	} else if (!runtime.inspected) {
		result.transfer.status = Status::Verifying;
	} else if (runtime.start && (runtime.resolving || runtime.resolve)) {
		result.transfer.status = runtime.resolving ? Status::Resolving : Status::Resuming;
	} else if (runtime.issue == Issue::SourceLookup) {
		result.transfer.status = Status::Waiting;
	} else if (runtime.inspectionFailed) {
		result.transfer.status = Status::Failed;
	}
	const auto status = result.transfer.status;
	result.canPause = Storage::AllowsDownloadAction(true, status, Action::Pause)
		&& (status == Status::Downloading
			|| Storage::PendingDownloadTransition(job.state, State::Paused));
	result.canResume = Storage::AllowsDownloadAction(true, status, Action::Resume);
	result.canCancel = Storage::AllowsDownloadAction(true, status, Action::Cancel)
		&& (job.state != State::Finalizing || runtime.inspected);
	return result;
}

std::optional<PendingDownloadView> DownloadManager::pendingView(
		const Storage::PendingDownloadId &id) const {
	if (id.job.isNull()) {
		return std::nullopt;
	}
	const auto owner = pendingSession(id.account);
	const auto job = owner ? findPending(owner, id.job) : nullptr;
	if (!job) {
		return std::nullopt;
	}
	const auto &runtime = sessionData(owner).pendingRuntime;
	const auto i = runtime.find(id.job);
	return (i != end(runtime) && i->second.row)
		? std::make_optional(makePendingView(owner, *job, i->second))
		: std::nullopt;
}

std::vector<PendingDownloadView> DownloadManager::pendingViews(uint64 account) const {
	auto result = std::vector<PendingDownloadView>();
	if (const auto session = pendingSession(account)) {
		const auto &data = sessionData(session);
		result.reserve(data.pending.size());
		for (const auto &job : data.pending) {
			const auto i = data.pendingRuntime.find(job.file.id);
			if (i != end(data.pendingRuntime) && i->second.row) {
				result.push_back(makePendingView(session, job, i->second));
			}
		}
	}
	return result;
}

HistoryItem *DownloadManager::pendingSourceItem(const Storage::PendingDownloadId &id) const {
	const auto session = pendingSession(id.account);
	const auto job = session ? findPending(session, id.job) : nullptr;
	if (!job || job->state == State::Unavailable || job->state == State::Cancelling) {
		return nullptr;
	}
	const auto origin = SourceId(*job);
	const auto item = session->data().message(origin);
	const auto document = PendingSourceDocument(item, *job);
	return (document && document->size == job->file.size
		&& document->persistentSourceAllowed(origin)) ? item : nullptr;
}

rpl::producer<> DownloadManager::pendingViewsChanged() const {
	return _pendingViewsChanged.events();
}

rpl::producer<> DownloadManager::pendingProgressChanged() const {
	return _pendingProgressChanged.events();
}

rpl::producer<uint64> DownloadManager::pendingCleanupDeferred() const {
	return _pendingCleanupDeferred.events();
}

void DownloadManager::pendingLoaderUpdated(
		bool transition,
		Storage::PendingDownloadId id) {
	_pendingViewsDirty = _pendingViewsDirty || transition;
	if (!id.job.isNull()) {
		if (const auto session = pendingSession(id.account)) {
			auto &runtime = sessionData(session).pendingRuntime;
			if (const auto i = runtime.find(id.job); i != runtime.end()) {
				i->second.rowDirty = true;
			}
		}
	}
	if (!_pendingViewTimer.isActive()) {
		_pendingViewTimer.callOnce(crl::time(350));
	}
}

void DownloadManager::flushPendingViews() {
	const auto transitioned = base::take(_pendingViewsDirty);
	auto hasRate = false;
	for (auto &[session, data] : _sessions) {
		for (auto &[id, runtime] : data.pendingRuntime) {
			const auto view = runtime.active
				? runtime.active->persistentTransferView()
				: std::nullopt;
			const auto rate = view && view->bytesPerSecond > 0;
			const auto dirty = base::take(runtime.rowDirty);
			const auto changedRate = std::exchange(runtime.displayingRate, rate) != rate;
			if (runtime.row && (dirty || transitioned || changedRate)) {
				session->data().requestItemRepaint(runtime.row);
			}
			hasRate = hasRate || rate;
		}
	}
	if (transitioned) {
		_pendingViewsChanged.fire({});
		resolvePending();
	}
	_pendingProgressChanged.fire({});
	if (hasRate && !_pendingViewTimer.isActive()) {
		_pendingViewTimer.callOnce(crl::time(350));
	}
}

struct DownloadManager::PendingBatch {
	base::weak_ptr<Main::Session> owner;
	std::vector<Storage::PendingDownloadId> ids;
	Storage::DownloadAction action;
	Fn<void(DownloadActionResult)> done;
	DownloadActionResult result;
	size_t next = 0;
};

bool DownloadManager::pendingBatchActive(uint64 account) const {
	return _pendingBatches.contains(account);
}

void DownloadManager::actOnPendingBatch(
		uint64 account,
		std::vector<Storage::PendingDownloadId> ids,
		Storage::DownloadAction action,
		Fn<void(DownloadActionResult)> done) {
	const auto session = pendingSession(account);
	const auto count = int(ids.size());
	if (!session || _pendingQuitting || pendingBatchActive(account)) {
		Storage::TracePendingDownload({}, "bulk scope unavailable or busy", count);
		done({ .skipped = count });
		return;
	}
	auto scoped = Storage::ScopedDownloadIds(account, ids);
	const auto eligible = int(scoped.size());
	Storage::TracePendingDownload({}, "bulk current account started action", int(action));
	Storage::TracePendingDownload({}, "bulk snapshot size", eligible);
	_pendingBatches.emplace(account, std::make_shared<PendingBatch>(PendingBatch{
		.owner = base::make_weak(session),
		.ids = std::move(scoped),
		.action = action,
		.done = std::move(done),
		.result = { .skipped = count - eligible },
	}));
	pendingLoaderUpdated(true);
	base::call_delayed(crl::time(1), this, [=] { continuePendingBatch(account); });
}

void DownloadManager::continuePendingBatch(uint64 account) {
	const auto i = _pendingBatches.find(account);
	if (i == end(_pendingBatches)) {
		return;
	}
	const auto batch = i->second;
	const auto owner = batch->owner.get();
	if (!owner || pendingSession(account) != owner || _pendingQuitting) {
		batch->result.skipped += int(batch->ids.size() - batch->next);
		batch->next = batch->ids.size();
		Storage::TracePendingDownload({}, "bulk owner changed or closing");
	}
	if (batch->next == batch->ids.size()) {
		_pendingBatches.remove(account);
		Storage::TracePendingDownload({}, "bulk accepted", batch->result.accepted);
		Storage::TracePendingDownload({}, "bulk skipped", batch->result.skipped);
		Storage::TracePendingDownload({}, "bulk failed", batch->result.failed);
		pendingLoaderUpdated(true);
		batch->done(batch->result);
		return;
	}
	switch (applyPendingAction(batch->ids[batch->next++], batch->action)) {
	case PendingActionOutcome::Accepted: ++batch->result.accepted; break;
	case PendingActionOutcome::Skipped: ++batch->result.skipped; break;
	case PendingActionOutcome::Failed: ++batch->result.failed; break;
	}
	base::call_delayed(crl::time(1), this, [=] { continuePendingBatch(account); });
}

void DownloadManager::actOnPending(
		const Storage::PendingDownloadId &id,
		Storage::DownloadAction action) {
	applyPendingAction(id, action);
}

DownloadManager::PendingActionOutcome DownloadManager::applyPendingAction(
		const Storage::PendingDownloadId &id,
		Storage::DownloadAction action) {
	using Action = Storage::DownloadAction;
	const auto session = pendingSession(id.account);
	const auto view = session ? pendingView(id) : std::nullopt;
	if (_pendingQuitting || !view || !view->allows(action)) {
		Storage::TracePendingDownload(id.job, "job action skipped, stale or ineligible", int(action));
		return PendingActionOutcome::Skipped;
	}
	auto &runtime = sessionData(session).pendingRuntime[id.job];
	auto accepted = false;
	if (action == Action::Resume) {
		requestPending(session, id.job);
		accepted = true;
	} else if (action == Action::Cancel) {
		cancelPending(session, id.job);
		const auto current = findPending(session, id.job);
		accepted = !current || current->state == State::Cancelling;
	} else if (view->transfer.status == Storage::DownloadStatus::Downloading) {
		if (const auto document = runtime.active) {
			document->pauseDownload();
			const auto current = findPending(session, id.job);
			accepted = current && current->state == State::Paused;
		}
	} else if (updatePending(session, id.job, State::Paused)) {
		runtime.start = runtime.resolve = false;
		runtime.issue = Storage::DownloadIssue::None;
		accepted = true;
	}
	Storage::TracePendingDownload(id.job,
		accepted ? "job action accepted" : "job action failed",
		int(action));
	if (findPending(session, id.job)) {
		sessionData(session).pendingRuntime[id.job].issue = accepted
			? Storage::DownloadIssue::None
			: Storage::DownloadIssue::Checkpoint;
	}
	pendingLoaderUpdated(true);
	return accepted ? PendingActionOutcome::Accepted : PendingActionOutcome::Failed;
}

bool DownloadManager::resumePending(not_null<DocumentData*> document, FullMsgId origin) {
	const auto session = &document->session();
	const auto &data = sessionData(session);
	for (const auto &job : data.pending) {
		if (job.document == document->id && SourceId(job) == origin
			&& job.state != State::Cancelling) {
			requestPending(session, job.file.id);
			return true;
		}
	}
	return false;
}

bool DownloadManager::hasCompletedPending(
		not_null<Main::Session*> session,
		const Job &job) const {
	const auto &list = sessionData(session).downloaded;
	return std::any_of(begin(list), end(list), [&](const DownloadedId &done) {
		return done.download.objectId == job.document
			&& done.download.type == DownloadType::Document
			&& done.path == job.file.destination
			&& done.started == job.started
			&& done.size == job.file.size
			&& done.itemId == SourceId(job);
	});
}

void DownloadManager::scanPending() {
	if (_pendingScanBusy || _pendingQuitting) {
		return;
	}
	for (auto &[session, data] : _sessions) {
		for (const auto &job : data.pending) {
			const auto i = data.pendingRuntime.find(job.file.id);
			if (i == end(data.pendingRuntime) || i->second.inspected) {
				continue;
			}
			_pendingScanBusy = true;
			const auto weak = base::make_weak(session.get());
			const auto alreadyCompleted = hasCompletedPending(session, job);
			auto done = crl::guard(this, [=, id = job.file.id, state = job.state](ScanResult scan) {
				_pendingScanBusy = false;
				const auto session = weak.get();
				const auto found = session ? findPending(session, id) : nullptr;
				if (!_pendingQuitting && found) {
					const auto current = *found;
					auto &runtime = sessionData(session).pendingRuntime[id];
					runtime.inspected = true;
					Storage::TracePendingDownload(id, "staging recovery", int(scan.result));
					if (current.state == State::Cancelling) {
						if (state == State::Cancelling) {
							pendingCancelled(session, id, scan.result);
						} else {
							runtime.inspected = false;
						}
					} else if (scan.published && current.state == State::Finalizing) {
						completePending(session, id);
					} else if (scan.completeStage && current.state == State::Finalizing) {
						publishRecoveredPending(session, current);
						return;
					} else {
						const auto currentState = current.state;
						const auto interrupted = currentState == State::Downloading
							|| currentState == State::Interrupted;
						if (currentState == State::Downloading) {
							const auto saved = updatePending(session, id, State::Interrupted);
							Storage::TracePendingDownload(id, "interruption recorded", int(saved));
						}
						const auto automatic = scan.result == Sink::Result::Ok
							&& Storage::PendingDownloadsAutoResume()
							&& (interrupted || currentState == State::Finalizing);
						runtime.resolve = runtime.resolve || automatic;
						runtime.start = runtime.start || automatic;
						runtime.inspectionFailed = scan.result != Sink::Result::Ok
							&& (currentState != State::Initializing || scan.result != Sink::Result::Missing);
						runtime.inspectedBytes = scan.ready;
						runtime.issue = runtime.inspectionFailed
							? Storage::DownloadIssueFromSink(scan.result)
							: Storage::DownloadIssue::None;
						Storage::TracePendingDownload(id, "startup resume decision", int(runtime.start));
					}
				}
				pendingLoaderUpdated(true);
				scanPending();
				resolvePending();
			});
			crl::async([job, alreadyCompleted, done = std::move(done)]() mutable {
				const auto result = InspectJob(job, alreadyCompleted);
				crl::on_main([done = std::move(done), result]() mutable { done(result); });
			});
			return;
		}
	}
}

void DownloadManager::publishRecoveredPending(
		not_null<Main::Session*> session,
		const Job &job) {
	_pendingScanBusy = true;
	const auto weak = base::make_weak(session.get());
	auto &runtime = sessionData(session).pendingRuntime[job.file.id];
	const auto stop = std::make_shared<std::atomic<bool>>(false);
	runtime.stopPublication = stop;
	runtime.resolve = runtime.start = false;
	Storage::TracePendingDownload(job.file.id, "retrying committed publication");
	pendingLoaderUpdated(true);
	auto done = crl::guard(this, [=](Sink::Result result) {
		_pendingScanBusy = false;
		const auto session = weak.get();
		const auto current = session ? findPending(session, job.file.id) : nullptr;
		if (current && current->state == State::Finalizing) {
			auto &runtime = sessionData(session).pendingRuntime[job.file.id];
			runtime.stopPublication.reset();
			runtime.inspectionFailed = (result != Sink::Result::Ok);
			runtime.issue = Storage::DownloadIssueFromSink(result);
			if (result == Sink::Result::Ok) {
				completePending(session, job.file.id);
			} else {
				runtime.inspectedBytes = job.file.size;
			}
		}
		Storage::TracePendingDownload(job.file.id, "recovered publication", int(result));
		pendingLoaderUpdated(true);
		scanPending();
		resolvePending();
	});
	crl::async([stop, job, done = std::move(done)]() mutable {
		auto sink = Sink(job.file);
		auto result = sink.open(true, job.stagingIdentity);
		if (result == Sink::Result::Ok) {
			result = stop->load() ? Sink::Result::PublishFailed : sink.publish(true);
		}
		if (result == Sink::Result::Ok) {
			Platform::File::PostprocessDownloaded(job.file.destination);
		}
		crl::on_main([done = std::move(done), result]() mutable { done(result); });
	});
}

void DownloadManager::requestPending(
		not_null<Main::Session*> session,
		const QUuid &id) {
	const auto job = findPending(session, id);
	const auto view = job ? pendingView({ session->uniqueId(), id }) : std::nullopt;
	if (_pendingQuitting || !view || !view->canResume) {
		return;
	}
	auto &runtime = sessionData(session).pendingRuntime[id];
	if (runtime.stopPublication) {
		return;
	}
	runtime.resolve = runtime.start = true;
	runtime.issue = Storage::DownloadIssue::None;
	runtime.inspectionFailed = false;
	pendingLoaderUpdated(true);
	if (job->state == State::Finalizing && !runtime.active) {
		runtime.inspected = false;
		scanPending();
	}
	resolvePending();
}

void DownloadManager::resolvePending() {
	if (_pendingResolveBusy || _pendingQuitting) {
		return;
	}
	auto opening = 0;
	for (const auto &[session, data] : _sessions) {
		for (const auto &[id, runtime] : data.pendingRuntime) {
			const auto view = runtime.active
				? runtime.active->persistentTransferView() : std::nullopt;
			if (view && (view->status == Storage::DownloadStatus::Preparing
				|| view->status == Storage::DownloadStatus::Verifying)
				&& ++opening == 2) {
				return;
			}
		}
	}
	for (auto &[session, data] : _sessions) {
		for (const auto &job : data.pending) {
			const auto i = data.pendingRuntime.find(job.file.id);
			if (i == end(data.pendingRuntime) || !i->second.inspected
				|| !i->second.resolve || job.state == State::Cancelling
				|| job.state == State::Unavailable) {
				continue;
			}
			i->second.resolve = false;
			i->second.resolving = true;
			pendingLoaderUpdated(true);
			_pendingResolveBusy = true;
			const auto weak = base::make_weak(session.get());
			const auto complete = std::make_shared<bool>(false);
			const auto resolved = crl::guard(this, [=](
					const MTPmessages_Messages *result, bool unavailable) {
				if (std::exchange(*complete, true)) {
					return;
				}
				const auto finish = [&] {
					_pendingResolveBusy = false;
					pendingLoaderUpdated(true);
					resolvePending();
				};
				const auto session = weak.get();
				const auto available = [&] {
					const auto found = weak.get() ? findPending(session, job.file.id) : nullptr;
					return (_pendingQuitting || !found || found->state == State::Cancelling
						|| found->state == State::Unavailable) ? nullptr : found;
				};
				if (!available()) {
					finish();
					return;
				}
				auto &runtime = sessionData(session).pendingRuntime[job.file.id];
				runtime.resolve = false;
				runtime.resolving = false;
				if (!std::exchange(runtime.start, false)) {
					finish();
					return;
				}
				const auto origin = SourceId(job);
				const auto channelId = peerToChannel(origin.peer);
				const auto reference = result ? SourceReference(*result, job) : std::nullopt;
				if (result && reference) {
					session->data().processExistingMessages(
						channelId ? session->data().channel(channelId).get() : nullptr,
						*result);
				}
				const auto found = available();
				if (!found) {
					finish();
					return;
				}
				const auto current = *found;
				if (result && reference) {
					const auto item = session->data().message(origin);
					const auto document = PendingSourceDocument(item, job);
					if (!document || document->id != job.document || document->size != job.file.size
						|| !document->persistentSourceAllowed(origin)) {
						unavailable = true;
					} else if (!reference->isEmpty()) {
						document->refreshFileReference(*reference);
						Storage::TracePendingDownload(job.file.id, "source and permissions validated");
						if (!document->loading() && document->restorePendingDownload(current)) {
							if (available()) {
								sessionData(session).pendingRuntime[job.file.id].active = document;
								addLoading({ .item = item, .document = document });
							}
						} else if (sessionData(session).pendingRuntime[job.file.id].active == document) {
							document->resumeValidatedPendingDownload();
						}
					}
				} else if (result && result->type() != mtpc_messages_messagesNotModified) {
					unavailable = true;
				}
				if (!available()) {
					finish();
					return;
				}
				if (unavailable) {
					const auto saved = current.state != State::Finalizing
						&& updatePending(session, job.file.id, State::Unavailable);
					Storage::TracePendingDownload(job.file.id, "source unavailable", int(saved));
					auto &data = sessionData(session);
					if (const auto active = data.pendingRuntime[job.file.id].active; saved && active) {
						const auto row = std::find_if(begin(data.downloading), end(data.downloading),
							[&](const DownloadingId &entry) {
								return !entry.externalCancel && entry.object.document == active;
							});
						if (row != end(data.downloading)) {
							pendingSourceRemoved(data, row);
						}
					}
				} else if (!reference || reference->isEmpty()) {
					Storage::TracePendingDownload(job.file.id, "source retryable");
					sessionData(session).pendingRuntime[job.file.id].issue = Storage::DownloadIssue::SourceLookup;
					if (current.state == State::Interrupted) {
						const auto saved = updatePending(session, job.file.id, State::Retryable);
						Storage::TracePendingDownload(job.file.id, "source retry recorded", int(saved));
					}
				}
				finish();
			});
			const auto done = [resolved](const MTPmessages_Messages &result) {
				resolved(&result, false);
			};
			const auto fail = [resolved](const MTP::Error &error) {
				resolved(nullptr, error.type() == u"CHANNEL_PRIVATE"_q
					|| error.type() == u"CHANNEL_INVALID"_q
					|| error.type() == u"MESSAGE_ID_INVALID"_q);
			};
			const auto source = SourceId(job);
			const auto messages = MTP_vector<MTPInputMessage>(
				1, MTP_inputMessageID(MTP_int(int(source.msg.bare))));
			const auto channel = peerToChannel(source.peer);
			const auto loaded = channel ? session->data().channelLoaded(channel) : nullptr;
			const auto access = loaded && loaded->accessHash()
				? loaded->accessHash() : job.channelAccess;
			Storage::TracePendingDownload(job.file.id, "resolve source");
			const auto request = channel
				? session->api().request(MTPchannels_GetMessages(
					MTP_inputChannel(MTP_long(channel.bare), MTP_long(access)),
					messages)).done(done).fail(fail).send()
				: session->api().request(MTPmessages_GetMessages(messages))
					.done(done).fail(fail).send();
			base::call_delayed(15 * crl::time(1000), this, [=] {
				if (!*complete) {
					if (const auto session = weak.get()) {
						session->api().request(request).cancel();
					}
					resolved(nullptr, false);
				}
			});
			return;
		}
	}
}

void DownloadManager::erasePending(
		not_null<Main::Session*> session,
		const QUuid &id) {
	auto changed = sessionData(session).pending;
	const auto i = FindJob(changed, id);
	if (i == end(changed)) {
		return;
	}
	changed.erase(i);
	if (savePending(session, std::move(changed))) {
		hidePendingRow(session, id);
		sessionData(session).pendingRuntime.remove(id);
		Storage::TracePendingDownload(id, "job retired");
	}
}

void DownloadManager::cancelPending(not_null<Main::Session*> session, const QUuid &id) {
	const auto jobId = id;
	const auto job = findPending(session, jobId);
	if (!job) {
		return;
	}
	auto &runtime = sessionData(session).pendingRuntime[jobId];
	if (runtime.active) {
		runtime.active->cancel();
		return;
	}
	if ((job->state != State::Finalizing || (runtime.inspected && !runtime.stopPublication))
		&& updatePending(session, jobId, State::Cancelling)) {
		runtime.inspected = false;
		scanPending();
	}
}

void DownloadManager::pendingSourceRemoved(
		SessionData &data,
		std::vector<DownloadingId>::iterator i) {
	const auto document = i->object.document;
	const auto session = &document->session();
	const auto id = document->persistentDownloadId();
	remove(data, i);
	const auto job = findPending(session, id);
	if (!job || job->state == State::Finalizing || job->state == State::Cancelling) {
		return;
	}
	const auto changed = updatePending(session, id, State::Unavailable);
	Storage::TracePendingDownload(id, "source removed", int(changed));
	document->stopPendingForUnavailableSource();
	if (const auto updated = findPending(session, id)) {
		showPendingRow(session, *updated);
	}
}

void DownloadManager::pendingCancelled(
		not_null<Main::Session*> session,
		const QUuid &id,
		Sink::Result result) {
	const auto current = findPending(session, id);
	if (!current || current->state != State::Cancelling) {
		return;
	}
	auto &runtime = sessionData(session).pendingRuntime[id];
	runtime.active = nullptr;
	runtime.inspected = true;
	hidePendingRow(session, id);
	if (result == Sink::Result::Ok) {
		erasePending(session, id);
	} else {
		Storage::TracePendingDownload(id, "cancel cleanup deferred", int(result));
		_pendingCleanupDeferred.fire(session->uniqueId());
	}
}

void DownloadManager::completePending(not_null<Main::Session*> session, const QUuid &id) {
	const auto found = findPending(session, id);
	if (!found || found->state != State::Finalizing) {
		return;
	}
	const auto job = *found;
	auto &data = sessionData(session);
	data.pendingRuntime[id].active = nullptr;
	if (!hasCompletedPending(session, job)) {
		data.downloaded.push_back({
			.download = { job.document, DownloadType::Document },
			.started = job.started,
			.path = job.file.destination,
			.size = job.file.size,
			.itemId = SourceId(job),
			.peerAccessHash = job.channelAccess,
		});
		auto &done = data.downloaded.back();
		const auto item = session->data().message(done.itemId);
		const auto document = PendingSourceDocument(item, job);
		if (document && document->id == job.document && document->size == job.file.size
			&& document->persistentSourceAllowed(done.itemId)) {
			const auto location = Core::FileLocation(done.path);
			document->setLocation(location);
			session->local().writeFileLocation(document->mediaKey(), location);
			done.object = std::make_unique<DownloadObject>(DownloadObject{ item, document });
			_loaded.emplace(item);
		} else {
			generateEntry(session, done);
		}
		_loadedAdded.fire(&done);
	}
	hidePendingRow(session, id);
	writePostponed(session);
	if (!session->local().flushDownloads()) {
		Storage::TracePendingDownload(id, "completed history commit failed");
		return;
	}
	Storage::TracePendingDownload(id, "completed history durable");
	const auto weak = base::make_weak(session.get());
	auto done = crl::guard(this, [=](bool cleaned) {
		if (const auto session = weak.get(); session && cleaned) {
			erasePending(session, id);
		}
		Storage::TracePendingDownload(id, "publication cleanup", int(cleaned));
	});
	crl::async([job, done = std::move(done)]() mutable {
		auto sink = Sink(job.file);
		const auto cleaned = sink.purge(job.stagingIdentity) == Sink::Result::Ok;
		crl::on_main([done = std::move(done), cleaned]() mutable { done(cleaned); });
	});
}

bool DownloadManager::persistentQuitPending() {
	if (_pendingQuitting) {
		return _pendingQuitWorkers > 0;
	}
	_pendingQuitting = true;
	auto active = std::vector<DocumentData*>();
	for (const auto &[session, data] : _sessions) {
		for (const auto &[id, runtime] : data.pendingRuntime) {
			if (runtime.active && runtime.active->canPauseDownload()) {
				active.push_back(runtime.active);
			}
		}
	}
	if (active.empty()) {
		return false;
	}
	Storage::TracePendingDownload({}, "shutdown suspending jobs", int(active.size()));
	_pendingQuitWorkers = int(active.size());
	_pendingQuitTimer.setCallback([=] {
		_pendingQuitWorkers = 0;
		Storage::TracePendingDownload({}, "shutdown checkpoint timeout");
		Core::App().quitPreventFinished();
	});
	_pendingQuitTimer.callOnce(crl::time(1250));
	for (const auto document : active) {
		document->suspendPendingForQuit(crl::guard(this, [=] {
			if (_pendingQuitWorkers > 0 && !--_pendingQuitWorkers) {
				_pendingQuitTimer.cancel();
				crl::on_main(this, [] { Core::App().quitPreventFinished(); });
			}
		}));
	}
	return _pendingQuitWorkers > 0;
}

void DownloadManager::retirePending(not_null<Main::Session*> session) {
	const auto file = session->local().pendingDownloadFile(session->uniqueId());
	auto &data = sessionData(session);
	auto jobs = data.pendingReady
		? std::make_optional(data.pending)
		: file.read();
	if (!jobs || jobs->empty()) {
		return;
	}
	const auto active = data.pendingRuntime;
	for (auto &job : *jobs) {
		job.state = State::Cancelling;
	}
	data.pendingReady = true;
	if (!savePending(session, *jobs)) {
		Storage::TracePendingDownload({}, "logout retirement write failed");
		return;
	}
	for (const auto &[id, runtime] : active) {
		if (runtime.stopPublication) {
			runtime.stopPublication->store(true);
		}
		if (runtime.active) {
			runtime.active->cancel();
		}
	}
	Storage::TracePendingDownload({}, "logout retirement durable");
	std::erase_if(*jobs, [&](const Job &job) {
		const auto i = active.find(job.file.id);
		return i != end(active) && (i->second.active || i->second.stopPublication);
	});
	crl::async([jobs = std::move(*jobs)] {
		for (const auto &job : jobs) {
			auto sink = Sink(job.file);
			const auto result = sink.purge(job.stagingIdentity);
			Storage::TracePendingDownload(job.file.id, "logout cleanup", int(result));
		}
	});
}

} // namespace Data
