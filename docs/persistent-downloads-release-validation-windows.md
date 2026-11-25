# Windows release validation for resumable downloads

The release owner confirmed that Milestone 6 manual validation passed. Use this guide to record individual results and validate the final portable artifact. Use your isolated test account, non-sensitive data and newly named files. Record the source commit, toolchain, Windows version, filesystem, test date and result per case.

## Stage A — essential smoke tests

Open `D:\TBuild\tdesktop\out\Telegram.slnx` in Visual Studio. Select **Debug | x64**, build **Telegram**, then **test_persistent_file_sink** under **(tests)**. Repeat under **Release | x64** for Release smoke and benchmarks. Use the existing non-LTO Release configuration. Equivalent commands from x64 Developer PowerShell:

```powershell
$repo = 'D:\TBuild\tdesktop'
$out = Join-Path $repo 'out'
Select-String 'DESKTOP_APP_ENABLE_LTO:.*=OFF' "$out\CMakeCache.txt"
cmake --build $out --config Debug --target Telegram test_persistent_file_sink
cmake --build $out --config Release --target Telegram test_persistent_file_sink
$debugExe = "$out\Debug\64Gram.exe"
$releaseExe = "$out\Release\64Gram.exe"
$lab = Join-Path $env:LOCALAPPDATA '64Gram-M6-Validation'
$profile = Join-Path $lab 'profile'
$dest = Join-Path $lab 'downloads'
$originals = Join-Path $lab 'originals'
New-Item -ItemType Directory -Force -Path $profile,$dest,$originals | Out-Null
$flags = 'TDESKTOP_EXPERIMENTAL_RESUMABLE_DOWNLOADS','TDESKTOP_EXPERIMENTAL_RESUME_ON_STARTUP','TDESKTOP_PERSISTENT_DOWNLOAD_TRACE','TDESKTOP_PERSISTENT_DOWNLOAD_PERF'
foreach ($flag in $flags) { Remove-Item "Env:$flag" -ErrorAction SilentlyContinue }
$link = (New-Object -ComObject WScript.Shell).CreateShortcut("$lab\64Gram validation.lnk")
$link.TargetPath = $debugExe
$link.WorkingDirectory = Split-Path $debugExe
$link.Arguments = '-many -workdir "' + $profile + '"'
$link.Save()
```

1. Double-click the shortcut in File Explorer. Use a Windows test user with no inherited overrides. Removing shell variables only affects that shell's children. Sign into the isolated profile and open **Settings → Advanced → Resumable Downloads**. Expect Off / Off / On / On. Enable the main toggle and use **Restart now**. Confirm it stays On.
2. Upload a 32–100 MiB ZIP as a file and an MP4 as playable media to a private test chat. Retain originals. When the server transcodes media, obtain an uninterrupted reference download for the same exact document and quality. Right-click each message → **Save As**, using new filenames under the test destination.
3. Open Downloads from Advanced. Pause both transfers after some progress. Watch and seek in the video. Expect the partial files and checkpoints to remain, the final names to be absent, and playback to continue. Resume the ZIP, quit via the application menu before completion, and reopen the shortcut. With startup resume Off, the ZIP waits for Resume and the video stays manually paused.
4. Resume both. Expect one completed entry per job, no owned sidecars after reconciliation, and equal SHA-256 hashes. Start and cancel a third job; confirm its prompt and verify the completed files survive.

```powershell
Get-ChildItem -LiteralPath $dest -Force
function Assert-SameFile($Original, $Downloaded) {
    $a = (Get-FileHash -LiteralPath $Original -Algorithm SHA256).Hash
    $b = (Get-FileHash -LiteralPath $Downloaded -Algorithm SHA256).Hash
    if ($a -ne $b) { throw "SHA-256 mismatch for $Downloaded" }
    $a
}
Assert-SameFile "$originals\fixture.zip" "$dest\fixture.zip"
```

For functional tracing, close the shortcut-launched client first. Launch from this shell and retain its process object. Save test logs before each restart because the client can rotate them.

```powershell
$env:TDESKTOP_PERSISTENT_DOWNLOAD_TRACE = '1'
$client = Start-Process $debugExe -ArgumentList ('-many -debug -workdir "' + $profile + '"') -PassThru
$log = Join-Path $profile 'log.txt'
Select-String -Path $log -Pattern 'download policy|registry|backend direct|backend streaming|request offset|committed offsets|startup resume decision|suspend checkpoint|published|completed history durable'
$jobId = Read-Host 'Job UUID from test log'
Select-String -Path $log -SimpleMatch $jobId
```

## Stage B — automated regression suite

Use NTFS for the temporary test directory with support for hard links and sparse files. The test executable starts its own child process to test abrupt exit.

```powershell
foreach ($cfg in 'Debug','Release') {
    $test = "$out\test_persistent_file_sink\$cfg\test_persistent_file_sink.exe"
    & $test
    if ($LASTEXITCODE -ne 0) { throw "$cfg download tests failed with exit code $LASTEXITCODE" }
}
```

Expect **exit 0** and **Persistent file sink tests passed.** The fixture uses actual sink/registry parsers, file operations and subprocess boundaries. The request, state and presentation tests exercise production helpers. Live UI, Telegram server behavior and encrypted profile integration require Stage C.

## Stage C — extended lifecycle and filesystem tests

Use separate disposable fixtures for these cases. Quit the client before editing a paused file. Record the job UUID and compare the hash after each successful publication.

| Case | Procedure | Pass criteria |
|---|---|---|
| Forced exit and Windows restart | Enable startup resume. Start two saves, pause one and wait for a checkpoint on the other. Terminate the exact test process as shown below, then restart. Repeat with normal Quit, with Windows Restart and with startup resume Off. | Only eligible interruptions resume automatically. Manually paused jobs remain paused. No valid file-consumer range is requested again. |
| Altered chunk and truncation | In a paused stage identified by its UUID, modify one byte in a known committed chunk. In another run truncate within a committed chunk. Resume. The recovery guide has PowerShell byte-edit commands. | Affected chunks become missing. Good ranges remain valid. Hash equals the uninterrupted reference. |
| Changed stage or sidecar | Move a paused stage or checkpoint aside. Put a disposable text sentinel in its place. Restart, Retry and Cancel. Restore the matching original only while the client is closed. | Sentinel remains intact. No unrelated file is published or removed. Cleanup can defer. |
| Occupied destination | Start at an unused name. Pause, create a sentinel at the final path, record its hash and resume. Try a legacy job with prior overwrite approval. Move the sentinel out of the way and retry. | The sentinel and partial progress survive; retry publishes to the now-free name. A new persistent save to an occupied name requests another name. |
| Hard links, junctions, symlinks, missing drive | Create a hard link to a paused stage and try an ordinary save to the alias. Try a destination beneath a disposable junction or symlink. Disconnect and reconnect a disposable external drive. | Ambiguous ownership is refused, sentinel hashes survive, the returned original drive permits recovery. Symlink creation may require Developer Mode or privileges. |
| Account isolation and logout | Two test accounts download equal filenames to distinct paths. Try a conflicting exact destination. Switch accounts mid-batch. Log A out while verifying or finalizing a large file, then sign a different test identity into A's old slot. | Only intended jobs change; no cross-account resumption. Work before the publication boundary can stop. A file whose rename already committed can remain. |
| Network and source | Disconnect the test VM or set an unavailable test proxy, then restore connectivity and Retry. Delete the original message, replace the document, or revoke saving permission in a private test chat. | Temporary failures remain retryable. Confirmed permanent loss stops retries. No permission bypass. Record expired references, flood waits, CDN redirects and variant availability only when observed; these depend on Telegram. |
| Streaming and bulk races | Play and seek during a save, change quality if offered, pause and resume rapidly, cancel the save during playback, then close the player during a different save. Try Resume All immediately followed by Pause All and Cancel Selected as another file finishes. | Playback and file consumers remain independent; correct variant hash, no unrelated cancellations or duplicate history entries. |
| UI, settings, discovery | Use Ctrl/Shift selection, Ctrl+A, Escape, Unicode and punctuation searches, all status filters, mixed selections, current-account actions and the idle indicator. Test default light/dark, custom accent, RTL, narrow windows and 125–200% DPI. Disable the feature during activity, restart, cancel one preserved job, re-enable and restart. | Counts and byte-weighted progress agree with rows. Hidden selection is safe. Idle rows show no transfer speed. Disabled jobs remain reachable from Advanced. |
| Full disk / storage failure | Use a quota-limited volume or small disposable test VHD in a VM. Keep the OS, production data and build tree elsewhere. Fill only that volume during transfer, restore space, then retry. Use the test executable's deterministic replacement failures first. | Last valid checkpoint survives. No partial final file. Failed preference save reports failure. Unwritable logout storage is a documented recovery limitation. |
| Stress and ordinary behavior | Run five and ten simultaneous transfers, twenty pause/resume/cancel cycles, seek during transfer, revisit Downloads and switch accounts. Test ordinary documents, local playback, caching, thumbnails, web media and completed-history actions after restarting with the feature off. | Handles, private bytes and threads settle after queues drain. No new persistent jobs for unsupported or ordinary saves. Account limit is 64 pending jobs. Use the 4096-record fixture for synthetic filtering; time actual large history UI independently. |

```powershell
$stage = Read-Host 'Full disposable .part path from the job UUID'
if ((Split-Path -Parent (Resolve-Path -LiteralPath $stage).Path) -ne $dest) { throw 'Wrong fixture folder' }
New-Item -ItemType HardLink -Path "$dest\alias.bin" -Target $stage
$real = New-Item -ItemType Directory -Path "$lab\junction-target"
New-Item -ItemType Junction -Path "$dest\test-junction" -Target $real.FullName
```

To simulate a crash, use only the process object returned by your isolated launch. Never terminate clients by image name.

```powershell
$live = Get-CimInstance Win32_Process -Filter ("ProcessId = " + $client.Id)
if (!$live -or $client.HasExited -or $live.ExecutablePath -ne $debugExe -or
    !$live.CommandLine.Contains('-workdir "' + $profile + '"')) { throw 'Test client identity changed' }
$client.Kill()
$client.WaitForExit()
```

Compare one job's `committed offsets` ranges and `request offset` lines in the resumed session. Each pair is inclusive, at 131072-byte intervals. Only missing offsets may appear as file requests. Independent playback or shared network traffic can legitimately read a committed offset. For detailed step-by-step source, crash, variant and UI cases see the [recovery](persistent-download-recovery-windows.md), [streaming](persistent-streaming-downloads-windows.md), [panel](persistent-download-manager-windows.md) and [settings](persistent-download-settings-windows.md) guides.

## Stage D — performance and resources

Compare Release x64 A (pre-feature `3aee543e0bde78dc9a370d24349c8a6468973c7e`), B (Milestone 5 `41710ac9a025fa14cfe56205b8580f097ecb8012`, record on/off), C (M6 off) and D (M6 on). Use archived comparable binaries or manually create isolated detached checkouts for historical SHAs with their own build directories and matching dependency revisions. For example, `git worktree add --detach D:\TBuild\tdesktop-pre-downloads 3aee543e0bde78dc9a370d24349c8a6468973c7e` creates a sibling source tree without switching dev. Manually provision its pinned submodules and use the project's Windows build instructions. Never share configured output between revisions. Mark a missing baseline comparison unavailable.

Keep exact media, Windows power mode, drive, compiler, Qt, account tier, DC, network, antivirus policy and concurrent tasks comparable. Use isolated equivalent profiles for cold-cache comparisons and separate warm-cache runs. Record OS cache state. Alternate run order, perform at least three runs and report median and min–max. Capture interruptions and observed server rate limits.

Run single 100 MiB, 1 GiB, >2 GiB and near-4000 MiB cases; 5/10 concurrent saves; playback plus save; several paused jobs opening together; a roughly 90%-complete resume; and a large Downloads history. Record Save/Resume click, first accepted byte, publication, seek-to-next-frame, UI response and Quit-to-process-exit. Compare sustained and end-to-end throughput separately. Use the smaller fixtures for repetitive coverage. The final readback intentionally incurs one full sequential content read.

```powershell
foreach ($flag in $flags) { Remove-Item "Env:$flag" -ErrorAction SilentlyContinue }
$client = Start-Process $releaseExe -ArgumentList ('-many -workdir "' + $profile + '"') -PassThru
& "$repo\tools\measure-persistent-downloads.ps1" -ProcessId $client.Id -ExpectedExecutable $releaseExe -OutputCsv "$lab\M6-on-run1.csv" -Seconds 300
```

The sampler observes one verified process, creates a fresh CSV and never controls the client. Use a one-second interval by default. It records CPU seconds and machine-normalized CPU percentage, working set, lifetime peak working set, private bytes, handles, threads and cumulative process I/O bytes. Subtract the first counter from the last. Process I/O can include cache and device/network traffic. For physical disk attribution use WPR. For leaks compare steady-state samples before and several minutes after the same work drains. Sampling can miss short peaks and consumes CPU; compare like-for-like runs.

For a separate diagnostic run, set `$env:TDESKTOP_PERSISTENT_DOWNLOAD_PERF='1'` before launch. Keep per-chunk trace unset. Filter using `Select-String -Path $log -Pattern 'Download perf'`. Logs include logical checkpoint bytes, newly accepted data bytes in the batch, flush/total checkpoint time, bytes verified successfully and verification duration, publication result and duration, registry saves and the per-loader in-flight high-water mark on destruction. Divide total logical checkpoint bytes by unique downloaded bytes for metadata write amplification. Record wall-clock resume latency separately. These values exclude filesystem bookkeeping. Use uninstrumented throughput for the gate.

Optional WPR/WPA: in an elevated test shell, run `wpr -status` and `wpr -profiles`. Leave any existing recording alone. For a short separate run, use `wpr -start GeneralProfile -filemode`, then `wpr -stop "$lab\M6-run1.etl"`. In WPA inspect CPU Usage, Disk Usage, File I/O and UI-thread scheduling. Keep ETLs private; they can contain local paths. [Microsoft WPR reference](https://learn.microsoft.com/en-us/windows-hardware/test/wpt/wpr-command-line-options)

| Build, mode, scenario, run | Cache, storage, tier/DC | New bytes / seconds / MiB/s | CPU median / peak | Working set / private peak | Read/write deltas / amplification | Checkpoint / verify / resume ms | UI / seek / quit ms | Handles / threads before and after | Result, variation |
|---|---|---|---|---|---|---|---|---|---|
| A / B / C / D | | | | | | | | | |

Provisional targets: typical sustained overhead around 5% or less under controlled conditions. Investigate repeated regressions above 10%. Fail on corrupted output, repeated requests for verified file-consumer chunks, unbounded resource growth, persistent leaks, indefinite shutdown or repeatable playback/UI stalls. Record integrity and durability costs separately. No performance result is available yet.

## Stage E — upgrades and release packaging gates

Prepare the portable beta from a clean checkout or detached worktree of the final release commit, with a separate build directory and pinned submodules. Commit any chosen version changes before building and tag the same commit at publication. For a manual-only beta, distribute the portable artifact through a GitHub prerelease and leave stable-feed metadata and update packages alone. The GitHub prerelease flag is independent of the application's update channel.

1. Quit an isolated M5 client with a paused ZIP and interrupted streaming job. Back up its entire disposable profile and matching destination directory while closed. Keep the original absolute paths when restoring. Record reference hashes and committed ranges.
2. Open M6. Restore the two old jobs, create a new one, quit, and restart. Test the mixed registry, manual pause, completion deduplication and unrelated settings. A v1 sidecar without an account registry entry has no automatic discovery. Old unkeyed data requires a trusted destination.
3. For downgrade testing use a disposable backup. A registry containing new keyed jobs and their v2 sidecars cannot be managed by M5. Return to the compatible client using the intact profile and staging files. Avoid concurrent writers or testing on production account data.
4. Repeat Stage A in Release using a normal shortcut. Where an already authorized test installer or update is available, test installed/portable launch, updater restart, Windows startup and tray/minimized launch in an isolated VM. Expect the same account identity, settings, intent, hashes and history. Check expected NTFS zone information when Group Policy enables marking with `Get-Content -LiteralPath "$dest\fixture.zip" -Stream Zone.Identifier`.
5. Record results in the [release checklist](persistent-downloads-release-checklist.md). Signing, packaging and distribution are separate, pending actions.

If a test fails, retain the job UUID, trace excerpt, hashes, profile and corresponding partial files together. Quit only the isolated client. Cancel recognized test jobs through Downloads. Inspect ambiguous artifacts individually before removing disposable fixtures; release test links separately from their targets. Keep the production profile, originals and build outputs intact.
