/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/update_keys.h"

#ifdef TDESKTOP_FORK_UPDATE_PREFIX
#include "fork_update_keys_data.h"
#else // TDESKTOP_FORK_UPDATE_PREFIX
#include "update_keys_data.h"
#endif // TDESKTOP_FORK_UPDATE_PREFIX

namespace Core::Updates {

QByteArray RootPublicKeyPem() {
	return QByteArray::fromRawData(
		reinterpret_cast<const char*>(details::kRootPublicKeyPem),
		sizeof(details::kRootPublicKeyPem));
}

QByteArray EmbeddedManifest() {
	return QByteArray::fromRawData(
		reinterpret_cast<const char*>(details::kEmbeddedManifest),
		sizeof(details::kEmbeddedManifest));
}

QByteArray EmbeddedManifestSignature() {
	return QByteArray::fromRawData(
		reinterpret_cast<const char*>(details::kEmbeddedManifestSig),
		sizeof(details::kEmbeddedManifestSig));
}

} // namespace Core::Updates
