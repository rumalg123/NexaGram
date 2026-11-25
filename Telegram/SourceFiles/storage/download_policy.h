/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QByteArrayView>
#include <QtCore/QStringView>
#include <QtCore/QUuid>
#include <QtCore/QtGlobal>

#include <optional>
#include <string_view>

namespace Storage {

enum class DownloadPreference {
	Enabled,
	StartupResume,
	ConfirmCancellation,
	PendingIndicator,
};

struct DownloadPreferences {
	bool enabled = false;
	bool startupResume = false;
	bool confirmCancellation = true;
	bool pendingIndicator = true;
};

enum class DownloadOverride { Absent, Disabled, Enabled, Invalid };

struct DownloadOverrides {
	DownloadOverride enabled = DownloadOverride::Absent;
	DownloadOverride startupResume = DownloadOverride::Absent;
	DownloadOverride trace = DownloadOverride::Absent;
};

struct DownloadActivation {
	bool enabled = false;
	bool startupResume = false;
};

[[nodiscard]] inline std::optional<bool> DownloadBoolean(QByteArrayView value) {
	if (value.size() == 1) {
		switch (value.front()) {
		case '1': return true;
		case '0': return false;
		}
	}
	return std::nullopt;
}

[[nodiscard]] inline QByteArray DownloadBooleanValue(bool value) {
	return value ? QByteArray("1") : QByteArray("0");
}

[[nodiscard]] constexpr std::string_view DownloadPreferenceKey(DownloadPreference preference) {
	switch (preference) {
	case DownloadPreference::Enabled: return "downloads.resumable.enabled";
	case DownloadPreference::StartupResume: return "downloads.resumable.startup_resume";
	case DownloadPreference::ConfirmCancellation: return "downloads.resumable.confirm_cancel";
	case DownloadPreference::PendingIndicator: return "downloads.resumable.pending_indicator";
	}
	return {};
}

template <typename Read>
[[nodiscard]] DownloadPreferences ReadDownloadPreferences(Read read) {
	const auto value = [&](DownloadPreference preference, bool fallback) {
		const auto bytes = read(DownloadPreferenceKey(preference));
		return DownloadBoolean(bytes).value_or(fallback);
	};
	const auto defaults = DownloadPreferences();
	return {
		.enabled = value(DownloadPreference::Enabled, defaults.enabled),
		.startupResume = value(DownloadPreference::StartupResume, defaults.startupResume),
		.confirmCancellation = value(DownloadPreference::ConfirmCancellation, defaults.confirmCancellation),
		.pendingIndicator = value(DownloadPreference::PendingIndicator, defaults.pendingIndicator),
	};
}

[[nodiscard]] inline DownloadOverride ParseDownloadOverride(bool present, QByteArrayView value) {
	if (!present) {
		return DownloadOverride::Absent;
	}
	if (const auto parsed = DownloadBoolean(value)) {
		return *parsed ? DownloadOverride::Enabled : DownloadOverride::Disabled;
	}
	return DownloadOverride::Invalid;
}

[[nodiscard]] constexpr std::optional<bool> DownloadOverrideValue(DownloadOverride value) {
	switch (value) {
	case DownloadOverride::Enabled: return true;
	case DownloadOverride::Disabled: return false;
	case DownloadOverride::Absent:
	case DownloadOverride::Invalid: return std::nullopt;
	}
	return std::nullopt;
}

[[nodiscard]] constexpr DownloadActivation ResolveDownloadActivation(
		DownloadPreferences preferences,
		DownloadOverrides overrides) {
	const auto enabled = DownloadOverrideValue(overrides.enabled).value_or(preferences.enabled);
	return {
		.enabled = enabled,
		.startupResume = enabled
			&& DownloadOverrideValue(overrides.startupResume).value_or(preferences.startupResume),
	};
}

[[nodiscard]] constexpr bool ConfirmDownloadCancellation(
		bool persistent,
		bool bulk,
		bool confirmIndividual) {
	return persistent && (bulk || confirmIndividual);
}

[[nodiscard]] constexpr bool ShowDownloadIndicatorEntry(
		bool persistent,
		bool transferring,
		bool showPending) {
	return !persistent || transferring || showPending;
}

[[nodiscard]] inline bool IsPendingDownloadArtifactName(QStringView name) {
#ifdef Q_OS_WIN
	if (const auto alternateStream = name.indexOf(u':'); alternateStream >= 0) {
		name = name.left(alternateStream);
	}
	while (!name.isEmpty() && (name.back() == u'.' || name.back() == u' ')) {
		name.chop(1);
	}
#endif
	const auto prefix = QStringView(u".64gram-");
	const auto suffix = QStringView(u".part");
	const auto checkpoint = QStringView(u".resume");
	const auto casing = Qt::CaseInsensitive;
	if (name.endsWith(checkpoint, casing)) {
		name.chop(checkpoint.size());
	}
	return name.size() == prefix.size() + 36 + suffix.size()
		&& name.startsWith(prefix, casing)
		&& name.endsWith(suffix, casing)
		&& !QUuid::fromString(name.mid(prefix.size(), 36)).isNull();
}

[[nodiscard]] inline bool PersistentDownloadPerformanceLog() {
	static const auto enabled = qgetenv("TDESKTOP_PERSISTENT_DOWNLOAD_PERF") == "1";
	return enabled;
}

[[nodiscard]] inline const DownloadOverrides &PersistentDownloadOverrides() {
	static const auto result = [] {
		const auto read = [](const char *name) {
			return ParseDownloadOverride(qEnvironmentVariableIsSet(name), qgetenv(name));
		};
		return DownloadOverrides{
			.enabled = read("TDESKTOP_EXPERIMENTAL_RESUMABLE_DOWNLOADS"),
			.startupResume = read("TDESKTOP_EXPERIMENTAL_RESUME_ON_STARTUP"),
			.trace = read("TDESKTOP_PERSISTENT_DOWNLOAD_TRACE"),
		};
	}();
	return result;
}

} // namespace Storage
