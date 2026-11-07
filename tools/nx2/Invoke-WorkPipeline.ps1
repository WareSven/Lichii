param(
    [Parameter(Mandatory=$true)][string]$Config,
    [ValidateSet('sync','report','status','add','assign','candidate','record','run','gate','handoff')]
    [string]$Action='sync',
    [switch]$Open,
    [Parameter(ValueFromRemainingArguments=$true)][string[]]$PipelineArguments
)
$ErrorActionPreference='Stop'
$python=(Get-Command python.exe -ErrorAction Stop).Source
$arguments=@('-I', (Join-Path $PSScriptRoot 'work_pipeline.py'), '--config', $Config, $Action)
if ($PipelineArguments) { $arguments += $PipelineArguments }
& $python @arguments
$code=$LASTEXITCODE
if ($code -eq 0 -and $Open) {
    $settings=Get-Content -LiteralPath $Config -Raw | ConvertFrom-Json
    $dashboard=Join-Path $settings.report_dir 'dashboard.html'
    if (-not (Test-Path -LiteralPath $dashboard -PathType Leaf)) { throw 'Dashboard not generated.' }
    Start-Process -FilePath $dashboard
}
exit $code
