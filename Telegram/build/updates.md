# NexaGram signed updates

NexaGram starts at **1.0.0 beta**, based on Telegram Desktop **7.4.7** and 64Gram **1.2.13**. Update all three version fields from the repository root.

    python Telegram/build/set_version.py 1.0.0.beta 7.4.7 1.2.13

Build normal desktop releases with auto-update enabled, with a matching stable or beta value for the channel, and with the NexaGram public update trust directory. It contains the public Ed25519 root, its signed manifest, and the detached signature.

    -DDESKTOP_APP_DISABLE_AUTOUPDATE=OFF
    -DTDESKTOP_UPDATE_CHANNEL=beta
    -DTDESKTOP_FORK_UPDATE_URL=https://github.com/rumalg123/NexaGram/releases/download/nexagram-feed
    -DTDESKTOP_FORK_UPDATE_TRUST_DIR=<public-trust-directory>

The trust directory supplies **root-public.pem**, **manifest.min.json**, and **manifest.sig**. Provision a NexaGram signing identity, keep the private root offline, and keep private signing files outside the repository and all distributed artifacts. Builds without a signed feed use manual downloads unless configured for a supported packaging or canary update channel.

## GitHub release layout

Publish user downloads and release notes at a versioned tag such as **nexagram-v1.0.0.beta**. The mutable **nexagram-feed** prerelease holds **check** and the signed packages it references. Publish each package, verify the public download, then replace **check** while carrying forward every unaffected channel and platform.

The first beta uses this Windows x64 entry.

    {"win64":{"beta":{"released":1000000,"link":"/td-update-win-x64-1000000-beta"}}}

**Install beta versions** in Settings → Advanced controls whether a client considers the beta lane along with stable. With beta updates off, the client considers stable only. The first install is manual. Subsequent upgrades require a greater numeric version, so a stable **1.0.0** cannot automatically replace **1.0.0.beta**. A later patch such as **1.0.1** can.

The final installer or ZIP and its checksum belong on the public versioned release. Keep the existing 64Gram **Latest** release in place while it still serves the legacy feed. NexaGram uses the explicit feed tag and a dedicated trust root. GitHub release immutability requires a separate mutable host for the map and packages.

## Packaging and profiles

Use the **Packer** target to build signed v2 packages with the same platform, internal version, channel, and public trust as the build. For Windows x64, include **NexaGram.exe**, **Updater.exe**, and the widget runtime under **modules/x64**. The older zstd package tool serves historical workflows. Use the signed v2 format for the NexaGram feed.

The CMake app target remains **Telegram**. The product files are **NexaGram.exe**, **NexaGram.app**, and **NexaGram**. Standard installations use a NexaGram profile. For a portable profile, include a **NexaGramForcePortable** folder beside the executable or macOS bundle. Deploy in a distinct location and sign in fresh. Use an isolated profile when testing package updates.
