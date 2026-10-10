param([Parameter(Mandatory = $true)][string]$TestEditor)

$ErrorActionPreference = 'Stop'
$instanceRoot = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$instanceTestExe = (Resolve-Path -LiteralPath $TestEditor).Path
if (![System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($instanceTestExe)).Contains('editor-results.txt')) {
    throw 'Build the isolated test executable with RADIANT_MAP_INSTANCE_TEST first.'
}
$instanceRuntime = Join-Path $instanceRoot '.build-tmp/map-instance-editor-check'
$instanceProject = Join-Path $instanceRuntime 'project'
$instanceMaps = Join-Path $instanceProject 'base/maps'
$instanceSettings = Join-Path $instanceRuntime 'settings/1.6.0'
New-Item -ItemType Directory -Force -Path $instanceRuntime,$instanceMaps,($instanceSettings + '/ja.game'),($instanceRuntime + '/modules'),($instanceRuntime + '/reports') | Out-Null
Copy-Item -LiteralPath $instanceTestExe -Destination ($instanceRuntime + '/radiant.exe') -Force
foreach ($instanceAsset in @('plugins','bitmaps','gamepacks','gl','share','themes')) {
    $instanceLink = Join-Path $instanceRuntime $instanceAsset
    if (!(Test-Path -LiteralPath $instanceLink)) {
        New-Item -ItemType Junction -Path $instanceLink -Target (Join-Path ($instanceRoot + '/install') $instanceAsset) | Out-Null
    }
}
Get-ChildItem -LiteralPath ($instanceRoot + '/install') -File |
    Where-Object { $_.Extension -eq '.dll' -or $_.Name -in @('qt.conf','RADIANT_MAJOR','RADIANT_MINOR') } |
    ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $instanceRuntime -Force }
Get-ChildItem -LiteralPath ($instanceRoot + '/install/modules') -Filter '*.dll' -File |
    ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination ($instanceRuntime + '/modules') -Force }
'<qpref version="1.0"><epair name="gamefile">ja.game</epair><epair name="gamePrompt">false</epair></qpref>' |
    Set-Content -LiteralPath ($instanceSettings + '/global.pref')
$instanceProjectPath = [System.Security.SecurityElement]::Escape($instanceProject.Replace('\','/') + '/')
('<qpref version="1.0"><epair name="EnginePath">' + $instanceProjectPath +
 '</epair><epair name="InstalledDevFilesPath">' + $instanceProjectPath +
 '</epair><epair name="GameName">base</epair><epair name="GameMode">mp</epair>' +
 '<epair name="Autosave">false</epair><epair name="LoadLastMap">false</epair></qpref>') |
    Set-Content -LiteralPath ($instanceSettings + '/ja.game/local.pref')

$instanceCases = @(
    @{name='axial'; file='maps/room.map'},
    @{name='patch'; file='../patch_seam/maps/patch_seam.map'},
    @{name='brush_primitives'; file='../tiny_structural_brush/maps/tiny_structural_brush.map'},
    @{name='geometry_only'; file='maps/room.map'}
)
foreach ($instanceCase in $instanceCases) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $instanceCase.file) -Destination ($instanceMaps + '/room.map') -Force
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'maps/master.map') -Destination ($instanceMaps + '/master.map') -Force
    if ($instanceCase.name -eq 'geometry_only') {
        $instanceText = [System.IO.File]::ReadAllText($instanceMaps + '/room.map')
        $instanceDepth = 0
        for ($instanceIndex = 0; $instanceIndex -lt $instanceText.Length; $instanceIndex++) {
            if ($instanceText[$instanceIndex] -eq '{') { $instanceDepth++ }
            if ($instanceText[$instanceIndex] -eq '}') {
                $instanceDepth--
                if ($instanceDepth -eq 0) { break }
            }
        }
        [System.IO.File]::WriteAllText($instanceMaps + '/room.map',$instanceText.Substring(0,$instanceIndex + 1))
    }
    $instanceProcess = Start-Process -FilePath ($instanceRuntime + '/radiant.exe') -WorkingDirectory $instanceRuntime `
        -ArgumentList @('-platform','windows',('"' + $instanceMaps + '/master.map"')) -WindowStyle Hidden -PassThru
    if (!$instanceProcess.WaitForExit(20000)) {
        Stop-Process -Id $instanceProcess.Id -Force
        throw 'Editor regression harness timed out.'
    }
    $instanceReport = $instanceMaps + '/editor-results.txt'
    if ($instanceProcess.ExitCode -ne 0) {
        if (Test-Path -LiteralPath $instanceReport) { Get-Content -LiteralPath $instanceReport | Select-Object -Last 5 }
        throw ('Editor regression failed: ' + $instanceCase.name)
    }
    Copy-Item -LiteralPath $instanceReport -Destination ($instanceRuntime + '/reports/' + $instanceCase.name + '.txt') -Force
    Write-Output ('PASS: editor ' + $instanceCase.name)
}
