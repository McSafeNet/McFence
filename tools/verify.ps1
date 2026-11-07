param(
    [string]$Root = (Split-Path -Parent $PSScriptRoot),
    [string]$MSBuild = '',
    [string]$Dumpbin = '',
    [switch]$SkipBuild
)

# McFence 的自动化验收：
#   1) 用 v141_xp 构建 Win32 + x64
#   2) 检查 PE 头与导入表是否对 XP 安全（子系统 5.01/5.02、不得出现 api-ms-win-* 等）
#   3) 造隔离数据 → MCFENCE_SELFTEST 读入后原样重写 → 与种子文件逐字节比对（读写往返无损）
#   4) 渲染围栏（含折叠围栏、映射文件夹围栏）并把分层缓冲导出 PNG
$ErrorActionPreference = 'Stop'
# 未显式指定时用 vswhere 找装有 v141_xp 工具集的 Visual Studio，再定位 MSBuild / dumpbin
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsRoot = & $vswhere -all -products * -property installationPath |
    Where-Object { Get-ChildItem (Join-Path $_ 'MSBuild\Microsoft\VC\*\Platforms\Win32\PlatformToolsets\v141_xp') -ErrorAction SilentlyContinue } |
    Select-Object -First 1
if (-not $vsRoot) { Write-Host 'v141_xp toolset not found in any Visual Studio installation' -ForegroundColor Red; exit 1 }
if (-not $MSBuild) {
    $MSBuild = Get-ChildItem (Join-Path $vsRoot 'MSBuild\*\Bin\MSBuild.exe') | Select-Object -First 1 -ExpandProperty FullName
}
if (-not $Dumpbin) {
    $Dumpbin = Get-ChildItem (Join-Path $vsRoot 'VC\Tools\MSVC\*\bin\Hostx64\x64\dumpbin.exe') |
        Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
}
$msbuild = $MSBuild
$dumpbin = $Dumpbin
$solution = Join-Path $Root 'McFence.sln'
$smokeRoot = Join-Path $Root '_smoke'
$dataRoot = Join-Path $smokeRoot 'mcfence-data'
$report = @()
$failed = $false

function Step($text) { Write-Host "`n== $text ==" -ForegroundColor Cyan }

if (-not $SkipBuild) {
    Step 'build'
    foreach ($platform in @('x64', 'x86')) {
        $output = & $msbuild $solution /p:Configuration=Release /p:Platform=$platform /v:minimal /nologo 2>&1
        $errors = $output | Select-String -Pattern 'error'
        if ($errors) { $errors | ForEach-Object { Write-Host $_ -ForegroundColor Red }; exit 1 }
        $warnings = ($output | Select-String -Pattern 'warning').Count
        $report += "build $platform : ok ($warnings warnings)"
    }
}

Step 'PE header and imports'
foreach ($target in @(
        @{ Path = (Join-Path $Root 'Release\x64\McFence.exe'); Subsystem = '5.02' },
        @{ Path = (Join-Path $Root 'Release\x86\McFence.exe'); Subsystem = '5.01' })) {
    $headers = (& $dumpbin /nologo /headers $target.Path) | Out-String
    $dependents = (& $dumpbin /nologo /dependents $target.Path) | Out-String
    $subsystem = ([regex]::Match($headers, '([\d\.]+) subsystem version')).Groups[1].Value
    $bad = @()
    foreach ($name in @('api-ms-win', 'ucrtbase', 'vcruntime', 'msvcp', 'dwmapi', 'd2d1', 'd3d11')) {
        if ($dependents -match [regex]::Escape($name)) { $bad += $name }
    }
    $status = if ($subsystem -eq $target.Subsystem -and $bad.Count -eq 0) { 'ok' } else { 'FAIL' }
    $report += "pe $(Split-Path $target.Path -Leaf) : subsystem $subsystem (want $($target.Subsystem)), forbidden imports: $(if ($bad.Count) { $bad -join ',' } else { 'none' }) -> $status"
    if ($status -eq 'FAIL') { exit 1 }
}

Step 'seed isolated data'
& (Join-Path $Root 'tools\seed.ps1') -DataRoot $dataRoot | Out-Null
$firstFence = Join-Path $dataRoot '11111111-2222-3333-4444-555555555555\__fence_metadata.xml'

Step 'XML round-trip (own format v2)'
$exe = Join-Path $Root 'Release\x64\McFence.exe'
$metas = Get-ChildItem -Path $dataRoot -Recurse -Filter '__fence_metadata.xml'
$before = @{}
foreach ($m in $metas) { $before[$m.FullName] = (Get-FileHash $m.FullName).Hash }
$env:MCFENCE_DATA_ROOT = $dataRoot
$env:MCFENCE_INSTANCE = 'verify'
$env:MCFENCE_SELFTEST = '1'
Start-Process -FilePath $exe -Wait | Out-Null
Remove-Item Env:\MCFENCE_SELFTEST -ErrorAction SilentlyContinue
$changed = @($metas | Where-Object { (Get-FileHash $_.FullName).Hash -ne $before[$_.FullName] })
if ($changed.Count -eq 0) { $report += "xml round-trip : $($metas.Count) files byte-identical after rewrite" }
else { $report += "xml round-trip : DIFFERS -> $($changed.Name -join ',')"; $failed = $true }

Step 'render surface dump'
$shot = Join-Path $smokeRoot 'mcfence-surface.png'
$shot2 = Join-Path $smokeRoot 'mcfence-surface_2.png'
$shot3 = Join-Path $smokeRoot 'mcfence-surface_3.png'
Remove-Item $shot, $shot2, $shot3 -ErrorAction SilentlyContinue
$env:MCFENCE_DUMP_SURFACE = $shot
Start-Process -FilePath $exe -Wait | Out-Null
Remove-Item Env:\MCFENCE_DUMP_SURFACE -ErrorAction SilentlyContinue
$rendered = Test-Path $shot
$second = Test-Path $shot2
$third = Test-Path $shot3
$report += "surface dump : $(if ($rendered) { "ok ($((Get-Item $shot).Length) bytes)" } else { 'FAILED' }); rolled-up fence: $(if ($second) { 'ok' } else { 'missing' }); mapped fence: $(if ($third) { 'ok' } else { 'missing' })"

Step 'log'
$log = Join-Path $dataRoot 'app_diag.log'
if (Test-Path $log) { Get-Content $log -Encoding UTF8 -Tail 8 | ForEach-Object { Write-Host "  $_" } }

Step 'summary'
$report | ForEach-Object { Write-Host "  $_" }
if (-not $rendered -or -not $third -or $failed) { exit 1 }
