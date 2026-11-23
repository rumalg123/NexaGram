/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "history/history_smart_selection_box.h"

#include "base/timer.h"
#include "data/data_messages.h"
#include "data/data_search_controller.h"
#include "data/data_session.h"
#include "history/history.h"
#include "main/main_session.h"
#include "mtproto/sender.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "window/window_session_controller.h"

#include "styles/style_menu_icons.h"

namespace HistoryView {
namespace {

constexpr auto kPageDelay = crl::time(250);
constexpr auto kSecond = crl::time(1000);

class WholeChatScan final {
public:
	WholeChatScan(
		not_null<Ui::GenericBox*> box,
		not_null<History*> history,
		SmartMessageSelection selection,
		Fn<void(SmartMessageSelection)> apply);

	void requestPage();
	void cancel();
	void remove(FullMsgId id);

private:
	void pageReceived(const MTPmessages_Messages &result);
	void pageFailed(const MTP::Error &error);
	void resume();
	void nextHistory();
	void finish();
	void fail();
	void showProgress();

	const not_null<Ui::GenericBox*> _box;
	const not_null<Ui::FlatLabel*> _progress;
	PeerData *_peer = nullptr;
	PeerData *_migrated = nullptr;
	MTP::Sender _api;
	SmartMessageSelection _selection;
	Fn<void(SmartMessageSelection)> _apply;
	base::Timer _timer;
	mtpRequestId _requestId = 0;
	MsgId _offset = 0;
	crl::time _resumeAt = 0;
	int64 _checked = 0;

};

WholeChatScan::WholeChatScan(
	not_null<Ui::GenericBox*> box,
	not_null<History*> history,
	SmartMessageSelection selection,
	Fn<void(SmartMessageSelection)> apply)
: _box(box)
, _progress(box->addRow(object_ptr<Ui::FlatLabel>(box, st::boxLabel)))
, _peer(history->peer.get())
, _api(&history->session().mtp())
, _selection(std::move(selection))
, _apply(std::move(apply))
, _timer([=] { resume(); }) {
	if (const auto migrated = history->migrateFrom()) {
		_migrated = migrated->peer.get();
	}
	showProgress();
}

void WholeChatScan::requestPage() {
	if (!_apply || _requestId) {
		return;
	} else if (_peer->session().frozen()) {
		fail();
		return;
	}
	showProgress();
	_requestId = _api.request(Api::PrepareHistoryRequest(
		_peer,
		_offset,
		Data::LoadDirection::Before
	)).done([=](const MTPmessages_Messages &result) {
		pageReceived(result);
	}).fail([=](const MTP::Error &error) {
		pageFailed(error);
	}).handleFloodErrors().send();
}

void WholeChatScan::cancel() {
	_apply = nullptr;
	_timer.cancel();
	if (const auto request = base::take(_requestId)) {
		_api.request(request).cancel();
	}
}

void WholeChatScan::remove(FullMsgId id) {
	_selection.remove(id);
}

void WholeChatScan::pageReceived(const MTPmessages_Messages &result) {
	_requestId = 0;
	const auto page = Api::ProcessFetchedMessages(_peer, result);
	if (!page.messages) {
		fail();
		return;
	} else if (page.messages->empty()) {
		nextHistory();
		return;
	}
	auto &owner = _peer->owner();
	auto oldest = _offset;
	_selection.resetRegexBudget();
	for (const auto &message : *page.messages) {
		const auto id = IdFromMessage(message);
		if (id <= 0 || (_offset && id >= _offset)) {
			continue;
		}
		++_checked;
		if (!oldest || id < oldest) {
			oldest = id;
		}
		if (PeerFromMessage(message) != _peer->id) {
			continue;
		}
		auto item = owner.message(_peer, id);
		if (!item && _selection.mayMatchUnloaded(message)) {
			item = owner.addNewMessage(id, message, {}, NewMessageType::Existing);
		}
		if (item
			&& item->canBeSelected()
			&& (!_selection.add(item)
				|| _selection.items().size() == MaxSelectedItems)) {
			finish();
			return;
		}
	}
	if (oldest == _offset) {
		fail();
	} else if (oldest == 1) {
		nextHistory();
	} else {
		_offset = oldest;
		showProgress();
		_timer.callOnce(kPageDelay);
	}
}

void WholeChatScan::pageFailed(const MTP::Error &error) {
	_requestId = 0;
	if (!MTP::IsFloodError(error)) {
		fail();
		return;
	}
	const auto &type = error.type();
	auto valid = false;
	const auto seconds = type.mid(type.lastIndexOf('_') + 1).toInt(&valid);
	if (!valid || seconds < 0) {
		fail();
		return;
	}
	_resumeAt = crl::now() + seconds * kSecond + kPageDelay;
	resume();
}

void WholeChatScan::resume() {
	const auto remaining = _resumeAt - crl::now();
	if (remaining <= 0) {
		requestPage();
		return;
	}
	_progress->setText(tr::lng_smart_select_chat_wait(
		tr::now,
		lt_duration,
		tr::lng_seconds(
			tr::now,
			lt_count,
			(remaining + kSecond - 1) / kSecond)));
	_timer.callOnce(std::min(remaining, kSecond));
}

void WholeChatScan::nextHistory() {
	if (!_migrated) {
		finish();
		return;
	}
	_peer = base::take(_migrated);
	_offset = 0;
	showProgress();
	_timer.callOnce(kPageDelay);
}

void WholeChatScan::finish() {
	const auto apply = base::take(_apply);
	auto selection = std::move(_selection);
	_box->closeBox();
	if (apply) {
		apply(std::move(selection));
	}
}

void WholeChatScan::fail() {
	_box->closeBox();
	Ui::Toast::Show(tr::lng_smart_select_chat_failed(tr::now));
}

void WholeChatScan::showProgress() {
	_progress->setText(tr::lng_smart_select_chat_progress(
		tr::now,
		lt_checked,
		QString::number(_checked),
		lt_found,
		QString::number(_selection.items().size()),
		lt_limit,
		QString::number(MaxSelectedItems)));
}

} // namespace

void AddSmartSelectActions(
		not_null<Ui::PopupMenu*> menu,
		not_null<QWidget*> parent,
		not_null<Window::SessionController*> controller,
		const PeerData *sender,
		bool wholeChatAvailable,
		Fn<void(SmartMessageSelection, bool)> apply) {
	const auto addScope = [&](bool wholeChat) {
		auto smart = std::make_unique<Ui::PopupMenu>(
			parent.get(),
			st::popupMenuWithIcons);
		const auto add = [&](SmartMessageFilter filter, const QString &label) {
			smart->addAction(label, [=] {
				apply(SmartMessageSelection(filter, sender), wholeChat);
			}, &st::menuIconSelect);
		};
		add(SmartMessageFilter::Photos,
			tr::lng_context_smart_select_photos(tr::now));
		add(SmartMessageFilter::Videos,
			tr::lng_context_smart_select_videos(tr::now));
		add(SmartMessageFilter::Files,
			tr::lng_context_smart_select_files(tr::now));
		add(SmartMessageFilter::Audio,
			tr::lng_context_smart_select_audio(tr::now));
		add(SmartMessageFilter::Text,
			tr::lng_context_smart_select_text(tr::now));
		if (sender) {
			add(SmartMessageFilter::Sender,
				tr::lng_context_smart_select_sender(tr::now));
		}
		const auto addFilename = [&](bool regex) {
			smart->addAction(
				regex
					? tr::lng_context_smart_select_filename_regex(tr::now)
					: tr::lng_context_smart_select_filename(tr::now),
				[=] {
					controller->show(Box(
						SmartSelectFilenameBox,
						regex,
						[=](SmartMessageSelection selection) {
							apply(std::move(selection), wholeChat);
						}));
				},
				&st::menuIconSelect);
		};
		addFilename(false);
		addFilename(true);
		menu->addAction(
			wholeChat
				? tr::lng_context_smart_select_whole_chat(tr::now)
				: tr::lng_context_smart_select(tr::now),
			std::move(smart),
			&st::menuIconSelect);
	};
	addScope(false);
	if (wholeChatAvailable) {
		addScope(true);
	}
}

void SmartSelectWholeChatBox(
		not_null<Ui::GenericBox*> box,
		not_null<Ui::RpWidget*> origin,
		not_null<History*> history,
		SmartMessageSelection selection,
		Fn<void(SmartMessageSelection)> apply) {
	box->setTitle(tr::lng_context_smart_select_whole_chat());
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	const auto scanLifetime = box->lifetime().make_state<rpl::lifetime>();
	const auto scan = scanLifetime->make_state<WholeChatScan>(
		box,
		history,
		std::move(selection),
		std::move(apply));
	history->owner().itemRemoved() | rpl::on_next(
		[=](not_null<const HistoryItem*> item) {
			scan->remove(item->fullId());
		},
		*scanLifetime);
	box->boxClosing() | rpl::on_next([=] {
		scan->cancel();
	}, *scanLifetime);
	const auto close = [=] {
		if (box->isBoxShown()) {
			box->closeBox();
		}
	};
	QObject::connect(origin.get(), &QObject::destroyed, box.get(), close);
	origin->shownValue() | rpl::skip(1) | rpl::on_next([=](bool shown) {
		if (!shown) {
			close();
		}
	}, *scanLifetime);
	box->showFinishes() | rpl::take(1) | rpl::on_next([=] {
		scan->requestPage();
	}, *scanLifetime);
}

} // namespace HistoryView
