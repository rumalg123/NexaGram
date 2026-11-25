# Resumable downloads in 64Gram

Open **Settings → Advanced → Resumable Downloads**, enable **Enable resumable downloads**, and restart 64Gram. The initial setting is off. Open **Downloads** from Advanced or the chat-list download indicator.

Use **Save As** on a document you have permission to save. Pick an unused filename in a private folder you control. Native Telegram file saves can preserve progress, including eligible video and audio saves. Watching, playing or buffering media alone does not create a resumable save. Automatic caching, protected or disappearing media, web transfers, Mini Apps, exports and uploads are outside this feature.

In Downloads, **Pause** keeps the incomplete file. **Resume** checks the source and saved chunks and fetches what is missing. **Retry** retries a recoverable problem. **Cancel** permanently discards that job's verified incomplete files. Completed files are separate. Select multiple rows for selected actions, or use the current account's **Pause All** and **Resume All**. Bulk cancellation asks for confirmation.

After a restart, open Downloads to find interrupted saves. **Resume interrupted downloads on startup** is optional and starts off. Manually paused saves wait for your explicit Resume. A large partial download may spend time verifying before its progress appears. File downloads and playback have separate controls.

Partial data and a small checkpoint occupy disk space beside the chosen destination. Keep both in place. A changed or shortened chunk can be fetched again. If a file was replaced, the drive is disconnected, or the checkpoint cannot be verified, reconnect the drive or restore the matching files from your own backup. When ownership is unclear, choose a new empty destination for a fresh save. If the final name is occupied, move the occupant yourself before retrying. The completed file appears only after verification.

Turning the feature off takes effect after a restart. Your incomplete saves remain manageable in Downloads. Re-enable and restart to resume. You can hide the idle indicator and still open Downloads through Settings. Logging out retires that account's incomplete saves. Files on an unavailable drive or files whose identity has changed can require manual attention. Review them individually before removal.

Use a local filesystem with reliable file identity and rename support. Saved media and partial files inherit filesystem privacy. Successful checkpoints help after a process interruption. Power loss, device failure and storage that does not honor flushes can lose recent work. Keep completed files you need backed up.

The beta reads historical v1 jobs. Their checksums can detect mismatched bytes, but cannot authenticate a v1 checkpoint that was altered together with the partial file. Resume those jobs only from a trusted folder. New jobs authenticate their checkpoints, and a registry containing one of these jobs uses v2. Finish or cancel such jobs in a compatible build before downgrading. If you already downgraded, close the older client and return to the compatible build with the same intact profile and download files. Use one client per profile.

## Public beta release notes

This public beta of an unofficial 64Gram fork adds persistent resumable downloads for eligible Telegram files, including direct MTProto and streaming-backed saves. Pause individual downloads and recover verified progress after closing 64Gram, a crash, or a computer restart. Video playback and the file transfer can continue independently.

- Use **Pause All**, **Resume All**, or actions for selected downloads. Search by filename, filter by status, and see progress, speed, and estimated time remaining when available.
- Enable the feature in **Settings → Advanced → Resumable Downloads**, then restart 64Gram. Automatic startup resume is optional and off by default. Manually paused downloads wait for you. Pause keeps the partial download. Cancel discards it.
- 64Gram verifies the saved chunks before finishing and preserves any existing file at the destination. If the final name is occupied, move the existing file and retry.
- The feature covers eligible Telegram file saves, including supported video and audio. Web downloads, Mini Apps, exports, automatic caching, and protected or disappearing media are outside its scope. Recovery depends on continued access to the source, intact local files, and the filesystem. Keep older v1 jobs in trusted folders and finish or cancel new v2 jobs before downgrading.

Report problems in the beta announcement thread with the build version, operating system, filesystem, and steps to reproduce. Share only relevant, redacted logs.
