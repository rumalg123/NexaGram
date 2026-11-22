/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "history/history_item.h"
#include "data/data_document.h"
#include "data/data_media_types.h"

namespace HistoryView {

// Smart Select works on messages already loaded by the current chat view.
// It does not fetch history or download any media.
enum class SmartMessageFilter : uchar {
	Photos,
	Videos,
	Files,
	Audio,
	Text,
	Sender,
};

[[nodiscard]] inline bool SmartMessageMatches(
		not_null<HistoryItem*> item,
		SmartMessageFilter filter,
		const PeerData *sender = nullptr) {
	if (!item->isRegular() || item->isService()) {
		return false;
	}
	if (filter == SmartMessageFilter::Sender) {
		return sender && item->from().get() == sender;
	}
	const auto media = item->media();
	const auto document = media ? media->document() : nullptr;
	switch (filter) {
	case SmartMessageFilter::Photos:
		return media && media->photo();
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
		return !item->originalText().text.isEmpty()
			&& (!media || media->webpage());
	case SmartMessageFilter::Sender:
		return false;
	}
	return false;
}

} // namespace HistoryView
