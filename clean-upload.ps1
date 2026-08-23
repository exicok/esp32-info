param(
    [switch]$IncludeUserData,
    [switch]$WhatIf
)

$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath($PSScriptRoot)

function Remove-WorkspaceItem([string]$RelativePath) {
    $target = [IO.Path]::GetFullPath((Join-Path $root $RelativePath))
    if (-not $target.StartsWith($root + [IO.Path]::DirectorySeparatorChar,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove path outside workspace: $target"
    }
    if (-not (Test-Path -LiteralPath $target)) { return }
    if ($WhatIf) {
        Write-Host "[WhatIf] Remove $target"
        return
    }
    Remove-Item -LiteralPath $target -Recurse -Force
    Write-Host "Removed $target"
}

@(
    '.pio',
    '.cache',
    'compile_commands.json',
    'pc_data_sync.exe',
    'pc_data_sync.build.exe',
    'music/build',
    'music/dist',
    'music/out',
    'music/.cache',
    'music/cmake-build-debug',
    'music/cmake-build-release',
    'music/compile_commands.json',
    'music/WANMUSIC.exe',
    'music/MusicPlayer.exe.lnk'
) | ForEach-Object { Remove-WorkspaceItem $_ }

Get-ChildItem -LiteralPath $root -Recurse -File -ErrorAction SilentlyContinue |
    Where-Object {
        -not ($_.FullName.StartsWith((Join-Path $root '.git') + [IO.Path]::DirectorySeparatorChar,
                                     [StringComparison]::OrdinalIgnoreCase)) -and
        $_.Extension -in '.log', '.tmp', '.obj', '.o', '.ilk', '.pdb', '.exp', '.res'
    } |
    ForEach-Object {
        if ($WhatIf) { Write-Host "[WhatIf] Remove $($_.FullName)" }
        else { Remove-Item -LiteralPath $_.FullName -Force }
    }

Get-ChildItem -LiteralPath $root -Recurse -Directory -ErrorAction SilentlyContinue |
    Where-Object {
        -not ($_.FullName.StartsWith((Join-Path $root '.git') + [IO.Path]::DirectorySeparatorChar,
                                     [StringComparison]::OrdinalIgnoreCase)) -and
        ($_.Name -eq 'CMakeFiles' -or $_.Name.EndsWith('_autogen'))
    } |
    Sort-Object { $_.FullName.Length } -Descending |
    ForEach-Object {
        $relative = $_.FullName.Substring($root.Length).TrimStart(
            [IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
        Remove-WorkspaceItem $relative
    }

if ($IncludeUserData) {
    foreach ($folder in @('music/data/database', 'music/data/cache', 'music/data/settings')) {
        $path = Join-Path $root $folder
        Get-ChildItem -LiteralPath $path -Force -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -ne '.gitkeep' } |
            ForEach-Object {
                if ($WhatIf) { Write-Host "[WhatIf] Remove $($_.FullName)" }
                else { Remove-Item -LiteralPath $_.FullName -Recurse -Force }
            }
    }
}

Write-Host 'Upload cleanup complete.'
