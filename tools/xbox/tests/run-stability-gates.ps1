# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later

param([switch]$SkipPortal)
$ErrorActionPreference = 'Stop'
Push-Location (Resolve-Path "$PSScriptRoot/../../..")
try {
    $stabilityVs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (!$stabilityVs) { throw 'Visual Studio C++ tools not found' }
    $stabilityCache = Get-Content build-uwp/CMakeCache.txt
    $stabilityFmt = (($stabilityCache | Where-Object { $_ -match '^CPM_PACKAGE_fmt_SOURCE_DIR:INTERNAL=' }) -replace '^[^=]*=', '') + '/include'
    if (!(Test-Path "$stabilityFmt/fmt/format.h")) { throw 'Pinned fmt headers not found' }
    $stabilityOutput = New-Item -ItemType Directory -Force build-uwp/test-results/stability
    $stabilityCommands = @('@echo off', ('call "' + $stabilityVs + '\VC\Auxiliary\Build\vcvarsall.bat" x64'), 'if not %errorlevel%==0 exit /b 1')
    $stabilityFlags = '/nologo /O2 /MD /std:c++20 /EHsc /utf-8 /Zc:preprocessor /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DFMT_HEADER_ONLY /I src /I "' + $stabilityFmt + '" '
    # Compile the production exporter before the large UWP build. It needs only zstd headers,
    # so an invalid Windows call fails in seconds rather than after the complete emulator link.
    $stabilityZstd = (($stabilityCache | Where-Object { $_ -match '^CPM_PACKAGE_zstd_SOURCE_DIR:INTERNAL=' }) -replace '^[^=]*=', '') + '/lib'
    if (!(Test-Path "$stabilityZstd/zstd.h")) { throw 'Pinned zstd headers not found' }
    $stabilityCommands += ('cl ' + $stabilityFlags + '/D_WIN32_WINNT=0x0A00 /I "' + $stabilityZstd + '" /c src/jadyx/storage/log_export.cpp /Fo:build-uwp/test-results/stability/log_export.obj')
    $stabilityCommands += 'if not %errorlevel%==0 exit /b 1'
    $stabilityCases = @(
        @{ Name = 'page-table-reset'; Sources = 'tools/xbox/tests/page-table-reset.cpp src/common/page_table.cpp src/common/sparse_large_vector.cpp' },
        @{ Name = 'sparse-vector-windows'; Sources = 'tools/xbox/tests/sparse-vector-windows.cpp' },
        @{ Name = 'fault-log-limiter'; Sources = 'tools/xbox/tests/fault-log-limiter.cpp' },
        @{ Name = 'guest-diagnostics'; Sources = 'tools/xbox/tests/bug-tracker.cpp src/common/bug_tracker.cpp' }
    )
    foreach ($stabilityCase in $stabilityCases) {
        $stabilityName = $stabilityCase.Name
        $stabilityCommands += ('cl ' + $stabilityFlags + $stabilityCase.Sources + ' /Fo:build-uwp/test-results/stability/ /Fe:build-uwp/test-results/stability/' + $stabilityName + '.exe /link kernel32.lib')
        $stabilityCommands += 'if not %errorlevel%==0 exit /b 1'
        # Correctness gates run on shared runners; the performance threshold is measured locally.
        $stabilityArgument = if ($stabilityName -eq 'guest-diagnostics') { ' --no-benchmark' } else { '' }
        $stabilityCommands += ('build-uwp\test-results\stability\' + $stabilityName + '.exe' + $stabilityArgument)
        $stabilityCommands += 'if not %errorlevel%==0 exit /b 1'
    }
    Set-Content "$($stabilityOutput.FullName)/build.cmd" $stabilityCommands -Encoding ascii
    & "$env:SystemRoot\System32\cmd.exe" /d /c "$($stabilityOutput.FullName)\build.cmd"
    if ($LASTEXITCODE -ne 0) { throw 'Memory/diagnostics stability gate failed' }
    if (!$SkipPortal) {
        & tools/xbox/tests/run-network-portal.ps1
        if ($LASTEXITCODE -ne 0) { throw 'Live log export gate failed' }
    }
} finally { Pop-Location }
