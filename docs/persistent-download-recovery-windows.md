# Persistent download recovery on Windows

Use a separate 64Gram profile, an authorized test account, and non-sensitive files. Use a second test account for the isolation checks. Keep personal profiles and production download folders outside these tests. All commands below run in PowerShell.

## Build the existing solution

1. Open `D:\TBuild\tdesktop\out\Telegram.slnx` in Visual Studio 2022. Select **Debug** and **x64**.
2. Build the **Telegram** project. Allow the solution's normal CMake regeneration if Visual Studio requests it for the new source entries. Keep the existing build tree and dependencies.
3. Build the **test_persistent_file_sink** project in the **(tests)** group separately. This project is excluded from the default build.
4. The client is `D:\TBuild\tdesktop\out\Debug\64Gram.exe`. The test program is `D:\TBuild\tdesktop\out\test_persistent_file_sink\Debug\test_persistent_file_sink.exe`.

Run the standalone suite after building the test project.

```powershell
$root = 'D:\TBuild\tdesktop'
$test = Join-Path $root 'out\test_persistent_file_sink\Debug\test_persistent_file_sink.exe'
$testProcess = Start-Process -FilePath $test -Wait -PassThru -WindowStyle Hidden
$testProcess.ExitCode
if ($testProcess.ExitCode -ne 0) { throw "Persistent download tests failed" }
```

Expect exit code 0. The suite uses isolated temporary directories, including a subprocess that exits without unwinding to test checkpoint recovery. The sparsely allocated test beyond 2 GiB requires a filesystem that supports sparse files, such as NTFS. The test project checks the real registry codec and atomic replacement with opaque fixtures. It exercises staging and publication recovery at deterministic boundaries. The connected client scenarios below exercise actual account encryption, Telegram source checks, and application startup and shutdown.

## Prepare an isolated client

```powershell
$root = 'D:\TBuild\tdesktop'
$exe = Join-Path $root 'out\Debug\64Gram.exe'
$lab = Join-Path $env:LOCALAPPDATA '64Gram-Persistent-Recovery-Test'
$profile = Join-Path $lab 'profile'
$dest = Join-Path $lab 'downloads'
$fixtures = Join-Path $lab 'originals'
New-Item -ItemType Directory -Force -Path $profile, $dest, $fixtures | Out-Null

$env:TDESKTOP_EXPERIMENTAL_RESUMABLE_DOWNLOADS = '1'
$env:TDESKTOP_PERSISTENT_DOWNLOAD_TRACE = '1'
$env:TDESKTOP_EXPERIMENTAL_RESUME_ON_STARTUP = '0'

function Start-RecoveryClient {
    Start-Process -FilePath $exe -ArgumentList ('-many -debug -workdir "{0}"' -f $profile) -PassThru
}
$client = Start-RecoveryClient
```

Sign into the test account in this profile. In **Settings → Advanced**, turn on asking where to save files or point the download path at your test destination. Upload at least two distinctly named ZIP or BIN files as **files** to Saved Messages or a test chat. Keep the originals in `$fixtures` and save into `$dest`. Use a 100–300 MiB fixture for most checks. Use 500 MiB–1 GiB on a fast connection so there is time to stop mid-download. Use a 2.1–3 GiB fixture for the large-file case if your test account can upload it and the destination has room. A file whose size is a few bytes past a 128 KiB boundary also checks a short last chunk. Choose a plain file attachment and an explicit **Save As** or **Download** action. A video playback download or an already cached file can take a different path.

Set `$env:TDESKTOP_EXPERIMENTAL_RESUME_ON_STARTUP = '1'` before launching to test automatic restart. Each new process inherits its launch shell's environment. To disable all three switches, exit this test client first and run the following commands.

```powershell
Remove-Item Env:TDESKTOP_EXPERIMENTAL_RESUMABLE_DOWNLOADS -ErrorAction SilentlyContinue
Remove-Item Env:TDESKTOP_PERSISTENT_DOWNLOAD_TRACE -ErrorAction SilentlyContinue
Remove-Item Env:TDESKTOP_EXPERIMENTAL_RESUME_ON_STARTUP -ErrorAction SilentlyContinue
```

## Observe artifacts and logs

While a persistent transfer is incomplete, expect an application-owned `.64gram-<job-id>.part` and adjacent `.64gram-<job-id>.part.resume`. The final path should be absent, or still contain the previously approved overwrite target. After successful publication and history acknowledgement, expect the final file and one completed Downloads row. The staging file and sidecar should be gone.

```powershell
$log = Join-Path $profile 'log.txt'
Get-ChildItem -LiteralPath $dest -Force | Select-Object Name, Length
Select-String -LiteralPath $log -SimpleMatch 'Persistent download'
$job = 'paste-the-job-guid-without-braces-here'
Select-String -LiteralPath $log -SimpleMatch $job
```

The registry lives below the isolated profile's account data, in a `pending-downloads` subdirectory with a hexadecimal authenticated account and environment identifier as the basename. The registry is encrypted. Use this read-only discovery command to locate it.

```powershell
Get-ChildItem -LiteralPath (Join-Path $profile 'tdata') -Recurse -File |
    Where-Object { $_.Directory.Name -eq 'pending-downloads' } |
    Select-Object FullName, Length, LastWriteTimeUtc
```

The log is recreated at launch. Copy it into the lab directory before restarting when you need before-and-after evidence. The stable GUID appears in the staging and checkpoint filenames and in log entries. Registry-wide messages use the null GUID. `-debug` makes Qt informational diagnostics available in `log.txt`.

| Expected trace | What to check |
|---|---|
| `registry loaded, account validated` and `job registered` | A saved job has an account record before the initial request offsets appear. |
| `created`, `checkpoint`, `reopened`, `staging recovery` | Checkpoint entries include committed and missing counts. Startup first verifies the checkpoint against the staging file. |
| `job state 2`, `job state 3`, `job state 4` | The persisted states are Downloading, Paused, and Interrupted. State 1 is Initializing, 5 is Retryable, 6 is Unavailable, 7 is Finalizing, and 8 is Cancelling. |
| `startup resume decision 0` or `1` | Only an eligible interrupted job restarts automatically. |
| `resolve source`, `source and permissions validated` | A fresh source check precedes requests on a recovered or manually resumed job. |
| `committed offsets <first> <last>`, `request offset <offset>` | Ranges are inclusive and their members are spaced 131072 bytes apart. |
| `shutdown suspending jobs`, `suspend checkpoint 0` | Normal quit attempts a final checkpoint for active jobs. On failure, inspect the prior successful checkpoint. |
| `job state 7`, `published`, `completed history durable`, `publication cleanup`, `job retired` | The history acknowledgement precedes removing the pending record. |
| `job state 8`, `cancel cleanup`, `cancel cleanup deferred`, `job retired` | The job is terminal before files are removed. A failed cleanup may leave a tombstone. |
| `source retryable`, `source unavailable`, `logout retirement durable`, `logout cleanup` | Use the state and job ID to distinguish temporary failures, revoked access, and retirement. |

Verify the completed file SHA-256.

```powershell
$original = Join-Path $fixtures 'sample.zip'
$final = Join-Path $dest 'sample.zip'
$left = Get-FileHash -LiteralPath $original -Algorithm SHA256
$right = Get-FileHash -LiteralPath $final -Algorithm SHA256
$left.Hash
$right.Hash
if ($left.Hash -ne $right.Hash) { throw 'SHA-256 mismatch' }
```

For an ordinary restart of an unfinished job, run this after it has begun requesting missing chunks. Use a fresh startup log and one Resume attempt for this job. For corrupted chunks, expect the corrupted offset to be absent from the accepted ranges and present in the new requests.

```powershell
$jobLines = Select-String -LiteralPath $log -SimpleMatch $job
$ranges = foreach ($line in $jobLines) {
    if ($line.Line -match 'committed offsets\s+(\d+)\s+(\d+)') {
        [pscustomobject]@{ First = [int64]$Matches[1]; Last = [int64]$Matches[2] }
    }
}
$requests = foreach ($line in $jobLines) {
    if ($line.Line -match 'request offset\s+(\d+)') { [int64]$Matches[1] }
}
if (!$ranges -or $null -eq $requests) { throw 'Choose a restart run with committed ranges and missing requests' }
$repeated = foreach ($offset in $requests) {
    foreach ($range in $ranges) {
        if ($offset -ge $range.First -and $offset -le $range.Last) {
            $offset
            break
        }
    }
}
if ($null -ne $repeated) { throw "Requested committed offsets $($repeated -join ', ')" }
```

## Exit and terminate the test process

For normal exit, choose **Quit 64Gram** from the tray icon or press **Ctrl+Q** in the isolated client. Closing the window can minimize to the tray. Verify that the PID has exited before restarting or editing fixture files.

For forced termination, launch with `$client = Start-RecoveryClient` and check the PID, executable, and isolated profile on its command line. Abort if the client was relaunched and the original PID no longer matches.

```powershell
$processInfo = Get-CimInstance Win32_Process -Filter "ProcessId=$($client.Id)"
if (!$processInfo -or $processInfo.ExecutablePath -ne $exe -or
    !$processInfo.CommandLine -or !$processInfo.CommandLine.Contains($profile)) {
    throw 'The PID does not identify the isolated recovery client'
}
Stop-Process -Id $client.Id
$client.WaitForExit()
```

Never select a process by name alone.

## Runtime acceptance cases

Start a separate fixture for each destructive case. Changes to test files below are made only after verifying that the isolated client is offline.

| Scenario | UI actions or controlled fault | Pass criteria |
|---|---|---|
| New transfer and pause | Save a fresh fixture to the lab. After the first checkpoint, right-click its Downloads row and choose **Pause download**. Then choose **Resume download**. | Staging and sidecar persist. No final file appears during the pause. The same GUID continues. Verify SHA-256. |
| Normal application exit with manual recovery | Set startup resume to `0`. While a fresh job is running and has a checkpoint, choose **Quit 64Gram** and relaunch from the same PowerShell. Open **Downloads** from the search UI or download tray. Right-click the interrupted row and choose **Resume download**. | No requests while waiting for Resume. Shutdown logs a suspension attempt. Resume validates the source and requests only missing offsets. Exactly one completed entry appears. |
| Resume from original chat | Repeat the previous test. Return to the original message and select its **Download** action instead of the Downloads row. | The same GUID and staging pair are reused. One completion and no duplicate job. |
| Forced process exit | Wait for one or more successful checkpoints. Force stop only the test process with the PID check. Relaunch and Resume. | The row returns on startup. Verified committed offsets are skipped. Data written after the last durable checkpoint may be requested again. |
| Windows restart | Start a fresh eligible job, wait for a checkpoint, quit normally, then restart Windows. Open PowerShell and run **Prepare an isolated client** again using the same profile path. | The job remains available and completes with the original hash. The test profile and sources were retained. |
| Automatic startup resume | Start a fresh job. Quit normally while running. Before relaunch set `TDESKTOP_EXPERIMENTAL_RESUME_ON_STARTUP` to `1`. | The startup decision is `1`, then source validation and requests occur without a click. |
| Manual pause across restarts | Explicitly pause a fresh job, quit, set automatic resume to `1`, and relaunch twice. On the second launch select **Resume download**. | The job stays Paused across both launches. There are no request offsets until the explicit Resume. |
| Temporary network failure | Restore a paused fixture and use an isolated network or test VM to deny the test client's connectivity. Choose **Resume download** and wait for source lookup to time out. Restore connectivity and choose **Resume download** again. | The first attempt has no chunk requests and logs a retry. The second uses the same GUID and verifies the source. A manually paused job retains its manual intent until a successful Resume. |
| Corrupted chunk | Pause or quit after capturing a committed range. With the client closed, open the exact staging file and flip one byte in that range using the command below. Relaunch and Resume. | The damaged chunk is missing from committed ranges and requested again. Other verified committed offsets are skipped. Final SHA-256 matches. |
| Truncated staging file | With the client closed, shorten the staging file to cut through a known committed chunk. Relaunch and Resume. | Coverage beyond the new extent is dropped. Earlier valid offsets remain committed. Final SHA-256 matches. |
| Missing stage, missing checkpoint, or corrupt checkpoint | On three independent stopped fixtures, respectively move the stage aside, move the sidecar aside, or keep a copy and replace the sidecar with incomplete test bytes. Relaunch and try Resume. | The saved job remains visible. There is no recreated known staging file, no trust in file length, and no final output. Put the original artifact back in place with the client stopped and try again. |
| Replaced staging file | With the test client stopped, rename the actual staging file within its directory. Put an unrelated file of the same length at the old staging name. Resume or Cancel. | The replacement is rejected and the unrelated file survives. Restore the original filename while the client is stopped before retrying. |
| Destination collision | During a fresh download that did not approve overwrite, create a non-sensitive sentinel at the exact final path. Let the job reach finalization. Move the sentinel aside, then choose Resume after restart or from the failed transfer row. | The sentinel is never overwritten. A full valid partial remains retryable. The retry needs zero network chunks if all committed chunks verify. |
| Final rename failure or failed storage write | Use a small isolated test volume or a test handle that prevents rename. Keep enough space on the profile volume for job metadata, wait for the error, release the lock or free the test space, then Resume. | No unrelated file is replaced. Previously committed chunks are reused. No `completed history durable` appears before publication. |
| Finalization crash windows | The standalone test deterministically exercises the disk state immediately before and after rename. In the client, observe `job state 7` or `published` and force stop around those boundaries. Repeat with fresh fixtures if the process finishes cleanup first. | Before rename, the complete partial can finish with no missing requests. After rename, one completed row is reconciled without redownloading or replacing the final file. This UI timing test is probabilistic. |
| Cancellation during download and during cleanup | Pause or run an eligible job. Right-click its Downloads row and choose **Cancel download**. Test ordinary completion once. Repeat while holding a test handle on the staging file to delay cleanup and then force-stop or quit. Release the handle before relaunch. | Cancellation persists once state 8 is logged. The job never appears or requests chunks again. Startup removes only verified owned artifacts. Failed cleanup leaves a hidden tombstone. |
| Multiple accounts and same filenames | In the isolated profile, add the second test account. Have both save distinct documents named `sample.zip` into separate lab folders. Try choosing the same destination for both while the first is pending. Switch accounts and restart. | Each valid job retains a distinct GUID and account identity. A known conflicting destination is refused. The second account never resumes the first account's job. Existing output is protected if separate jobs converge on the same path. |
| Logout and account slot reuse | With one active and one paused job in the first test account, log it out. Keep the second signed in. After reusing the vacant slot, also try signing in to the original test account again. | Logged-out jobs stop and never resume. Owned partials are cleaned or left under a terminal cleanup state if locked. Jobs for the other account still work. |
| Registry I/O and downgrade behavior | Close the isolated client. Back up one test registry, then test unreadable or corrupt bytes in that isolated copy. Relaunch and attempt a new eligible job. Restore the original after quit. Also launch with all flags removed and then re-enable. | The invalid registry is reported and left in place. Restoring the known copy recovers its jobs. Disabling the feature leaves its files intact and exposes no pending rows. |
| App update with a retained test profile | Leave a pending job. Quit and launch the newer Debug executable with the same isolated work directory and feature flags. | The same job is restored and the final SHA-256 matches after resuming. |
| Ordinary download and supported features | With all flags removed, download a fresh document manually. Enable the flags and test streaming video, thumbnails, automatic caching, and a supported Mini App or web download. Spot-check chat filters, smart/regex selection and forwarding. | Ordinary flows behave as before and do not create experimental jobs. Streaming follows its existing loader. Completed history remains readable. |
| Large-file transfer | Save a file larger than 2 GiB, interrupt and resume. Confirm logs contain offsets above 2147483647. | Offsets are positive and verified. Final exact byte count and SHA-256 match. |

To flip a byte in a committed chunk, set `$offset` from the trace. To truncate a separate fixture, use the same exclusive open and call `$handle.SetLength($newLength)` such that `$newLength` cuts a committed chunk.

```powershell
$stage = Join-Path $dest ".64gram-$job.part"
$offset = [int64]131072
$handle = [IO.File]::Open($stage, 'Open', 'ReadWrite', 'None')
try {
    $handle.Position = $offset
    $value = $handle.ReadByte()
    if ($value -lt 0) { throw 'Choose an offset inside the verified staging file' }
    $handle.Position = $offset
    $handle.WriteByte([byte]($value -bxor 1))
    $handle.Flush($true)
} finally {
    $handle.Dispose()
}
```

## Cases that depend on Telegram

Use a controlled group or channel and a second test account when the server must change access. Upload a plain document, begin a download, pause, and exit the client. From the second account, change one condition before relaunching and resuming.

| Action | Pass criteria |
|---|---|
| Delete the source message or replace its attachment with a different document | `source unavailable` is logged. No chunk request follows. The recovered row can be cancelled. |
| Revoke the test account's access or turn on content protection | Source validation fails when the changed permissions are returned by Telegram. No chunk requests are issued. |
| Resume after a file reference expires | Message lookup obtains a fresh reference, or normal MTProto file-reference refresh handles a later expiry. The job retains its GUID and skips verified chunks. Feature timing depends on Telegram's server. |
| Trigger CDN redirection, when Telegram serves the fixture through CDN | The existing CDN validation applies. No CDN state persists across restarts. The completed SHA-256 matches. |

## Persistence boundaries

Registry version 1 uses the existing local account encryption and a bounded record list. Its owner includes the authenticated user ID and Telegram network environment. A job records the original message, document, exact byte count, destination, staging identity, timestamps, and intent. The job UUID and destination determine both staging paths. Version 1 supports the original document byte stream. The sidecar holds the chunk bitmap and SHA-256 values. The completed-download history keeps its existing encoding.

| Durable state | Recovery |
|---|---|
| Initializing with no registered file identity | If both artifacts are absent, a manual Resume can create the staging file with exclusive creation. If a file was created before its identity was saved, it stays unclaimed. |
| Initializing with a recorded identity | Resume can create an initial sidecar for an existing, identity-matched, empty staging file when the sidecar is absent. A missing stage with a recorded identity stays missing. |
| Downloading or Interrupted | Verify staged chunks. Startup can resume after source validation when the automatic switch is enabled. |
| Paused | Preserve the manual pause intent. A user Resume checks the source again. |
| Retryable | Preserve the latest complete checkpoint. Retry from Downloads after fixing the storage or network problem. |
| Unavailable | Keep the job visible for cancellation. Skip automated retries after a confirmed source rejection. |
| Finalizing | If all staged bytes verify, retry publication without a network request. With a published, verified file, reconcile Windows marking and completed history even if the original message is gone. When staged bytes are invalid, check the source before requesting the affected chunks. |
| Cancelling | Keep the job out of Downloads. Retry cleanup only for files whose ownership can be verified. |

The client commits finalization intent before rename. It keeps the sidecar until the existing history is written with checked atomic replacement. Recovery identifies the published file by its recorded filesystem identity and the checkpoint, or by identity and exact size after matching completed history and confirming sidecar cleanup. A crash after the history acknowledgement can repeat cleanup without adding another history entry. Application deletion of a pending job never targets the final file.

## Cleanup and failure evidence

Capture the test case, job GUID, log from each process, sizes and hashes, and whether the sidecar and final file exist. Logs may contain other client activity. Share only the lines relevant to the GUID and redact unrelated personal details. Use **Cancel download** in the isolated client to retire an incomplete job. Leave an unexpected or replaced artifact in place until you have copied it or identified it. Once the tests are finished, you can archive the isolated lab directory. Keep your normal Telegram profile separate throughout.

Application crash recovery verifies data against the last durable checkpoint and requires the recorded file identity. An orderly Windows restart should retain completed flushes. Sudden power-loss durability also depends on the filesystem, storage device and write caches. It is possible to lose the last checkpoint generation or directory operation on a filesystem that cannot guarantee their persistence.
