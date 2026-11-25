/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/core_download_settings.h"

#include "core/core_settings.h"
#include "storage/localstorage.h"
#include "storage/pending_downloads.h"

namespace Core {
namespace {

using Preference = Storage::DownloadPreference;

} // namespace

DownloadSettings::DownloadSettings(not_null<Settings*> settings)
: _settings(settings) {
}

void DownloadSettings::initialize() {
	const auto &overrides = Storage::PersistentDownloadOverrides();
	const auto raw = _settings->readPref<QByteArray>(
		Storage::DownloadPreferenceKey(Preference::Enabled));
	if (raw.isNull()) {
		Storage::TracePendingDownload({}, "download preferences absent, using defaults");
	} else if (!Storage::DownloadBoolean(raw)) {
		Storage::TracePendingDownload({}, "download preference invalid, using default");
	}
	_activation = Storage::ResolveDownloadActivation(preferences(), overrides);
	Storage::TracePendingDownload({}, "download preferences loaded");
	if (overrides.enabled != Storage::DownloadOverride::Absent) {
		Storage::TracePendingDownload({}, "download feature override", int(overrides.enabled));
	}
	if (overrides.startupResume != Storage::DownloadOverride::Absent) {
		Storage::TracePendingDownload({}, "download startup override", int(overrides.startupResume));
	}
	Storage::TracePendingDownload({}, "download tracing override", int(overrides.trace));
	Storage::TracePendingDownload({}, "download policy enabled", int(_activation.enabled));
	Storage::TracePendingDownload({}, "download policy startup resume", int(_activation.startupResume));
}

void DownloadSettings::preferencesReset() {
	Storage::TracePendingDownload({}, "download preferences reset");
	_changes.fire({});
}

bool DownloadSettings::setPreference(Preference preference, bool value) {
	const auto key = Storage::DownloadPreferenceKey(preference);
	const auto data = Storage::DownloadBooleanValue(value);
	if (!_settings->writePrefChecked(key, data, Local::writeSettingsChecked)) {
		Storage::TracePendingDownload({}, "download preference save failed", int(preference));
		return false;
	}
	Storage::TracePendingDownload({}, "download preference saved", int(preference));
	if (preference == Preference::Enabled) {
		Storage::TracePendingDownload({}, value
			? "download activation saved for next launch"
			: "download deactivation saved for next launch, pending jobs preserved");
	}
	_changes.fire({});
	return true;
}

Storage::DownloadPreferences DownloadSettings::preferences() const {
	return Storage::ReadDownloadPreferences([&](std::string_view key) {
		return _settings->readPref<QByteArray>(key);
	});
}

Storage::DownloadActivation DownloadSettings::activation() const {
	return _activation;
}

bool DownloadSettings::restartRequired() const {
	return _activation.enabled != Storage::ResolveDownloadActivation(
		preferences(), Storage::PersistentDownloadOverrides()).enabled;
}

rpl::producer<> DownloadSettings::changes() const {
	return _changes.events();
}

} // namespace Core
