param(
    [string]$DataRoot = (Join-Path (Split-Path -Parent $PSScriptRoot) '_smoke\mcfence-data')
)

# 造一份隔离的测试数据：结构与 C# 版写入的完全一致
#   <root>\fences.index
#   <root>\<guid>\__fence_metadata.xml
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

Remove-Item -Recurse -Force $DataRoot -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $DataRoot | Out-Null

# 一张图片用来验证缩略图路径，一个文本文件和一个目录用来验证图标路径
$picture = Join-Path $DataRoot 'picture.png'
$bitmap = New-Object System.Drawing.Bitmap 240, 140
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
    (New-Object System.Drawing.Point 0, 0),
    (New-Object System.Drawing.Point 240, 140),
    [System.Drawing.Color]::FromArgb(255, 40, 120, 220),
    [System.Drawing.Color]::FromArgb(255, 240, 200, 60))
$graphics.FillRectangle($brush, 0, 0, 240, 140)
$font = New-Object System.Drawing.Font 'Segoe UI', 18
$graphics.DrawString('thumb', $font, [System.Drawing.Brushes]::White, 60, 55)
$graphics.Dispose()
$bitmap.Save($picture, [System.Drawing.Imaging.ImageFormat]::Png)
$bitmap.Dispose()
[System.IO.File]::WriteAllText((Join-Path $DataRoot 'hello.txt'), 'smoke')

function Write-Fence {
    param([string]$Root, [string]$Id, [string]$Name, [int]$X, [int]$Y, [int]$W, [int]$H,
          [bool]$Locked, [bool]$CanMinify, [int]$TitleHeight, [string]$Folder = '', [int]$Sort = 0,
          [string[]]$Files = @())

    $dir = Join-Path $Root $Id
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $builder = New-Object System.Text.StringBuilder
    [void]$builder.Append("<?xml version=`"1.0`" encoding=`"utf-8`"?>`r`n")
    [void]$builder.Append("<McFence version=`"2`">`r`n")
    [void]$builder.Append('  <Id>' + $Id + "</Id>`r`n")
    [void]$builder.Append('  <Name>' + $Name + "</Name>`r`n")
    [void]$builder.Append('  <PosX>' + $X + "</PosX>`r`n")
    [void]$builder.Append('  <PosY>' + $Y + "</PosY>`r`n")
    [void]$builder.Append('  <Width>' + $W + "</Width>`r`n")
    [void]$builder.Append('  <Height>' + $H + "</Height>`r`n")
    [void]$builder.Append('  <Locked>' + $Locked.ToString().ToLower() + "</Locked>`r`n")
    [void]$builder.Append('  <CanMinify>' + $CanMinify.ToString().ToLower() + "</CanMinify>`r`n")
    [void]$builder.Append('  <TitleHeight>' + $TitleHeight + "</TitleHeight>`r`n")
    [void]$builder.Append('  <Folder>' + $Folder + "</Folder>`r`n")
    [void]$builder.Append('  <Sort>' + $Sort + "</Sort>`r`n")
    [void]$builder.Append("  <SortDescending>false</SortDescending>`r`n")
    if ($Folder -ne '' -or $Files.Count -eq 0) {
        [void]$builder.Append("  <Files />`r`n")
    } else {
        [void]$builder.Append("  <Files>`r`n")
        foreach ($file in $Files) { [void]$builder.Append('    <string>' + $file + "</string>`r`n") }
        [void]$builder.Append("  </Files>`r`n")
    }
    [void]$builder.Append("</McFence>`r`n")
    $encoding = New-Object System.Text.UTF8Encoding($true)
    [System.IO.File]::WriteAllText((Join-Path $dir '__fence_metadata.xml'), $builder.ToString(), $encoding)
}

$firstGuid = '11111111-2222-3333-4444-555555555555'
$secondGuid = '22222222-3333-4444-5555-666666666666'
$thirdGuid = '33333333-4444-5555-6666-777777777777'

# 映射文件夹：名称故意用 "文件10" / "文件2" 验证自然排序（2 应在 10 之前），外加一个子文件夹。
$mapped = Join-Path $DataRoot 'mapped'
New-Item -ItemType Directory -Force -Path (Join-Path $mapped 'Sub folder') | Out-Null
foreach ($n in @('file10.txt', 'file2.txt', 'notes.md')) {
    [System.IO.File]::WriteAllText((Join-Path $mapped $n), $n)
}
Write-Fence -Root $DataRoot -Id $firstGuid -Name 'McFence smoke' -X 80 -Y 120 -W 370 -H 230 `
    -Locked $false -CanMinify $false -TitleHeight 35 `
    -Files @((Join-Path $DataRoot 'mapped\Sub folder'), $picture, (Join-Path $DataRoot 'hello.txt'))
Write-Fence -Root $DataRoot -Id $secondGuid -Name 'Rolled up' -X 80 -Y 400 -W 370 -H 230 `
    -Locked $false -CanMinify $true -TitleHeight 35 -Files @($picture)

Write-Fence -Root $DataRoot -Id $thirdGuid -Name 'Mapped folder' -X 480 -Y 120 -W 370 -H 230 `
    -Locked $false -CanMinify $false -TitleHeight 35 -Folder $mapped -Sort 1

Set-Content -LiteralPath (Join-Path $DataRoot 'fences.index') -Value @($firstGuid, $secondGuid, $thirdGuid)
"seeded $DataRoot"
