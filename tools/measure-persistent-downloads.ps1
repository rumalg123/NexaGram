[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateRange(1, 2147483647)][int]$ProcessId,
    [Parameter(Mandatory)][string]$ExpectedExecutable,
    [Parameter(Mandatory)][string]$OutputCsv,
    [ValidateRange(1, 86400)][int]$Seconds = 300,
    [ValidateRange(1, 60)][int]$IntervalSeconds = 1
)

$ErrorActionPreference = 'Stop'
$client = Get-Process -Id $ProcessId
$expected = (Resolve-Path -LiteralPath $ExpectedExecutable).ProviderPath
if ($client.Path -ne $expected) { throw 'PID and executable do not match.' }
$started = $client.StartTime
$target = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputCsv)
$writer = $null
$stream = [IO.File]::Open($target, 'CreateNew', 'Write', 'Read')
try {
    $writer = [IO.StreamWriter]::new($stream)
    $writer.AutoFlush = $true
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $previousSeconds = 0.0
    $previousCpu = $client.TotalProcessorTime.TotalSeconds
    $first = $true
    while ($watch.Elapsed.TotalSeconds -lt $Seconds -and !$client.HasExited) {
        $counters = Get-CimInstance Win32_Process -Filter "ProcessId = $ProcessId"
        $client.Refresh()
        if (!$counters -or $client.HasExited -or $client.StartTime -ne $started) { break }
        $elapsed = $watch.Elapsed.TotalSeconds
        $cpu = $client.TotalProcessorTime.TotalSeconds
        $percent = if (!$first -and $elapsed -gt $previousSeconds) {
            [math]::Round(100 * ($cpu - $previousCpu) /
                (($elapsed - $previousSeconds) * [Environment]::ProcessorCount), 2)
        }
        $sample = [pscustomobject][ordered]@{
            Utc = [DateTime]::UtcNow.ToString('o')
            ElapsedSec = [math]::Round($elapsed, 3)
            CpuSeconds = $cpu
            MachineCpuPercent = $percent
            WorkingSetBytes = $client.WorkingSet64
            PeakWorkingSetSinceLaunchBytes = $client.PeakWorkingSet64
            PrivateBytes = $client.PrivateMemorySize64
            Handles = $client.HandleCount
            Threads = $client.Threads.Count
            ProcessReadBytes = $counters.ReadTransferCount
            ProcessWriteBytes = $counters.WriteTransferCount
        }
        $lines = @($sample | ConvertTo-Csv -NoTypeInformation)
        if ($first) { $writer.WriteLine($lines[0]) }
        $writer.WriteLine($lines[1])
        $first = $false
        $previousCpu = $cpu
        $previousSeconds = $elapsed
        Start-Sleep -Seconds $IntervalSeconds
    }
} finally {
    if ($writer) { $writer.Dispose() }
    $stream.Dispose()
    $client.Dispose()
}
