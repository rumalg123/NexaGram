# Downloads management testing on Windows

Use an isolated NexaGram profile and non-sensitive fixtures. The first tests require only small downloads. Keep the profile and all test destinations separate from your normal client.

## Build and run the standalone tests

Open `D:\TBuild\tdesktop\out\Telegram.slnx` in Visual Studio. Select **Debug | x64**, then build the **Telegram** project. Build **test_persistent_file_sink** separately in the **(tests)** group. The existing solution can regenerate its source and language registrations through its normal build dependencies. Keep the configured output and dependencies. From an x64 Developer PowerShell, the equivalent command is shown below.

- The client is `D:\TBuild\tdesktop\out\Debug\NexaGram.exe`.
- The test executable is `D:\TBuild\tdesktop\out\test_persistent_file_sink\Debug\test_persistent_file_sink.exe`.

```powershell
$root = 'D:\TBuild\tdesktop'
cmake --build (Join-Path $root 'out') --config Debug --target Telegram test_persistent_file_sink
$exe = Join-Path $root 'out\Debug\NexaGram.exe'
$tests = Join-Path $root 'out\test_persistent_file_sink\Debug\test_persistent_file_sink.exe'
$result = Start-Process -FilePath $tests -Wait -PassThru -WindowStyle Hidden
$result.ExitCode
if ($result.ExitCode -ne 0) { throw 'Persistent download tests failed' }
```

Expect exit code `0`. The standalone fixtures exercise action eligibility, stale and duplicate selections, account scope, Unicode and punctuation search combined with status, a 4,096-record filter, byte-weighted totals, committed bytes, monotonic speed samples, and ETA. The existing direct, streaming, fault and restart fixtures run in the same executable. The sparse large-file fixture needs an NTFS temporary directory. The fixtures exercise presentation and lifecycle rules without connecting to Telegram.

## Launch an isolated client

```powershell
$lab = Join-Path $env:LOCALAPPDATA 'NexaGram-Download-Manager-Test'
$profile = Join-Path $lab 'profile'
$originals = Join-Path $lab 'originals'
$dest = Join-Path $lab 'downloads'
New-Item -ItemType Directory -Force -Path $profile, $originals, $dest | Out-Null

$env:TDESKTOP_EXPERIMENTAL_RESUMABLE_DOWNLOADS = '1'
$env:TDESKTOP_PERSISTENT_DOWNLOAD_TRACE = '1'
$env:TDESKTOP_EXPERIMENTAL_RESUME_ON_STARTUP = '0'

function Start-DownloadManagerClient {
    Start-Process -FilePath $exe -ArgumentList ('-many -debug -workdir "{0}"' -f $profile) -PassThru
}

$log = Join-Path $profile 'log.txt'
$client = Start-DownloadManagerClient
```

Sign in with a test account. Add another test account for the isolation cases. Set **Settings → Advanced** to ask where to save files and select the test destination. Open Downloads from the download bar in the chat list, or from the Downloads entry in search. Use the search icon in the panel to search filenames.

Use three or four different ZIP or BIN fixtures, one small completed file, and a playable MP4 or M4A. Start with 20–100 MiB direct fixtures and a 50–200 MiB media file. Use a larger file only if your connection finishes the smaller files before you can pause them. For filename tests, use a title such as `Résumé α (Final).ZIP`. Keep an original copy of each file in `$originals`. The streaming guide has an optional FFmpeg recipe for synthetic media.

To disable the experiment, quit the isolated client first and remove these environment variables before launching again.

```powershell
Remove-Item Env:TDESKTOP_EXPERIMENTAL_RESUMABLE_DOWNLOADS -ErrorAction SilentlyContinue
Remove-Item Env:TDESKTOP_PERSISTENT_DOWNLOAD_TRACE -ErrorAction SilentlyContinue
Remove-Item Env:TDESKTOP_EXPERIMENTAL_RESUME_ON_STARTUP -ErrorAction SilentlyContinue
```

## Read the panel and diagnostics

The list can show rows from all signed-in accounts. Pending rows identify their account and the source when it is available. The bottom controls and summary describe the current account's pending resumable files. **Pause All** and **Resume All** operate on that account across all search and status results. **Selected Actions** operates on eligible selected results from that account. Account changes and resuming a source can update the current result set.

**Active** includes preparation, verification, source lookup, queued resume, transfer, pause in progress, finishing, and cancellation in progress. **Paused** shows a settled manual pause. **Interrupted** includes waiting for network. **Failed** includes unavailable sources and retryable failures. **Completed** shows the existing history. A transition may change which filter contains a given row. Search and filter changes clear selection, and state changes remove selected rows that leave the filter. Select All Results uses the existing Downloads selection limit.

The footer percentage is written bytes divided by expected bytes for pending jobs in the current account. The tray animates active transfers and remains available with pending-only jobs. The Pause, Resume, or Retry tooltip shows the verified bytes and destination. Cancel is available on wide rows and in the context menu. Speed and ETA appear only when a recent network sample supports an estimate. Cache deliveries advance file progress independently.

```powershell
Select-String -LiteralPath $log -Pattern 'Pending|Persistent download'
$job = 'paste-the-test-job-guid-without-braces'
Select-String -LiteralPath $log -SimpleMatch $job
Select-String -LiteralPath $log -Pattern 'bulk current account|bulk snapshot|bulk accepted|bulk skipped|bulk failed|bulk owner changed'
Get-ChildItem -LiteralPath $dest -Force | Select-Object Name, Length
```

| Trace | Expected meaning |
|---|---|
| `job action accepted`, `job action skipped, stale or ineligible`, `job action failed` | A single job was dispatched, changed state before dispatch, or could not persist its intent. Action numbers 0, 1, and 2 mean Pause, Resume, and Cancel. |
| `bulk current account started action`, `bulk snapshot size`, `bulk accepted`, `bulk skipped`, `bulk failed` | One stable account-scoped batch. Accepted means an intent was persisted or a resume request was queued. Subsequent source or storage failures appear on the affected rows. |
| `job state 3`, `suspend checkpoint`, `resolve source`, `source and permissions validated` | Manual pause was recorded, then a later resume checked its source. |
| `job state 8`, `cancel cleanup`, `cancel cleanup deferred` | Cancellation is terminal. A file lock or changed artifact can defer cleanup. |
| `backend streaming`, `stream detach`, `stream playback` | A streaming-backed file action detaches just the file consumer. |

Start with an unused final name. If a file later occupies the name, it stays intact and the partial remains available for retry. The directory holds the same `.64gram-<job>.part` and `.part.resume` pair through pause and resume. Completion publishes the expected final file and removes the pending artifacts after history acknowledgement. Cancellation deletes verified partial progress and never targets a completed file.

```powershell
$fixture = 'Résumé α (Final).ZIP'
$expected = (Get-FileHash -LiteralPath (Join-Path $originals $fixture) -Algorithm SHA256).Hash
$actual = (Get-FileHash -LiteralPath (Join-Path $dest $fixture) -Algorithm SHA256).Hash
if ($expected -ne $actual) { throw 'Final SHA-256 mismatch' }
$actual
```

## Essential acceptance tests

| Case | Actions | Pass criteria |
|---|---|---|
| Individual actions | Start an explicit native file save. Open Downloads. Click that row's pause control after the first checkpoint. Hover its action for the saved-byte detail. Resume using the row's control, then repeat from its context menu. | Preparing and Downloading advance to Pausing and Paused. Resume passes through queueing, source lookup and verification, then Downloading. The same pending row and GUID are retained. SHA-256 matches. |
| Pause All and Resume All | Start two transfers and pause a third. Apply a filename search that hides one active transfer. Press **Pause All**, clear search, then press **Resume All**. | Both active jobs pause and the prior paused file stays paused. Resume All can intentionally resume manually paused jobs. The batch includes hidden jobs belonging to this account. Completed and ordinary non-resumable files remain outside the batch. |
| Selected actions with mixed states | Select a downloading row, a paused row, and a completed row using Ctrl-click. Open **Selected Actions**, then Pause Selected and Resume or Retry Selected. | The menu shows eligible counts for each action. Each action affects only the eligible selected jobs. The completed file still opens through the existing history. |
| Destructive confirmation | Select two incomplete rows and a completed row. Choose **Cancel Selected** and dismiss the confirmation once. Reopen it and confirm. | The dialog shows the account and incomplete count, explains partial progress deletion, and preserves completed files. Dismissal preserves all jobs. Confirmed cancelled jobs never reappear after a restart. |
| Keyboard and filters | Ctrl-click a row, Shift-click a distant visible row, then focus the list and press Ctrl+A. Use Escape or **Clear Selection**. Repeat with a filename search and status filter combined. | Range selection uses the current visible order. Ctrl+A selects the current results within the native limit. Changing search or filter clears selected IDs. Actions from a stale menu cannot target hidden or newly substituted results. |
| Filename and status | Search for `résumé α`, then `).zip` with mixed case. Select **Paused**, **Interrupted**, **Failed**, and **Completed** as applicable. | Filenames and applicable status both match. A file that changes status enters or leaves its filter. Empty filtered results show an appropriate empty message. |
| No active transfers | Pause every persistent job and close Downloads. Reopen using the pending download bar. | The entry remains visible, its subtitle says pending downloads, and it has no active transfer animation. Closing or reopening the panel leaves jobs and checkpoint files intact. |
| Recovered rows | While a job is active, quit from the tray or with Ctrl+Q. Restart the same test profile with automatic resume set to `0`. Open Downloads before the source chat. | The row is visible while coverage is verified and can then be resumed or cancelled. The context menu shows source navigation only when the message is available. Resume keeps the same row until completed. |
| Account isolation | Add a second test account. Download similarly named files from both accounts. Select rows from both accounts, then operate from the first account. Switch accounts while a bulk batch is processing. | Account names identify rows. Bulk mutates only the originating account's IDs. A running batch stays bound to its original account session. The other account's job remains untouched. |

## Extended regression tests

| Case | Actions | Pass criteria |
|---|---|---|
| Streaming and playback | Start a persistent MP4 download, open playback, and seek during **Pause Selected** and **Cancel Selected** on separate fixtures. Stop playback during a third file transfer. | Pausing or cancelling does not stop playback or reset its position. Closing the player does not stop the file transfer. Streaming logs keep the original document backend. |
| Retry and failed cleanup | With the test client closed, move a copied partial fixture or checkpoint aside and restart. Use the failed row's Retry after returning the original while closed. Separately cancel a test partial whose file is locked, then release the lock and reopen. | Missing, replaced, or corrupt metadata is reported accurately. Failed cleanup reports a deferred cleanup and keeps the job terminal. Retry verifies owned staged bytes before transfer. |
| Completion or logout during a batch | Start multiple small fixtures, begin a bulk action near completion, or switch and log out the originating test account during dispatch. | Stale, completed, cancelled or logged-out identities are skipped. Remaining eligible jobs continue. The batch totals and subsequent row states describe the actual outcomes. |
| Normal restart and automatic start | Quit with one active and one manually paused job. Set `TDESKTOP_EXPERIMENTAL_RESUME_ON_STARTUP` to `1` and launch again. | The interrupted job can resume automatically. The manually paused job stays paused until explicit Resume. |
| Forced termination regression | Wait for a successful checkpoint, force-stop only the isolated PID using the command below, and restart. Resume the interrupted job. | Recovery skips verified committed offsets. The expected SHA-256 and one completed entry are produced. |
| Themes and layout | In **Settings → Chat Settings**, try the light and dark themes and an existing custom theme. Change the accent, narrow the window, and test Windows scaling at 100% and 150%. If an RTL language is available, repeat with it. | Buttons, labels, and progress bars remain readable and positioned within bounds. Long names and labels elide, and native tooltips or text actions remain available. The layout follows RTL direction. |
| Large history and refresh | Run the deterministic large-history fixture. In the client, use whatever larger history already exists in the isolated profile and start a small active job while scrolling, filtering, and repeatedly reopening the panel. | The filter count is correct. Progress refreshes without row reordering on each chunk. Controls remain usable and reopened panels do not duplicate jobs. |
| Feature disabled | Pause a job, quit, remove the switches, then reopen. Test ordinary downloading and playback. Re-enable the switches and relaunch. | Experimental controls are absent when disabled. Ordinary playback, download, and completed history work. The previously paused job and artifacts return when re-enabled. |
| Source permissions | Use a controlled test message, then delete it or revoke its access from another authorized test account. Try to resume after restart. | The unavailable row cannot resume. No new transfer offsets are requested from the invalid source. Cancel remains available. Actual server permission changes require the connected client. |

Only force-stop the process launched from this guide. Verify the executable and isolated profile as well as the PID.

```powershell
$info = Get-CimInstance Win32_Process -Filter "ProcessId=$($client.Id)"
if (!$info -or $info.ExecutablePath -ne $exe -or
    !$info.CommandLine -or !$info.CommandLine.Contains($profile)) {
    throw 'The PID does not identify the isolated test profile'
}
Stop-Process -Id $client.Id
$client.WaitForExit()
```

Use normal quit for Windows restart testing, then reopen PowerShell and restore the flags and isolated profile commands. Copy relevant log lines before restarting if you need to keep them.

## Cleanup

Use Cancel Selected or Cancel Download on the verified test jobs. Completed test files can be removed through the normal completed history actions or archived after the isolated process exits. Retain an unexpected or unverified staging artifact until you can identify it. Share only the log lines needed for the job GUID, action, and batch totals. Keep your production profile outside the test directory.

For detailed disk faults, source expiry, committed-offset filtering, and CDN tests, see the [direct download recovery guide](persistent-download-recovery-windows.md) and the [streaming download guide](persistent-streaming-downloads-windows.md).
