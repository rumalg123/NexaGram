/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "storage/download_policy.h"

namespace Core {

class Settings;

class DownloadSettings final {
public:
	explicit DownloadSettings(not_null<Settings*> settings);

	void initialize();
	void preferencesReset();
	[[nodiscard]] bool setPreference(Storage::DownloadPreference preference, bool value);

	[[nodiscard]] Storage::DownloadPreferences preferences() const;
	[[nodiscard]] Storage::DownloadActivation activation() const;
	[[nodiscard]] bool restartRequired() const;
	[[nodiscard]] rpl::producer<> changes() const;

private:
	const not_null<Settings*> _settings;
	Storage::DownloadActivation _activation;
	rpl::event_stream<> _changes;

};

} // namespace Core
