/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "info/downloads/info_downloads_provider.h"

#include "core/application.h"
#include "data/data_document.h"
#include "data/data_download_manager.h"
#include "data/data_file_click_handler.h"
#include "data/data_media_types.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_helpers.h"
#include "info/downloads/info_downloads_status.h"
#include "info/media/info_media_list_section.h"
#include "info/media/info_media_widget.h"
#include "info/info_controller.h"
#include "lang/lang_keys.h"
#include "layout/layout_selection.h"
#include "main/main_account.h"
#include "main/main_app_config.h"
#include "main/main_session.h"
#include "storage/storage_shared_media.h"
#include "ui/text/format_song_document_name.h"
#include "ui/text/format_values.h"
#include "ui/ui_utility.h"

#include "styles/style_overview.h"

namespace Info::Downloads {
namespace {

using namespace Media;

} // namespace

Provider::Provider(not_null<AbstractController*> controller)
: _controller(controller)
, _storiesAddToAlbumId(_controller->storiesAddToAlbumId()) {
	style::PaletteChanged(
	) | rpl::on_next([=] {
		for (auto &layout : _layouts) {
			layout.second.item->invalidateCache();
		}
	}, _lifetime);
}

Type Provider::type() {
	return Type::File;
}

bool Provider::hasSelectRestriction() {
	return false;
}

rpl::producer<bool> Provider::hasSelectRestrictionChanges() {
	return rpl::never<bool>();
}

bool Provider::sectionHasFloatingHeader() {
	return false;
}

QString Provider::sectionTitle(not_null<const BaseLayout*> item) {
	return QString();
}

bool Provider::sectionItemBelongsHere(
		not_null<const BaseLayout*> item,
		not_null<const BaseLayout*> previous) {
	return true;
}

bool Provider::isPossiblyMyItem(not_null<const HistoryItem*> item) {
	return true;
}

std::optional<int> Provider::fullCount() {
	if (!searchMode() && _filter == Storage::DownloadFilter::All) {
		return _fullCount;
	}
	return int(ranges::count_if(_elements, [=](const auto &element) {
		return visible(element);
	}));
}

void Provider::restart() {
}

void Provider::checkPreload(
	QSize viewport,
	not_null<BaseLayout*> topLayout,
	not_null<BaseLayout*> bottomLayout,
	bool preloadTop,
	bool preloadBottom) {
}

void Provider::setSearchQuery(QString query) {
	if (_query == query) {
		return;
	}
	_query = std::move(query);
	_queryWords = TextUtilities::PrepareSearchWords(_query);
	for (auto &element : _elements) {
		element.found = searchMode() && computeIsFound(element);
	}
	_refreshed.fire({});
}

void Provider::setDownloadFilter(Storage::DownloadFilter filter) {
	if (_filter != filter) {
		_filter = filter;
		_refreshed.fire({});
	}
}

void Provider::jumpToMessage(MsgId messageId, Fn<void(FullMsgId)>) {
}

void Provider::refreshViewer() {
	if (_started) {
		return;
	}
	_started = true;
	auto &manager = Core::App().downloadManager();
	rpl::single(rpl::empty) | rpl::then(
		manager.loadingListChanges() | rpl::to_empty
	) | rpl::on_next([=, &manager] {
		auto copy = _downloading;
		for (const auto id : manager.loadingList()) {
			if (!id->done) {
				const auto item = id->object.item;
				if (!copy.remove(item) && !_downloaded.contains(item)) {
					_downloading.emplace(item);
					addElementNow({
						.item = item,
						.started = id->started,
						.path = id->path,
						.pending = id->pending,
					});
					trackItemSession(item);
					refreshPostponed(true);
				}
			}
		}
		for (const auto &item : copy) {
			Assert(!_downloaded.contains(item));
			remove(item);
		}
		if (!_fullCount.has_value()) {
			refreshPostponed(false);
		}
	}, _lifetime);

	manager.pendingViewsChanged(
	) | rpl::on_next([=] {
		if (_filter != Storage::DownloadFilter::All) {
			refreshPostponed(false);
		}
	}, _lifetime);

	for (const auto id : manager.loadedList()) {
		addPostponed(id);
	}

	manager.loadedAdded(
	) | rpl::on_next([=](not_null<const Data::DownloadedId*> entry) {
		addPostponed(entry);
	}, _lifetime);

	manager.loadedRemoved(
	) | rpl::on_next([=](not_null<const HistoryItem*> item) {
		if (!_downloading.contains(item)) {
			remove(item);
		} else {
			_downloaded.remove(item);
			_addPostponed.erase(
				ranges::remove(_addPostponed, item, &Element::item),
				end(_addPostponed));
		}
	}, _lifetime);

	manager.loadedResolveDone(
	) | rpl::on_next([=] {
		if (!_fullCount.has_value()) {
			_fullCount = 0;
		}
	}, _lifetime);

	performAdd();
	performRefresh();
}

void Provider::addPostponed(not_null<const Data::DownloadedId*> entry) {
	Expects(entry->object != nullptr);

	const auto item = entry->object->item;
	trackItemSession(item);
	const auto i = ranges::find(_addPostponed, item, &Element::item);
	if (i != end(_addPostponed)) {
		i->path = entry->path;
		i->started = entry->started;
	} else {
		_addPostponed.push_back({
			.item = item,
			.started = entry->started,
			.path = entry->path,
		});
		if (_addPostponed.size() == 1) {
			Ui::PostponeCall(this, [=] {
				performAdd();
			});
		}
	}
}

void Provider::performAdd() {
	if (_addPostponed.empty()) {
		return;
	}
	for (auto &element : base::take(_addPostponed)) {
		_downloaded.emplace(element.item);
		if (!_downloading.remove(element.item)) {
			addElementNow(std::move(element));
		}
	}
	refreshPostponed(true);
}

void Provider::addElementNow(Element &&element) {
	_elements.push_back(std::move(element));
	auto &added = _elements.back();
	fillSearchIndex(added);
	added.found = searchMode() && computeIsFound(added);
}

void Provider::remove(not_null<const HistoryItem*> item) {
	_addPostponed.erase(
		ranges::remove(_addPostponed, item, &Element::item),
		end(_addPostponed));
	_downloading.remove(item);
	_downloaded.remove(item);
	_elements.erase(ranges::remove(_elements, item, &Element::item), end(_elements));
	if (const auto i = _layouts.find(item); i != end(_layouts)) {
		_layoutRemoved.fire(i->second.item.get());
		// The list widget handles layoutRemoved() synchronously and may
		// refresh its height from there, which can reach refreshViewer()
		// -> refreshRows() -> fillSections() -> clearStaleLayouts() before
		// we get back here, erasing this very entry, so look it up again.
		if (const auto j = _layouts.find(item); j != end(_layouts)) {
			_layouts.erase(j);
		}
	}
	refreshPostponed(false);
}

void Provider::refreshPostponed(bool added) {
	if (added) {
		_postponedRefreshSort = true;
	}
	if (!_postponedRefresh) {
		_postponedRefresh = true;
		Ui::PostponeCall(this, [=] {
			performRefresh();
		});
	}
}

void Provider::performRefresh() {
	if (!_postponedRefresh) {
		return;
	}
	_postponedRefresh = false;
	if (!_elements.empty() || _fullCount.has_value()) {
		_fullCount = _elements.size();
	}
	if (base::take(_postponedRefreshSort)) {
		ranges::sort(_elements, ranges::less(), &Element::started);
	}
	_refreshed.fire({});
}

void Provider::trackItemSession(not_null<const HistoryItem*> item) {
	const auto session = &item->history()->session();
	if (_trackedSessions.contains(session)) {
		return;
	}
	auto &lifetime = _trackedSessions.emplace(session).first->second;

	session->data().itemRemoved(
	) | rpl::on_next([this](auto item) {
		itemRemoved(item);
	}, lifetime);

	session->account().sessionChanges(
	) | rpl::take(1) | rpl::on_next([=] {
		_trackedSessions.remove(session);
	}, lifetime);
}

rpl::producer<> Provider::refreshed() {
	return _refreshed.events();
}

std::vector<ListSection> Provider::fillSections(
		not_null<Overview::Layout::Delegate*> delegate) {
	const auto search = searchMode();

	if (!search) {
		markLayoutsStale();
	}
	const auto guard = gsl::finally([&] { clearStaleLayouts(); });

	if (_elements.empty()) {
		return {};
	}

	auto result = std::vector<ListSection>();
	result.emplace_back(Type::File, sectionDelegate());
	auto &section = result.back();
	for (const auto &element : ranges::views::reverse(_elements)) {
		if (!visible(element)) {
			continue;
		} else if (auto layout = getLayout(element, delegate)) {
			section.addItem(layout);
		}
	}
	if (section.empty()) {
		return {};
	}
	section.finishSection();
	return result;
}

void Provider::markLayoutsStale() {
	for (auto &layout : _layouts) {
		layout.second.stale = true;
	}
}

void Provider::clearStaleLayouts() {
	for (auto i = _layouts.begin(); i != _layouts.end();) {
		if (i->second.stale) {
			_layoutRemoved.fire(i->second.item.get());
			i = _layouts.erase(i);
		} else {
			++i;
		}
	}
}

rpl::producer<not_null<BaseLayout*>> Provider::layoutRemoved() {
	return _layoutRemoved.events();
}

BaseLayout *Provider::lookupLayout(const HistoryItem *item) {
	return nullptr;
}

bool Provider::isMyItem(not_null<const HistoryItem*> item) {
	if (_filter == Storage::DownloadFilter::All && !searchMode()) {
		return _downloading.contains(item) || _downloaded.contains(item);
	}
	const auto i = ranges::find(_elements, item, &Element::item);
	return i != end(_elements) && visible(*i);
}

bool Provider::isAfter(
		not_null<const HistoryItem*> a,
		not_null<const HistoryItem*> b) {
	if (a != b) {
		for (const auto &element : _elements) {
			if (element.item == a) {
				return false;
			} else if (element.item == b) {
				return true;
			}
		}
	}
	return false;
}

bool Provider::searchMode() const {
	return !_query.trimmed().isEmpty();
}

bool Provider::visible(const Element &element) const {
	if (searchMode() && !element.found) {
		return false;
	}
	if (_filter == Storage::DownloadFilter::All) {
		return true;
	}
	const auto pending = Core::App().downloadManager().pendingView(element.pending);
	const auto status = pending ? pending->transfer.status
		: _downloaded.contains(element.item) ? Storage::DownloadStatus::Completed
		: Storage::DownloadStatus::Downloading;
	return Storage::MatchesDownloadFilter(status, _filter);
}

void Provider::fillSearchIndex(Element &element) {
	element.filename = QFileInfo(element.path).fileName();
	auto strings = QStringList(element.filename);
	if (const auto media = element.item->media()) {
		if (const auto document = media->document()) {
			if (element.filename.isEmpty()) {
				element.filename = document->filename();
			}
			strings.append(document->filename());
			strings.append(Ui::Text::FormatDownloadsName(document).text);
		}
	}
	element.words = TextUtilities::PrepareSearchWords(strings.join(' '));
	element.letters.clear();
	for (const auto &word : element.words) {
		element.letters.emplace(word.front());
	}
}

bool Provider::computeIsFound(const Element &element) const {
	if (Storage::MatchesDownloadName(element.filename, _query)) {
		return true;
	}
	if (_queryWords.empty()) {
		return false;
	}

	const auto has = [&](const QString &queryWord) {
		if (!element.letters.contains(queryWord.front())) {
			return false;
		}
		for (const auto &word : element.words) {
			if (word.startsWith(queryWord)) {
				return true;
			}
		}
		return false;
	};
	for (const auto &queryWord : _queryWords) {
		if (!has(queryWord)) {
			return false;
		}
	}
	return true;
}

void Provider::itemRemoved(not_null<const HistoryItem*> item) {
	remove(item);
}

BaseLayout *Provider::getLayout(
		Element element,
		not_null<Overview::Layout::Delegate*> delegate) {
	auto it = _layouts.find(element.item);
	if (it == _layouts.end()) {
		if (auto layout = createLayout(element, delegate)) {
			layout->initDimensions();
			it = _layouts.emplace(element.item, std::move(layout)).first;
		} else {
			return nullptr;
		}
	}
	it->second.stale = false;
	return it->second.item.get();
}

std::unique_ptr<BaseLayout> Provider::createLayout(
		Element element,
		not_null<Overview::Layout::Delegate*> delegate) {
	const auto getFile = [&]() -> DocumentData* {
		if (auto media = element.item->media()) {
			return media->document();
		}
		return nullptr;
	};

	using namespace Overview::Layout;
	const auto &songSt = st::overviewFileLayout;
	if (const auto file = getFile()) {
		auto fields = DocumentFields{
			.document = file,
			.dateOverride = Data::DateFromDownloadDate(element.started),
			.forceFileLayout = true,
		};
		const auto item = element.item;
		auto &manager = Core::App().downloadManager();
		if (!element.pending.job.isNull()) {
			const auto id = element.pending;
			fields.externalLoading = [id]() -> std::optional<DocumentExternalLoading> {
				const auto view = Core::App().downloadManager().pendingView(id);
				if (!view) {
					return std::nullopt;
				}
				const auto &progress = view->transfer;
				const auto middle = u" · "_q;
				auto status = StatusText(progress.status);
				auto rate = QString();
				auto eta = QString();
				const auto issue = IssueText(progress.issue);
				if (!issue.isEmpty()) {
					status += middle + issue;
				} else if (progress.bytesPerSecond > 0) {
					rate = TransferRateText(progress.bytesPerSecond);
					if (const auto seconds = Storage::DownloadEta(
							progress.written, view->total, progress.bytesPerSecond)) {
						eta = tr::lng_downloads_transfer_eta(
							tr::now, lt_duration, Ui::FormatDurationText(*seconds));
					}
				}
				return DocumentExternalLoading{
					.ready = progress.written,
					.total = view->total,
					.status = std::move(status),
					.rate = std::move(rate),
					.eta = std::move(eta),
					.context = view->sourceName.isEmpty()
						? view->accountName
						: view->accountName + middle + view->sourceName,
					.pause = view->canPause,
					.actionable = view->canPause || view->canResume,
					.cancellable = view->canCancel,
				};
			};
			fields.externalCancel = [id] {
				auto &manager = Core::App().downloadManager();
				if (const auto view = manager.pendingView(id)) {
					manager.actOnPending(id, view->canPause
						? Storage::DownloadAction::Pause
						: Storage::DownloadAction::Resume);
				}
			};
			fields.externalDiscard = [id] {
				Data::RequestPendingDownloadCancellation(id);
			};
			fields.externalTooltip = [id] {
				const auto view = Core::App().downloadManager().pendingView(id);
				if (!view) {
					return QString();
				}
				const auto action = view->canPause ? Storage::DownloadAction::Pause
					: Storage::DownloadAction::Resume;
				return ActionText(action, view->transfer.status)
					+ u"\n"_q + view->destination
					+ u"\n"_q + tr::lng_downloads_committed_progress(
						tr::now, lt_size, Ui::FormatSizeText(view->transfer.committed));
			};
		} else if (manager.loadingExternalState(item).has_value()) {
			fields.externalLoading = [item]()
			-> std::optional<DocumentExternalLoading> {
				auto &manager = Core::App().downloadManager();
				const auto state = manager.loadingExternalState(item);
				if (!state || state->done) {
					return std::nullopt;
				}
				return DocumentExternalLoading{
					.ready = state->ready,
					.total = state->total,
				};
			};
			fields.externalCancel = [item] {
				Core::App().downloadManager().cancelLoadingExternal(item);
			};
		}
		return std::make_unique<Document>(
			delegate,
			element.item,
			std::move(fields),
			songSt);
	}
	return nullptr;
}

ListItemSelectionData Provider::computeSelectionData(
		not_null<const HistoryItem*> item,
		TextSelection selection) {
	auto result = ListItemSelectionData(selection);
	const auto pending = Core::App().downloadManager().pendingRow(item);
	result.canDelete = !pending;
	result.canForward = !pending && item->allowsForward()
		&& (&item->history()->session() == &_controller->session());
	return result;
}

void Provider::applyDragSelection(
		ListSelectedMap &selected,
		not_null<const HistoryItem*> fromItem,
		bool skipFrom,
		not_null<const HistoryItem*> tillItem,
		bool skipTill) {
	auto from = ranges::find(_elements, fromItem, &Element::item);
	auto till = ranges::find(_elements, tillItem, &Element::item);
	if (from == end(_elements) || till == end(_elements)) {
		return;
	}
	if (skipFrom) {
		++from;
	}
	if (!skipTill) {
		++till;
	}
	if (from >= till) {
		selected.clear();
		return;
	}
	const auto selectLimit = _storiesAddToAlbumId
		? _controller->session().appConfig().storiesAlbumLimit()
		: MaxSelectedItems;
	auto chosen = base::flat_set<not_null<const HistoryItem*>>();
	chosen.reserve(till - from);
	for (auto i = from; i != till; ++i) {
		if (!visible(*i)) {
			continue;
		}
		const auto item = i->item;
		chosen.emplace(item);
		ChangeItemSelection(
			selected,
			item,
			computeSelectionData(item, FullSelection),
			selectLimit);
	}
	if (selected.size() != chosen.size()) {
		for (auto i = begin(selected); i != end(selected);) {
			if (selected.contains(i->first)) {
				++i;
			} else {
				i = selected.erase(i);
			}
		}
	}
}

bool Provider::allowSaveFileAs(
		not_null<const HistoryItem*> item,
		not_null<DocumentData*> document) {
	return false;
}

QString Provider::showInFolderPath(
		not_null<const HistoryItem*> item,
		not_null<DocumentData*> document) {
	const auto i = ranges::find(_elements, item, &Element::item);
	return (i != end(_elements)) ? i->path : QString();
}

int64 Provider::scrollTopStatePosition(not_null<HistoryItem*> item) {
	const auto i = ranges::find(_elements, item, &Element::item);
	return (i != end(_elements)) ? i->started : 0;
}

HistoryItem *Provider::scrollTopStateItem(ListScrollTopState state) {
	if (!state.position) {
		return _elements.empty() ? nullptr : _elements.back().item.get();
	}
	const auto i = ranges::lower_bound(
		_elements,
		state.position,
		ranges::less(),
		&Element::started);
	return (i != end(_elements))
		? i->item.get()
		: _elements.empty()
		? nullptr
		: _elements.back().item.get();
}

void Provider::saveState(
		not_null<Media::Memento*> memento,
		ListScrollTopState scrollState) {
	if (!_elements.empty() && scrollState.item) {
		memento->setAroundId({ PeerId(), 1 });
		memento->setScrollTopItem(scrollState.item->globalId());
		memento->setScrollTopItemPosition(scrollState.position);
		memento->setScrollTopShift(scrollState.shift);
	}
}

void Provider::restoreState(
		not_null<Media::Memento*> memento,
		Fn<void(ListScrollTopState)> restoreScrollState) {
	if (memento->aroundId() == FullMsgId(PeerId(), 1)) {
		restoreScrollState({
			.position = memento->scrollTopItemPosition(),
			.item = MessageByGlobalId(memento->scrollTopItem()),
			.shift = memento->scrollTopShift(),
		});
		refreshViewer();
	}
}

} // namespace Info::Downloads
