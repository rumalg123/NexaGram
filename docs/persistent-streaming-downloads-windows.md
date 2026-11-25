# Persistent streaming downloads on Windows

Use a separate test account and test profile. This guide begins with the playback and recovery checks, then covers failure cases and regressions. The existing [direct download recovery guide](persistent-download-recovery-windows.md) covers the shared registry and additional account and storage tests.

## Build and run the standalone tests

Open `D:\TBuild\tdesktop\out\Telegram.slnx` in Visual Studio 2022. Select **Debug | x64**. Build **Telegram** and then **test_persistent_file_sink** in the **(tests)** group. The solution may update its project files when Visual Studio sees the new sources. Keep the existing build directory and dependencies.

- Client executable is `D:\TBuild\tdesktop\out\Debug\64Gram.exe`.
- Test executable is `D:\TBuild\tdesktop\out\test_persistent_file_sink\Debug\test_persistent_file_sink.exe`.

```powershell
$root = 'D:\TBuild\tdesktop'
$exe = Join-Path $root 'out\Debug\64Gram.exe'
$tests = Join-Path $root 'out\test_persistent_file_sink\Debug\test_persistent_file_sink.exe'
$result = Start-Process -FilePath $tests -Wait -PassThru -WindowStyle Hidden
if ($result.ExitCode -ne 0) { throw "Tests exited with code $($result.ExitCode)" }
$result.ExitCode
```

Expect exit code `0`. The deterministic fixtures check generation changes, shared request ownership, duplicate and stale deliveries, out-of-order writes, short cache parts, resumed holes, SHA-256, and offsets around 2 GiB and just below the 4,000 MiB limit. The existing fixtures cover disk faults, checkpoint corruption, account identity, abrupt exit, and publication recovery. Use an NTFS temporary directory for the sparse-file fixture. The standalone tests exercise the scheduler's ownership rules and the sink with synthetic deliveries. Use the client checks below for player and network behavior.

## Prepare an isolated profile and fixtures

```powershell
$lab = Join-Path $env:LOCALAPPDATA '64Gram-Persistent-Streaming-Test'
$profile = Join-Path $lab 'profile'
$originals = Join-Path $lab 'originals'
$dest = Join-Path $lab 'downloads'
New-Item -ItemType Directory -Force -Path $profile, $originals, $dest | Out-Null

$env:TDESKTOP_EXPERIMENTAL_RESUMABLE_DOWNLOADS = '1'
$env:TDESKTOP_PERSISTENT_DOWNLOAD_TRACE = '1'
$env:TDESKTOP_EXPERIMENTAL_RESUME_ON_STARTUP = '0'

function Start-StreamingTestClient {
    Start-Process -FilePath $exe -ArgumentList ('-many -debug -workdir "{0}"' -f $profile) -PassThru
}

$log = Join-Path $profile 'log.txt'
$client = Start-StreamingTestClient
```

Sign in to the test account. Use **Settings → Advanced** to ask where to save files, then select `$dest` on each download. Upload an uncached H.264/AAC MP4 and an MP3 or M4A to Saved Messages or a private test chat. Preserve the original files separately. Start with an MP4 of at least 100 MiB and several minutes of audio so there is time to interact. Use a 2.1–3 GiB video for the signed-offset boundary case if your test account supports the upload. Keep separate messages for destructive tests. Upload without compression when comparing against the uploaded file's hash. If Telegram transforms the media for a quality variant, save an uninterrupted copy of the **same variant** and use that file as the reference.

Use existing non-sensitive media or create synthetic fixtures with an already installed FFmpeg. The commands generate a test pattern and a tone and run independently of the application build.

```powershell
$video = Join-Path $originals 'test-pattern.mp4'
$audio = Join-Path $originals 'test-tone.m4a'
ffmpeg -f lavfi -i 'testsrc2=size=1280x720:rate=30' -f lavfi -i 'sine=frequency=440:sample_rate=48000' -t 480 -c:v libx264 -b:v 4M -c:a aac -movflags +faststart $video
ffmpeg -f lavfi -i 'sine=frequency=440:sample_rate=48000' -t 600 -c:a aac -b:a 192k $audio
Get-FileHash -LiteralPath $video,$audio -Algorithm SHA256
```

Quit the isolated client before changing feature switches. Launch a fresh process from the same PowerShell. To turn all experimental switches off, run the following commands.

```powershell
Remove-Item Env:TDESKTOP_EXPERIMENTAL_RESUMABLE_DOWNLOADS -ErrorAction SilentlyContinue
Remove-Item Env:TDESKTOP_PERSISTENT_DOWNLOAD_TRACE -ErrorAction SilentlyContinue
Remove-Item Env:TDESKTOP_EXPERIMENTAL_RESUME_ON_STARTUP -ErrorAction SilentlyContinue
```

## Confirm the backend and inspect progress

Start an explicit **Save As** or **Download** operation for the video. In Downloads, right-click the in-progress row for **Pause download**, **Resume download**, or **Cancel download**. For concurrent playback, open the video or start the audio in the same test client while the transfer is active. A playback-only load may show buffering but does not register a persistent job.

```powershell
Select-String -LiteralPath $log -Pattern 'backend streaming|backend direct|stream attach'
Get-ChildItem -LiteralPath $dest -Filter '.64gram-*.part*' -Force
$job = 'paste-job-guid-without-braces'
Select-String -LiteralPath $log -SimpleMatch $job
$attachment = 1
Select-String -LiteralPath $log -SimpleMatch $job |
    Where-Object { $_.Line -match "(stream attach|stream detach|attachment)\s+$attachment\b" }
Select-String -LiteralPath $log -SimpleMatch $job |
    Where-Object { $_.Line -match 'stream network|cache delivery|network delivery|stream download attachment' }
```

Expect `backend streaming` and `stream attach <id>` for the shared downloader. A native document on the direct loader reports `backend direct`. Choose another video or audio fixture if there is no streaming backend event. A cached or local file may complete through the existing local save path without a persistent job. Attachment numbers are allocated per reader and may start again in a new process. The persistent GUID identifies one job across processes.

| Trace text | Meaning |
|---|---|
| `job registered`, `stream reader created` or `stream reader reused`, `backend streaming` | The registry reserves the job before download chunks are accepted. |
| `stream attach`, `stream detach`, `stream stale delivery` | Resume gets a fresh attachment within the same reader. A queued callback for an old attachment is rejected. Stale delivery is timing-dependent. |
| `stream download attachment <id> offset <offset>` | The file-download consumer selected that missing offset. Use this line when checking for re-downloads of committed chunks. |
| `stream network download`, `stream network playback`, or `stream network shared` | Logical requests into the shared loader. A logical shared request may reuse one queued or in-flight MTProto request. |
| `cache delivery`, `network delivery`, `accepted write offset` | A validated part reached the file consumer or was written to staging. A write is durable after a successful checkpoint. |
| `committed offsets <first> <last>`, `checkpoint committed` | The sink verified or persisted coverage. Endpoints are inclusive with a spacing of 131072 bytes. |
| `source and permissions validated`, `startup resume decision`, `suspend checkpoint` | The existing account recovery and pause lifecycle. |
| `job state 7`, `published`, `completed history durable`, `job retired` | Publication and acknowledgement precede cleanup. |

Before completion, expect `.64gram-<job-guid>.part` and `.64gram-<job-guid>.part.resume` under `$dest`. The final path is absent or holds the previous explicitly approved overwrite target. After completion, expect one final file, one completed row, and removal of the staging pair after history acknowledgement. Keep the first run's `log.txt` in `$lab` if you need to compare runs, because each launch recreates it.

## Essential acceptance tests

Start a fresh fixture for each case. For the first normal recovery case leave startup resume at `0`.

| Case | UI steps | Pass criteria |
|---|---|---|
| Pause while playback runs | Save the video, open its player, wait for the first checkpoint, then use **Pause download** from Downloads. Seek to an unbuffered part and change playback speed while paused. Resume from Downloads. | Playback remains usable without resetting position or speed. The file consumer detaches, keeps staging, and makes no new download-consumer requests while paused. Resume validates the source and attaches again. The final SHA-256 matches. |
| Cancel while playback runs | Start another file transfer and play the source. Choose **Cancel download** for that transfer. Seek again while the player is open and restart the app afterward. | Playback continues. Cancel retires the GUID and removes its verified staging artifacts. The pending job does not return after restart. |
| Close playback while downloading | Begin playback and download concurrently. Close the video viewer or stop audio playback with the playback controls. Leave the app open. | The same file consumer continues requesting chunks and completes. Normal completion creates one row. |
| Normal exit and restart | While downloading, wait for a checkpoint and choose **Quit 64Gram** from the tray or press **Ctrl+Q**. Wait for the test PID to exit. Launch the same profile, open Downloads, and resume the interrupted row without opening the video. | The row and GUID return. Playback is unnecessary for recovery. Verified committed offsets are skipped and SHA-256 matches. |
| Pass through Windows restart | Start a fresh transfer, checkpoint and quit normally, then restart Windows. Reopen PowerShell, restore the variables above, and launch the same profile path. Resume from Downloads. | The job and valid staging progress survive. Completion has the expected hash and one history entry. |
| Forced termination | Wait for a checkpoint and stop only the isolated PID using the command below. Relaunch and resume without opening the player. | The job restores from the last successful checkpoint. The client may request chunks written since that checkpoint again. The final hash matches. |
| Automatic startup recovery | Quit while a different job is running. Set `$env:TDESKTOP_EXPERIMENTAL_RESUME_ON_STARTUP = '1'` and launch the same profile. | Observe an automatic resume decision followed by validation and streaming attachment. |
| Manual pause across multiple restarts | Enable automatic resume, explicitly pause an active job, and quit. Launch twice without clicking Resume. On the second launch, choose **Resume download**. | The job stays paused in both launches. The file consumer starts only after the explicit action. |

Verify the completed SHA-256.

```powershell
$original = Join-Path $originals 'test-pattern.mp4'
$completed = Join-Path $dest 'test-pattern.mp4'
$expected = (Get-FileHash -LiteralPath $original -Algorithm SHA256).Hash
$actual = (Get-FileHash -LiteralPath $completed -Algorithm SHA256).Hash
if ($actual -ne $expected) { throw 'Final SHA-256 mismatch' }
$actual
```

To check missing offsets, use a fresh starter log for one recovered job and one attach in that run. Choose a fixture with both committed and missing chunks. The command compares verified committed ranges against **file-download consumer** requests. Playback and shared loader traces are useful for explaining traffic but can legitimately mention any offset.

```powershell
$lines = Select-String -LiteralPath $log -SimpleMatch $job
$ranges = foreach ($line in $lines) {
    if ($line.Line -match 'committed offsets\s+(\d+)\s+(\d+)') {
        [pscustomobject]@{ First = [int64]$Matches[1]; Last = [int64]$Matches[2] }
    }
}
$requests = foreach ($line in $lines) {
    if ($line.Line -match "stream download attachment\s+$attachment\s+offset\s+(\d+)") {
        [int64]$Matches[1]
    }
}
if ($null -eq $ranges -or $null -eq $requests) { throw 'Choose a run with both committed ranges and missing requests' }
$repeated = foreach ($offset in $requests) {
    foreach ($range in $ranges) {
        if ($offset -ge $range.First -and $offset -le $range.Last) {
            $offset
            break
        }
    }
}
if ($null -ne $repeated) { throw "Requested committed download offsets $($repeated -join ', ')" }
```

To force-stop only the client launched by this guide, verify both its executable and profile command-line argument. If the app relaunched itself, repeat the launch with the saved process handle before testing this case.

```powershell
$info = Get-CimInstance Win32_Process -Filter "ProcessId=$($client.Id)"
if (!$info -or $info.ExecutablePath -ne $exe -or
    !$info.CommandLine -or !$info.CommandLine.Contains($profile)) {
    throw 'The PID does not identify this isolated profile'
}
Stop-Process -Id $client.Id
$client.WaitForExit()
```

## Extended regression tests

| Case | Steps | Pass criteria |
|---|---|---|
| Seeking under load | While the video downloads, seek repeatedly between distant timestamps and play through a buffer refill. Then let playback and download run together. | Playback resumes at each selected timestamp. The download continues to make progress with the same job and variant. A shared offset can satisfy both consumers. |
| Local corruption or truncation | Quit after a checkpoint. Choose a committed offset and flip one byte in the test staging file with the command below. For a separate job, truncate into a committed chunk. Relaunch and Resume. | The affected chunks are re-requested. The other verified offsets remain committed. The final hash matches. |
| Large file | Save a streamable 2.1–3 GiB video. After a restart or pause, let it finish. Search for request offsets at or above `2147483648`. | Offsets remain positive and progress reaches the exact expected byte count. SHA-256 matches. The standalone fixture covers the final non-aligned part near 4,000 MiB. |
| Playback-only and local media | Play a fresh video or audio file without saving. Also play an already saved test file. | Playback works. There is no new GUID or staging pair, and buffering does not create a resumable row. |
| Direct MTProto compatibility | Save an uncached ZIP or BIN document. Pause, quit, then resume it. | The job reports `backend direct` and retains the previous recovery behavior. |
| Feature disabled | Pause an eligible job, quit, remove all three variables, then reopen the same test profile. Play a fresh file and use an ordinary download. Re-enable the variables and reopen. | Ordinary playback and downloads still work, and the earlier pending artifacts survive for recovery after re-enabling. |
| Access restrictions and account isolation | Try protected or ephemeral source media in an authorized test chat. Use two test accounts for same-name files, then log one out. | Protected and ephemeral files create no persistent job. Logout retires its own jobs while the other account's jobs remain recoverable. |
| Publication and storage failures | Use the destination collision, locked artifact, disk-full and corrupt checkpoint cases in the direct download recovery guide with a streaming fixture. | The same shared transaction rules apply. An unrelated output is retained, recognized staging can be retried, and a recovered completed file gets one history entry. |

For corruption testing, run this only after the isolated test process has exited. Use the exact GUID and a verified committed offset in the lab folder.

```powershell
$stage = Join-Path $dest ".64gram-$job.part"
$offset = [int64]131072
$handle = [IO.File]::Open($stage, 'Open', 'ReadWrite', 'None')
try {
    $handle.Position = $offset
    $byte = $handle.ReadByte()
    if ($byte -lt 0) { throw 'Choose a valid committed offset in this staging file' }
    $handle.Position = $offset
    $handle.WriteByte([byte]($byte -bxor 1))
    $handle.Flush($true)
} finally {
    $handle.Dispose()
}
```

For truncation, use a separate fixture and call `$handle.SetLength($newLength)` with a length that cuts through a committed chunk. Open and dispose the same isolated handle as above.

## Telegram-dependent tests

- **Quality switching** requires Telegram to offer multiple qualities for the same source. Start a download, switch qualities in the player, seek, then finish the download. The job must keep the selected download document and expected byte count. Compare against an uninterrupted copy of that variant. A variant unavailable on restart cannot be replaced with a different one.
- **CDN redirects and expired references** depend on server behavior. If observed, completion should use the normal reference refresh or CDN verification path and retain the same GUID and hash.
- **Missing or revoked source** requires a test message that another test account can delete, replace, or restrict. The recovered job should send no chunk requests after a confirmed source rejection. Test a temporary network disconnect separately to verify that it remains retryable.

## Cleanup and failure evidence

Capture relevant lines for the GUID and attachment, whether there was playback or a seek, the backend, the phase of any crash, the final hash, and the staging and checkpoint sizes. Keep the isolated profile until the failure is understood. Use Cancel in the isolated client to retire a pending job. Close the verified test process before archiving any test files. Your production profile stays outside the lab.

The same persistence limits as Milestone 2 apply to write failures and sudden power loss. Resume verifies the staging identity and committed hashes, and the application can recover only the data and metadata that survived.
