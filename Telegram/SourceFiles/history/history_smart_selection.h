/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "config.h"
#include "data/data_document.h"
#include "data/data_media_types.h"
#include "data/data_peer.h"
#include "history/history_item.h"
#include "lang/lang_keys.h"
#include "ui/toast/toast.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QRegularExpression>
#include <QtCore/QStringList>

namespace HistoryView {

enum class SmartMessageFilter : uchar {
	Photos,
	Videos,
	Files,
	Audio,
	Text,
	Sender,
	Filename,
	FilenameRegex,
};

class SmartMessageSelection {
public:
	explicit SmartMessageSelection(
		SmartMessageFilter filter,
		const PeerData *sender = nullptr);
	explicit SmartMessageSelection(QStringList keywords);
	explicit SmartMessageSelection(QRegularExpression regex);

	[[nodiscard]] bool mayMatchUnloaded(const MTPMessage &message) const;
	[[nodiscard]] bool add(not_null<HistoryItem*> item);
	void remove(FullMsgId id);
	void resetRegexBudget();
	[[nodiscard]] bool finish(bool wholeChat = false) const;
	[[nodiscard]] const MessageIdsList &items() const;

private:
	[[nodiscard]] bool matches(not_null<HistoryItem*> item);

	SmartMessageFilter _filter;
	const PeerData *_sender = nullptr;
	QStringList _keywords;
	QRegularExpression _regex;
	QElapsedTimer _regexTimer;
	MessageIdsList _items;
	bool _failed = false;
	bool _limited = false;

};

inline SmartMessageSelection::SmartMessageSelection(
	SmartMessageFilter filter,
	const PeerData *sender)
: _filter(filter)
, _sender(sender) {
}

inline SmartMessageSelection::SmartMessageSelection(QStringList keywords)
: _filter(SmartMessageFilter::Filename)
, _keywords(std::move(keywords)) {
}

inline SmartMessageSelection::SmartMessageSelection(QRegularExpression regex)
: _filter(SmartMessageFilter::FilenameRegex)
, _regex(std::move(regex)) {
}

inline bool SmartMessageSelection::mayMatchUnloaded(
		const MTPMessage &message) const {
	if (message.type() != mtpc_message) {
		return false;
	}
	const auto &data = message.c_message();
	if (_filter == SmartMessageFilter::Text) {
		return true;
	} else if (_filter == SmartMessageFilter::Sender) {
		const auto sender = data.vfrom_id();
		return _sender && (!sender || peerFromMTP(*sender) == _sender->id);
	}
	const auto media = data.vmedia();
	const auto type = (_filter == SmartMessageFilter::Photos)
		? mtpc_messageMediaPhoto
		: mtpc_messageMediaDocument;
	return media && (media->type() == type);
}

inline bool SmartMessageSelection::add(not_null<HistoryItem*> item) {
	if (!matches(item)) {
		return !_failed;
	} else if (_items.size() == MaxSelectedItems) {
		_limited = true;
		return false;
	}
	_items.push_back(item->fullId());
	return true;
}

inline void SmartMessageSelection::remove(FullMsgId id) {
	std::erase(_items, id);
}

inline void SmartMessageSelection::resetRegexBudget() {
	_regexTimer.invalidate();
}

inline bool SmartMessageSelection::finish(bool wholeChat) const {
	if (_failed) {
		Ui::Toast::Show(tr::lng_smart_select_regex_failed(tr::now));
		return false;
	} else if (_items.empty()) {
		Ui::Toast::Show(wholeChat
			? tr::lng_smart_select_no_chat_matches(tr::now)
			: tr::lng_smart_select_no_matches(tr::now));
		return false;
	} else if (_limited) {
		Ui::Toast::Show(tr::lng_smart_select_limit(
			tr::now,
			lt_limit,
			QString::number(MaxSelectedItems)));
	}
	return true;
}

inline const MessageIdsList &SmartMessageSelection::items() const {
	return _items;
}

inline bool SmartMessageSelection::matches(not_null<HistoryItem*> item) {
	constexpr auto kRegexScanBudgetMs = 150;
	if (!item->isRegular() || item->isService()) {
		return false;
	}
	if (_filter == SmartMessageFilter::Sender) {
		return _sender && item->from().get() == _sender;
	}
	const auto media = item->media();
	if (_filter == SmartMessageFilter::Text) {
		return !item->originalText().text.isEmpty()
			&& (!media || media->webpage());
	} else if (!media || media->webpage()) {
		return false;
	}
	const auto document = media->document();
	if (_filter == SmartMessageFilter::FilenameRegex
		|| _filter == SmartMessageFilter::Filename) {
		const auto name = document ? document->filename() : QString();
		if (name.isEmpty()) {
			return false;
		} else if (_filter == SmartMessageFilter::Filename) {
			return !_keywords.isEmpty()
				&& ranges::all_of(_keywords, [&](const QString &word) {
					return name.contains(word, Qt::CaseInsensitive);
				});
		}
		if (!_regexTimer.isValid()) {
			_regexTimer.start();
		}
		if (_regexTimer.hasExpired(kRegexScanBudgetMs)) {
			_failed = true;
			return false;
		}
		const auto match = _regex.match(name);
		_failed = !match.isValid()
			|| _regexTimer.hasExpired(kRegexScanBudgetMs);
		return !_failed && match.hasMatch();
	}
	switch (_filter) {
	case SmartMessageFilter::Photos:
		return media->photo();
	case SmartMessageFilter::Videos:
		return document && (document->isVideoFile()
			|| document->isVideoMessage()
			|| document->isAnimation()
			|| document->isGifv());
	case SmartMessageFilter::Files:
		return document
			&& !document->isVideoFile()
			&& !document->isVideoMessage()
			&& !document->isAnimation()
			&& !document->isGifv()
			&& !document->isAudioFile()
			&& !document->isVoiceMessage()
			&& !document->sticker();
	case SmartMessageFilter::Audio:
		return document && (document->isAudioFile()
			|| document->isVoiceMessage());
	case SmartMessageFilter::Text:
	case SmartMessageFilter::Sender:
	case SmartMessageFilter::Filename:
	case SmartMessageFilter::FilenameRegex:
		return false;
	}
	return false;
}

} // namespace HistoryView
