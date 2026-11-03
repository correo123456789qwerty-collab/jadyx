# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
# CPU frontend gate using the production UWP libraries. Run after the incremental build.
param([switch]$SyntaxOnly, [switch]$MeasureIncrementalLink)
$ErrorActionPreference = 'Stop'
$testVs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$testVs) { throw 'MSVC not found' }
$testNinja = Get-Content build-uwp/build.ninja
$testObject = ($testNinja | Select-String -SimpleMatch 'build src\video_core\CMakeFiles\video_core.dir\renderer_d3d12\d3d12_image_copy_blit.cpp.obj:').LineNumber
if (!$testObject) { throw 'Configured D3D12 object missing' }
$testIncludes = ($testNinja[$testObject..($testObject+6)] | Where-Object { $_ -match '^  INCLUDES =' }) -replace '^  INCLUDES = ', ''
$testLibs = ($testNinja | Where-Object { $_ -match '^  LINK_LIBRARIES = .*video_core.lib' } | Select-Object -Last 1) -replace '^  LINK_LIBRARIES = ', ''
if (!$testIncludes -or !$testLibs) { throw 'Configured includes or libraries missing' }
$testExecutableRule = ($testNinja | Select-String '^build bin[\\/]jadyx.exe:').LineNumber
if (!$testExecutableRule) { throw 'Configured executable rule missing' }
$testLinkFlags = $testNinja[$testExecutableRule..($testExecutableRule + 12)] | Where-Object { $_ -match '^  LINK_FLAGS = ' }
if ($testLinkFlags -match '/LTCGOUT:') {
    $testModes = [regex]::Matches($testLinkFlags, '/LTCG(?::[A-Z]+)?(?=\s|$)')
    if ($testModes.Count -ne 1 -or $testModes[0].Value -ne '/LTCG:INCREMENTAL') {
        throw 'MSVC incremental optimized link is overridden by another IPO flag'
    }
    $testAppObject = ($testNinja | Select-String '^build src[\\/]jadyx[\\/]CMakeFiles[\\/]jadyx.dir[\\/].*\.cpp.obj:' | Select-Object -First 1).LineNumber
    $testAppFlags = $testNinja[$testAppObject..($testAppObject + 7)] | Where-Object { $_ -match '^  FLAGS = ' }
    if (!$testAppObject -or $testAppFlags -notmatch '(?:^|\s)/GL(?:\s|$)') {
        throw 'Incremental link configuration must preserve /GL on the executable'
    }
    Write-Host 'Verified effective executable link mode: /LTCG:INCREMENTAL with /GL libraries'
}
$testDir = New-Item -ItemType Directory -Force build-uwp/graphics-frontend-gate
$testSource = Join-Path (Get-Location) 'tools/xbox/tests/graphics-frontend.cpp'
$testVmaPath = 'src\jadyx\CMakeFiles\jadyx.dir\app\vma_impl.cpp.obj'
$testVma = if (Test-Path (Join-Path 'build-uwp' $testVmaPath)) { $testVmaPath } else { '' }
$testCommands = @('@echo off', ('call "' + $testVs + '\VC\Auxiliary\Build\vcvarsall.bat" x64'),
    'if errorlevel 1 exit /b 1', ('cd /d "' + (Join-Path (Get-Location) 'build-uwp') + '"'),
    ('cl /nologo /O2 /MD /Gy /std:c++20 /Zc:preprocessor /EHsc /utf-8 /DNOMINMAX /DBOOST_CONTAINER_NO_LIB /DBOOST_CONTAINER_STATIC_LINK ' + $testIncludes + ' "' + $testSource + '" ' + $testVma + ' /Fo:graphics-frontend-gate\ /Fe:graphics-frontend-gate\graphics-frontend.exe /link /OPT:REF /INCREMENTAL:NO /LTCG:INCREMENTAL /LTCGOUT:graphics-frontend-gate\graphics-frontend.iobj /DEBUG /PDB:graphics-frontend-gate\graphics-frontend.pdb /NODEFAULTLIB:libcmt ' + ' d3dcompiler.lib ' + $testLibs))
if ($SyntaxOnly) {
    $testCommands[-1] = 'cl /nologo /Zs /MD /std:c++20 /Zc:preprocessor /EHsc /utf-8 /DNOMINMAX /DBOOST_CONTAINER_NO_LIB /DBOOST_CONTAINER_STATIC_LINK ' + $testIncludes + ' "' + $testSource + '"'
}
Set-Content "$($testDir.FullName)/build.cmd" $testCommands -Encoding ascii
$testBuildTimer = [System.Diagnostics.Stopwatch]::StartNew()
& "$env:SystemRoot\System32\cmd.exe" /d /c "$($testDir.FullName)\build.cmd"
if ($LASTEXITCODE -ne 0) { throw 'Graphics frontend gate build failed' }
$testBuildTimer.Stop()
if ($SyntaxOnly) { return }
$testOldPath = $env:PATH
try {
    $testAppCrt = Get-AppxPackage 'Microsoft.VCLibs.140.00' | Where-Object Architecture -eq 'X64' | Select-Object -First 1 -ExpandProperty InstallLocation
    $env:PATH = (Join-Path (Get-Location) 'build-uwp/bin') + ';' + $testAppCrt + ';' + $env:PATH
    & "$($testDir.FullName)/graphics-frontend.exe"
    if ($LASTEXITCODE -ne 0) { throw 'Graphics frontend gate failed' }
    if ($MeasureIncrementalLink) {
        $testRelinkTimer = [System.Diagnostics.Stopwatch]::StartNew()
        & "$env:SystemRoot\System32\cmd.exe" /d /c "$($testDir.FullName)\build.cmd"
        if ($LASTEXITCODE -ne 0) { throw 'Incremental frontend relink failed' }
        $testRelinkTimer.Stop()
        & "$($testDir.FullName)/graphics-frontend.exe"
        if ($LASTEXITCODE -ne 0) { throw 'Incremental frontend relink changed behavior' }
        Write-Host "Frontend compile/link: $([math]::Round($testBuildTimer.Elapsed.TotalSeconds, 2)) s; retained-state recompile/link: $([math]::Round($testRelinkTimer.Elapsed.TotalSeconds, 2)) s"
    }
} finally {
    $env:PATH = $testOldPath
}
if ($LASTEXITCODE -ne 0) { throw 'Graphics frontend gate failed' }
