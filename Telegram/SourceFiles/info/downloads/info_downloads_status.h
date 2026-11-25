/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "storage/download_presentation.h"

namespace Info::Downloads {

[[nodiscard]] QString StatusText(Storage::DownloadStatus status);
[[nodiscard]] QString IssueText(Storage::DownloadIssue issue);
[[nodiscard]] QString FilterText(Storage::DownloadFilter filter);
[[nodiscard]] QString ActionText(
	Storage::DownloadAction action,
	Storage::DownloadStatus status);

} // namespace Info::Downloads
