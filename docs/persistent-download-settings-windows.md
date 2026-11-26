# Permanent resumable-download settings on Windows

Use an isolated profile, test accounts and non-sensitive fixtures. The essential cases usually work with two 8`64 MiB ZIP or BIN files and one playable MP4 or M4A of similar size. Use larger fixtures only if the small transfers finish before you can pause them.

## Build and run the standalone tests

Open `D:\TBuild\tdesktop\out\Telegram.slnx` in Visual Studio. Select **Debug | x64** and build **Telegram**, then build **test_persistent_file_sink** separately under **(tests)**. From an x64 Developer PowerShell, the equivalent is below. Use the existing configured output tree.

```powershell
$root = 'D:\TBuild\tdesktop'
$out = Join-Path $root 'out'
cmake --build $out --config Debug --target Telegram test_persistent_file_sink
$exe = Join-Path $out 'Debug\NexaGram.exe'
$test = Join-Path $out 'test_persistent_file_sink\Debug\test_persistent_file_sink.exe'
& $test
if ($LASTEXITCODE -ne 0) { throw "Download tests failed with exit code $LASTEXITCODE" }
```

Expect exit code `0` and `Persistent file sink tests passed.` The new fixtures exercise defaults, a preference-map round-trip, invalid values, override precedence, eligible startup states, confirmation and indicator policy, and staging filenames. The same target runs the earlier recovery, streaming, SHA-256 and presentation fixtures. Its sparse-file cases need an NTFS temporary directory on Windows. The native UI and account lifecycle checks below use the client.

## Prepare an isolated profile and a normal shortcut

```powershell
$root = 'D:\TBuild\tdesktop'
$exe = Join-Path $root 'out\Debug\NexaGram.exe'
$lab = Join-Path $env:LOCALAPPDATA 'NexaGram-Download-Settings-Test'
$profile = Join-Path $lab 'profile'
$originals = Join-Path $lab 'originals'
$downloads = Join-Path $lab 'downloads'
New-Item -ItemType Directory -Force -Path $profile,$originals,$downloads | Out-Null

$keys = 'TDESKTOP_EXPERIMENTAL_RESUMABLE_DOWNLOADS',
        'TDESKTOP_EXPERIMENTAL_RESUME_ON_STARTUP',
        'TDESKTOP_PERSISTENT_DOWNLOAD_TRACE'
foreach ($key in $keys) {
    Remove-Item "Env:$key" -ErrorAction SilentlyContinue
    foreach ($scope in 'User','Machine') {
        if ($null -ne [Environment]::GetEnvironmentVariable($key,$scope)) {
            Write-Warning "$key is still set for $scope"
        }
    }
}

$shortcut = (New-Object -ComObject WScript.Shell).CreateShortcut(
    (Join-Path $lab 'Isolated NexaGram.lnk'))
$shortcut.TargetPath = $exe
$shortcut.WorkingDirectory = Split-Path -Parent $exe
$shortcut.Arguments = '-many -workdir "{0}"' -f $profile
$shortcut.Save()
explorer.exe $lab
```

If Windows reports a persistent variable you added in an earlier test, remove that named variable in **System Properties > Advanced > Environment Variables** and sign out of Windows and back in so Explorer inherits the change. Later tests can set process variables in PowerShell without changing the values inherited by Explorer.

Double-click **Isolated NexaGram** in File Explorer. This launch uses a normal shortcut, no environment overrides, and no `-debug` argument. Sign in with a test account and turn on **Ask download path** in **Settings > Advanced**. Upload the original fixtures as files to a test chat. Keep your originals and download destinations in separate directories. Open **Resumable Downloads** in the Advanced page. The **Downloads** row just above the subsection is a permanent entry point even when the chat-list indicator is hidden. A second test account is useful for the isolation case. See the [streaming guide](persistent-streaming-downloads-windows.md) for synthetic media and how to identify the backend.

## Essential acceptance tests

| Case | Actions | Pass criteria |
|---|---|---|
| Fresh shortcut launch | Open **Settings > Advanced > Resumable Downloads** from the shortcut. Use Settings search for `resumable downloads`. | Feature Off, startup resume Off and unavailable, individual cancellation confirmation On, pending indicator On. The subsection and its switches are searchable. Ordinary downloads work. |
| Enable in Settings | Begin an ordinary save. Turn **Enable resumable downloads** on during that save, wait for the ordinary save to finish, then use **Restart**. Start another explicit file save. | The original transfer remains ordinary. After restart the switch is still On and the new eligible file has persistent controls, a `.64gram-<GUID>.part` staging file, and a `.part.resume` checkpoint. No incomplete output appears under the final name. |
| Manual startup policy | Pause A. Leave B active. Keep startup resume Off. Quit normally and relaunch through the shortcut. | A stays Paused and B is Interrupted after verification. Both retain progress and can resume manually. |
| Automatic startup policy | Leave A manually paused and B active. Turn startup resume On. Inspect the rows before quitting, then quit normally and relaunch. | The change does not start a transfer in the current process. At the next launch B resumes after source and integrity checks. A stays paused. |
| Disable during a transfer | While B transfers and A is paused, turn the main switch off. Dismiss the confirmation once. Open it again, confirm and click Restart. | Dismissal leaves the stored setting On. Confirmation schedules Off and keeps the current policy until restart. A remains Paused, B becomes Interrupted, and staging and checkpoints remain. Resume and Resume All are unavailable even if the stored startup preference is On. Cancel remains available. |
| Disable with paused jobs, then re-enable | Leave A paused, switch off the feature and restart. Open Advanced Downloads, then turn the feature on and restart again. Resume A. | The row survives both restarts and keeps the same ID and committed data. The job stays paused until explicitly resumed. |
| Destination collision and ordinary saves | With the feature off and a job preserved, use **Save As** to choose that job's final destination. Then choose a different filename. Also try entering the exact staging or checkpoint name. | The reserved destination is blocked even after a save-dialog overwrite approval. The preserved job and artifacts remain. Another filename saves normally and uses the ordinary path. |
| Individual confirmation | With confirmation On, test the row and context-menu Cancel, the chat file Cancel and the media viewer Cancel on separate jobs. Dismiss one dialog and confirm another. Turn confirmation Off and cancel one pending job. | Each action opens one dialog when On. Dismissal preserves the job. Confirming deletes only saved incomplete progress. Off cancels the single job immediately. Ordinary download cancellation follows its existing behavior. |
| Bulk confirmation | With individual confirmation Off, select two pending rows and one completed row. Choose **Cancel Selected**, dismiss it, then repeat and confirm. | Explicit confirmation is still required. It shows the current account and eligible count. Only the eligible pending jobs can be cancelled. The completed file still opens. |
| Idle indicator | Pause all jobs. Turn **Show pending download indicator** Off. Open Advanced Downloads, resume a job, pause it again, and re-enable the indicator. | Idle shortcut hides while Off and returns while On. The normal active transfer indicator still appears. The Advanced Downloads entry always works. |
| File integrity | Resume a preserved job and compare the final file with its original, using the command below. | SHA-256 matches. Completed history contains one item, and the completed job's staging artifacts are retired. |

```powershell
$name = 'fixture.zip'
$expected = (Get-FileHash -LiteralPath (Join-Path $originals $name) -Algorithm SHA256).Hash
$actual = (Get-FileHash -LiteralPath (Join-Path $downloads $name) -Algorithm SHA256).Hash
if ($actual -ne $expected) { throw 'Completed file SHA-256 mismatch' }
$actual
Get-ChildItem -LiteralPath $downloads -Force | Select-Object Name,Length
```

## Developer overrides and logs

Close the isolated process normally before changing variables. Alternate the following cases with normal shortcut launches to verify that forced values never modify the saved preferences.

```powershell
$log = Join-Path $profile 'log.txt'
function Start-SettingsTestClient {
    Start-Process -FilePath $exe -ArgumentList ('-many -debug -workdir "{0}"' -f $profile) -PassThru
}

$env:TDESKTOP_PERSISTENT_DOWNLOAD_TRACE = '1'
$env:TDESKTOP_EXPERIMENTAL_RESUMABLE_DOWNLOADS = '0'
$env:TDESKTOP_EXPERIMENTAL_RESUME_ON_STARTUP = '1'
$client = Start-SettingsTestClient
```

| Case | Actions | Pass criteria |
|---|---|---|
| Explicit Off wins | Save the feature as On, quit and launch the command above. | The feature switch is locked Off with an override explanation. Startup resume is ineffective even with its override set to `1`. |
| Explicit On wins | Set the feature override to `1` and the startup override to `0`. Relaunch the isolated client. | Main switch locked On and startup switch locked Off. Manual Resume remains available for eligible jobs. |
| Invalid and removed overrides | Relaunch with `true` for the feature override and `2` for startup. Quit, remove both activation variables and relaunch from the shortcut. | Invalid values fall back to the stored choices. Neither test changes the stored settings or locks the controls. |
| Diagnostic-only run | Remove both activation variables, keep trace set to `1`, and relaunch. | The effective policy comes from native settings. Logs show no activation override. Normal use works with the trace variable removed. |

```powershell
Select-String -LiteralPath $log -Pattern 'download preferences|download preference|download feature override|download startup override|download tracing override|download policy|download activation|download deactivation|pending jobs discovered|startup resume decision|shutdown suspending'
$job = 'paste-the-job-GUID'
Select-String -LiteralPath $log -SimpleMatch $job
foreach ($key in $keys) { Remove-Item "Env:$key" -ErrorAction SilentlyContinue }
```

Override detail codes are `0` absent, `1` explicit Off, `2` explicit On and `3` invalid. A traced launch shows `download preferences loaded` and the effective `download policy enabled` and `download policy startup resume` values. A write emits `download preference saved` or `download preference save failed`. On failure the switch retains its previous value and shows a toast. A normal shutdown can emit `shutdown suspending jobs` and `suspend checkpoint`. Recovered jobs emit `pending jobs discovered`, `staging recovery` and `startup resume decision`. Expect decision `0` with startup resume off. Earlier guides show how to match request offsets and streaming attachment events.

## Extended regression

| Case | Actions | Pass criteria |
|---|---|---|
| Rapid changes | Set the main switch On then Off before restarting. Repeat with the startup switch while the feature is on. Reopen Settings between changes. | The last stored choice wins. Restart is offered only when the effective main policy would change. |
| Multiple accounts and logout | Create jobs in two test accounts. Switch accounts during a transfer and change the global indicator setting. Use selected actions and log out of one test account. | The preferences are global. Jobs retain their owner and selected actions affect only the shown account scope. Logout retires its own jobs and leaves the other account's jobs available. |
| Playback independence | Start a streaming-backed file save while playing the source. Pause and seek. Cancel another file save during playback. Close the player during a third transfer. | Playback survives file suspension and cancellation. Downloading continues after closing playback. |
| Crash, verification and publication | Use separate attempts to stop only the isolated process after a checkpoint, while verifying, or near final publication. Use the exact PID and path guard in the [recovery guide](persistent-download-recovery-windows.md). Relaunch with the feature off where applicable, then enable and resume. | Committed data survives. Off prevents new source and chunk requests. Local publication of an already-complete file and terminal cleanup can reconcile while Off. The final file matches and appears once in history. |
| Offline, minimized and tray startup | Quit with one interrupted job and one manual pause. Relaunch once offline and once with **Launch NexaGram minimized** enabled for the test run. Then reopen the isolated window. | Manual pause remains. Offline failures are retryable without a retry loop. Minimized launch and normal launch follow the same persisted settings. |
| Reset to defaults | With test jobs present, set the four switches to defaults and restart. On a separate disposable test profile, log out of its last account, relaunch and sign in again. | Returning the switches to defaults preserves the first profile's jobs. Last logout resets application preferences and retires its account's jobs under the existing account logout policy. |
| Upgrade from Milestone 4 | In an isolated Milestone 4 profile, use its original environment flag to leave one paused and one interrupted job. Quit and launch the new client with that profile, the same destination paths and no overrides. | Default Off applies on first launch. The old jobs and committed bytes stay discoverable. Enable and restart to recover them. Completed history stays readable. |
| Theme and localization | Check light, dark, a custom theme, RTL, different DPI scales and a narrow window. Use Tab and Space on the settings controls. | Native switches, help text, locks and confirmations remain usable and readable. The layout follows theme and text direction. |
| Release smoke check | Use the existing x64 Release client, or check that `DESKTOP_APP_ENABLE_LTO` is `OFF` in `out\CMakeCache.txt` and manually build **Telegram** with **Release \| x64**. Point the isolated shortcut to `D:\TBuild\tdesktop\out\Release\NexaGram.exe`. Remove overrides and repeat the fresh-launch and enable cases. | Permanent controls and an eligible save work in Release without developer flags. |
| Existing panel regression | Exercise search, status filters, selected actions, completed-file actions, and ordinary downloading with a mixed list. | Results and scope match the [Downloads manager guide](persistent-download-manager-windows.md). Existing playback and completed-file actions work as before. |

The standalone cases and local setting changes are deterministic. Telegram access failures, reference expiration, CDN usage, available video variants and exact crash timing depend on the server or transfer. Use the earlier guides for those checks.

## Storage, downgrade and cleanup

The four preferences use the existing encrypted application settings record in the profile's `tdata` directory. A checked atomic replacement must succeed before the setting is reported saved. Power-loss durability remains filesystem- and device-dependent. An upgrade discovers the old v1 registry even with the main switch at its default Off. Once the registry contains new keyed jobs, it uses v2 and requires a compatible client. Finish or cancel those jobs in the beta before downgrading.

Close the isolated client before changing binaries or moving its profile. Pending destinations are absolute paths, so preserve the associated test destinations when revisiting a registry. Back up the isolated profile and test files before trying an older binary. Milestone 4 can manage the compatible registry with its original feature flag. Older builds can ignore pending jobs and may lack the necessary preference or registry support. Return to a compatible build to manage the jobs and avoid their reserved destinations while using an older build.

Use Downloads to cancel test jobs you no longer need. Exit the isolated client before archiving the lab profile, fixtures and relevant log lines. Leave unknown artifacts in place while checking their identity. Keep production profiles and files outside the lab.
