/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "info/downloads/info_downloads_status.h"

#include "lang/lang_keys.h"
#include "ui/text/format_values.h"

namespace Info::Downloads {

QString StatusText(Storage::DownloadStatus status) {
	using Status = Storage::DownloadStatus;
	switch (status) {
	case Status::Preparing: return tr::lng_downloads_status_preparing(tr::now);
	case Status::Downloading: return tr::lng_downloads_status_downloading(tr::now);
	case Status::Pausing: return tr::lng_downloads_status_pausing(tr::now);
	case Status::Paused: return tr::lng_downloads_status_paused(tr::now);
	case Status::Interrupted: return tr::lng_downloads_status_interrupted(tr::now);
	case Status::Resolving: return tr::lng_downloads_status_resolving(tr::now);
	case Status::Verifying: return tr::lng_downloads_status_verifying(tr::now);
	case Status::Resuming: return tr::lng_downloads_status_resuming(tr::now);
	case Status::Waiting: return tr::lng_downloads_status_network(tr::now);
	case Status::Failed: return tr::lng_downloads_status_failed(tr::now);
	case Status::Unavailable: return tr::lng_downloads_status_unavailable(tr::now);
	case Status::Finalizing: return tr::lng_downloads_status_finalizing(tr::now);
	case Status::Cancelling: return tr::lng_downloads_status_cancelling(tr::now);
	case Status::Completed: return tr::lng_downloads_status_completed(tr::now);
	}
	return {};
}

QString IssueText(Storage::DownloadIssue issue) {
	using Issue = Storage::DownloadIssue;
	switch (issue) {
	case Issue::None: return {};
	case Issue::SourceLookup: return tr::lng_downloads_issue_source(tr::now);
	case Issue::MissingFile: return tr::lng_downloads_issue_missing(tr::now);
	case Issue::ChangedFile: return tr::lng_downloads_issue_changed(tr::now);
	case Issue::Checkpoint: return tr::lng_downloads_issue_checkpoint(tr::now);
	case Issue::Storage: return tr::lng_downloads_issue_storage(tr::now);
	case Issue::Destination: return tr::lng_downloads_issue_destination(tr::now);
	case Issue::Transfer: return tr::lng_downloads_issue_transfer(tr::now);
	}
	return {};
}

QString TransferRateText(qint64 bytesPerSecond) {
	auto size = Ui::FormatSizeText(bytesPerSecond);
	size.replace(QChar(' '), QChar::Nbsp);
	return tr::lng_downloads_transfer_rate(tr::now, lt_size, size);
}

QString FilterText(Storage::DownloadFilter filter) {
	using Filter = Storage::DownloadFilter;
	switch (filter) {
	case Filter::All: return tr::lng_downloads_filter_all(tr::now);
	case Filter::Active: return tr::lng_downloads_filter_active(tr::now);
	case Filter::Paused: return StatusText(Storage::DownloadStatus::Paused);
	case Filter::Interrupted: return StatusText(Storage::DownloadStatus::Interrupted);
	case Filter::Failed: return StatusText(Storage::DownloadStatus::Failed);
	case Filter::Completed: return StatusText(Storage::DownloadStatus::Completed);
	}
	return {};
}

QString ActionText(Storage::DownloadAction action, Storage::DownloadStatus status) {
	using Status = Storage::DownloadStatus;
	using Action = Storage::DownloadAction;
	switch (action) {
	case Action::Pause: return tr::lng_downloads_pause(tr::now);
	case Action::Resume: return (status == Status::Failed || status == Status::Waiting)
		? tr::lng_downloads_retry(tr::now)
		: tr::lng_downloads_resume(tr::now);
	case Action::Cancel: return tr::lng_context_cancel_download(tr::now);
	}
	return {};
}

} // namespace Info::Downloads
