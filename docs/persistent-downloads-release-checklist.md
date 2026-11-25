# Resumable downloads release checklist

Checkpoint `41710ac9a025fa14cfe56205b8580f097ecb8012`. Baseline `3aee543e0bde78dc9a370d24349c8a6468973c7e`. This checklist applies to the first public beta. The release owner confirmed that Milestone 6 manual validation passed. Individual run results and benchmark measurements have not been recorded here.

## Engineering preparation

- [x] Review the five-milestone source diff and surrounding save, transport, settings and account lifecycles. See the [engineering audit](persistent-downloads-engineering-audit.md).
- [x] Implement source remediations D01–D08 and new-job checkpoint authentication for D09. Preserve legacy import and document its trust boundary.
- [x] Add regression fixtures, optional timing counters, a read-only Windows process sampler and a [validation guide](persistent-downloads-release-validation-windows.md).
- [x] Static verification completed for whitespace, conflict markers, the Git index, source and fixture registration, localization references, C++ delimiters, text encoding and PowerShell syntax. Verified the attachment-menu file hash and all four earlier guides.
- [ ] Accept the historical v1 trust boundary and supported destination policy. Tamper-resistant import from an untrusted legacy directory is unresolved. Exclude hostile writable directories or complete additional platform security work before promising support.
- [ ] Independently review the native publication, cancellation and filesystem identity changes against actual Windows behavior.

## Execution evidence from the release owner

- [x] Milestone 6 manual validation passed, as confirmed by the release owner.

Record individual runs below when their results are available. Test the final portable binary separately.

- [ ] Debug x64 build and standalone test executable build.
- [ ] Debug automated suite, exit 0.
- [ ] Debug connected functional tests for direct and streaming saves, pause/resume, cancellation, settings and UI.
- [ ] Release x64 non-LTO build, standalone tests and ordinary shortcut smoke.
- [ ] Filesystem tests on NTFS for denied writers, multiple links, reparse paths, replacement, occupied destinations and unavailable volumes.
- [ ] Crash recovery before/after publish, history flush, cancellation and Windows restart. Verify hashes and one history entry.
- [ ] Account isolation, simultaneous saves, logout under load, account-slot reuse and failed retirement.
- [ ] Playback, seeking, variants, offline/online and protected-source cases. Record server-dependent observations separately.
- [ ] Upgrade using real M5 jobs and rollback on disposable copies. Verify portable, installed and update-restart paths.
- [ ] Run A/B/C/D performance comparisons and review overhead, UI latency, handle/thread counts and memory after drain.
- [ ] Record pass, failure or explicit scope exclusion for each platform/filesystem offered to users.
- [ ] Review every remaining limitation and resolve any observed P0/P1 before release.

## Rollout decisions and delivery

- [x] Keep resumable downloads and automatic startup resume off by default for the public beta. Keep individual cancellation confirmation and the pending indicator on. Preserve saved preferences. Always confirm bulk cancellation.
- [x] Review and approve the Milestone 6 commit, including the separately requested attachment-menu crash fix.
- [ ] Select the beta version and confirm the final source commit, pinned dependencies and updater lane.
- [ ] Authorize and perform signing and packaging.
- [ ] Smoke-test the signed portable artifact from clean extraction. Keep updater packages and metadata off the stable feed for a manual-only beta.
- [ ] Compute and record SHA-256 for final release assets.
- [ ] Publish the GitHub prerelease with the exact source tag, then distribute its link.
- [ ] Collect and triage public beta feedback.

Do not infer readiness from an unchecked entry. Record the tested binary, configuration, results and any excluded scenarios alongside the release decision.
