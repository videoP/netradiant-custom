<#
    Packages the Large Maps build as a zip that extracts straight over an
    existing netradiant-custom install.

    radiant.exe and the module DLLs MUST ship together. The modules own scene
    instances, and it is their inline copies of parentSelectedChanged() and
    boundsChanged() that raise the notifications this work depends on - a stale
    entity.dll would never tell batching that an entity was selected.

    q3map2.exe ships too, because -cullgrid and -tjgrid are half of what this
    build is for and neither exists in a stock q3map2. It is standalone: an old
    radiant.exe can drive a new q3map2 and vice versa, since they only ever
    meet across a command line.

    plugins\ (bobtoolz, prtview, meshtex and friends) are deliberately NOT
    included. Nothing in contrib derives from scene::Instance or implements
    scene::Graph - only its nested Walker, which is unchanged - and the new
    virtuals were appended, so existing vtable slots kept their indices. Old
    plugins keep working against the new exe.

    Pass -DeployedDirectory to check each packaged file against a copy you have
    actually been running, so the zip cannot quietly contain a build that was
    never started. Without it the check is skipped and the script says so.

    -Standalone packages the whole install\ directory instead: every runtime
    DLL, the modules, plugins, gamepacks and data, so it runs on a machine with
    nothing already installed. Much larger, and the right thing to publish as a
    release; the patch zip is for someone who already has this editor.

    Usage:  .\package-build.ps1 [-Standalone] [-OutputDirectory <path>]
                                [-DeployedDirectory <path>]
#>

[CmdletBinding()]
param(
    [switch] $Standalone,
    [string] $OutputDirectory   = $PSScriptRoot,
    [string] $DeployedDirectory = ''
)

$ErrorActionPreference = 'Stop'

$root    = $PSScriptRoot
$install = Join-Path $root 'install'

if (-not (Test-Path $install)) {
    throw "build output not found at $install - build before packaging"
}

# --- the set that must ship together -------------------------------------

$items = @()
foreach ($exe in 'radiant.exe', 'q3map2.exe') {
    $items += [pscustomobject]@{ Source = Join-Path $install $exe; Relative = $exe }
}

$modules = Join-Path $install 'modules'
if (-not (Test-Path $modules)) {
    throw "modules not found in the build output - run a full build, not just binaries-radiant-core"
}
foreach ($dll in Get-ChildItem (Join-Path $modules '*.dll')) {
    $items += [pscustomobject]@{ Source = $dll.FullName; Relative = "modules\$($dll.Name)" }
}

foreach ($item in $items) {
    if (-not (Test-Path $item.Source)) {
        throw "missing from the build output: $($item.Relative)"
    }
}

# --- refuse to ship anything that was never actually run ------------------

if ($DeployedDirectory -eq '') {
    Write-Warning "No -DeployedDirectory given, so nothing here has been checked against a build you have run."
}
else {
    $unverified = @()
    foreach ($item in $items) {
        $deployed = Join-Path $DeployedDirectory $item.Relative
        if (-not (Test-Path $deployed)) {
            $unverified += "$($item.Relative)  (never deployed)"
            continue
        }
        $built = (Get-FileHash $item.Source -Algorithm SHA256).Hash
        $live  = (Get-FileHash $deployed    -Algorithm SHA256).Hash
        if ($built -ne $live) {
            $unverified += "$($item.Relative)  (differs from the deployed copy)"
        }
    }

    if ($unverified.Count -gt 0) {
        Write-Warning "These are in the build output but are not what you have been running:"
        foreach ($line in $unverified) { Write-Warning "    $line" }
        Write-Warning "Deploy and test before sending this to anyone."
    }
    else {
        Write-Output "verified: every packaged file matches $DeployedDirectory"
    }
}

# --- name it after the commit it came from -------------------------------

$revision = 'nogit'
$dirty    = $false
try {
    $described = & git -C $root rev-parse --short HEAD 2>$null
    if ($LASTEXITCODE -eq 0 -and $described) { $revision = $described.Trim() }

    # A build from a modified tree is not the commit it names. Two such zips
    # would otherwise share a filename while holding different binaries, and
    # the tester would have no way to tell which one they were sent - so say
    # so, and go to minute resolution for the ones that need telling apart.
    $status = & git -C $root status --porcelain 2>$null
    if ($LASTEXITCODE -eq 0 -and $status) { $dirty = $true }
} catch { }

if ($dirty) {
    $revision = "$revision-dirty"
    $stamp    = Get-Date -Format 'yyyyMMdd-HHmm'
}
else {
    $stamp = Get-Date -Format 'yyyyMMdd'
}

$flavour = if ($Standalone) { 'standalone' } else { 'patch' }
$zipName = "netradiant-largemaps-$flavour-$stamp-$revision.zip"
$zipPath = Join-Path $OutputDirectory $zipName

# --- stage, describe, compress -------------------------------------------

$stage = Join-Path ([System.IO.Path]::GetTempPath()) ("nrpkg-" + [System.Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $stage | Out-Null

try {
    if ($Standalone) {
        # everything needed to run on a machine with nothing installed
        Copy-Item (Join-Path $install '*') $stage -Recurse -Force
    }
    else {
        foreach ($item in $items) {
            $target = Join-Path $stage $item.Relative
            $parent = Split-Path $target -Parent
            if (-not (Test-Path $parent)) { New-Item -ItemType Directory $parent | Out-Null }
            Copy-Item $item.Source $target
        }
    }

    if ($Standalone) {
        $installText = @"
  Extract anywhere and run radiant.exe. Nothing else is needed - this is a
  complete editor, not a patch.

  On first run it asks which game you are mapping for. Point it at that game's
  folder and it remembers.

  It does not touch any netradiant you already have. The two can sit side by
  side; they only share per-user settings under %APPDATA%.
"@
    }
    else {
        $installText = @"
  Extract over your netradiant-custom folder, replacing radiant.exe,
  q3map2.exe and the contents of modules\. Keep a copy of the originals first.

  Replace radiant.exe and modules\ together. They are built against each other
  and mixing them with the originals will not work. Nothing in plugins\ needs
  replacing, and no new runtime DLLs are needed.

  q3map2.exe is independent - it only meets radiant across a command line, so
  it can be updated on its own if you prefer.
"@
    }

    $readme = @"
NetRadiant Custom - Large Maps build ($revision)

Faster loading, drawing and compiling of maps with hundreds of thousands of
brushes. Built from current netradiant-custom master plus the large map work.

INSTALL
$installText

USING IT
  Everything is off by default, so out of the box this behaves exactly like
  the build you already have.

  Settings -> Large Maps has ten options, grouped by what they affect: Load,
  View, Editing and Compile. Each says what it does and what it costs; hover
  for the detail.

  The eight editor options take effect after a restart. The two Compile ones
  do not - they only add a switch to q3map2's command line, so the next
  compile picks them up.

  Start with all of them on. If anything looks wrong, turn them off one at a
  time - whichever one makes the problem go away identifies it. The console
  prints which ones are actually in force on every load, which matters because
  the editor ones only change on a restart: what is ticked and what is running
  can differ.

  View -> Show Stats overlays the frame rate, draw calls, how much geometry is
  being drawn in batches, where the frame time went, and committed memory.

MEASURED
  On a 1 GB map, 972,961 brushes:

    map load        29.2s  ->  18.4s
    memory        14558 MB  ->  13510 MB
    frame rate       5 fps  ->  ~200 fps

  Compile stages, on synthetic maps built to stress them:

    CullSides       26.7s  ->   0.3s   at 97k brushes    (-cullgrid)
    FixTJunctions   12.8s  ->   1.6s   at 19.6k brushes  (-tjgrid)

  Both compile options produce a byte-identical .bsp - only the compile time
  changes. -tjgrid also has a -tjverify mode that runs the old scan alongside
  the new index and counts any disagreement; it reports zero.

WHAT TO SEND BACK
  Load the biggest map you have and send back the console log. That is the
  whole test - there is nothing to fill in.

  The log is rewritten every run. Radiant's first console line is
  "Started logging to <path>" and that is where it is; normally
  %APPDATA%\NetRadiant\radiant.log . Copy it after loading, before you quit
  and reload something else.

  Also say how much RAM the machine has. That is not small talk: this build
  prints memory and timing beside each load phase, and the open question is
  whether the slow phases on very large maps are doing too much work or merely
  waiting on the pagefile. Comparing a machine where the map fits in RAM
  against one where it does not is what settles that, and it cannot be settled
  on one machine.

  Worth having either way: how long the map took, whether it looked right, and
  anything that drew wrongly or vanished.

  For a compile, -v prints a timing line for CullSides and FixTJunctions, and
  the brush and edge-line counts behind them.
"@
    Set-Content -Path (Join-Path $stage 'README-largemaps.txt') -Value $readme -Encoding UTF8

    if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zipPath -CompressionLevel Optimal

    $size = [math]::Round((Get-Item $zipPath).Length / 1MB, 1)
    Write-Output ""
    if ($Standalone) {
        $staged = (Get-ChildItem $stage -Recurse -File).Count
        Write-Output "$zipName  ($size MB, $staged files - complete editor)"
    }
    else {
        Write-Output "$zipName  ($size MB, $($items.Count) files - patch)"
        Write-Output "  radiant.exe"
        Write-Output "  q3map2.exe"
        Write-Output "  modules\   $(($items | Where-Object { $_.Relative -like 'modules\*' }).Count) dll"
    }
    Write-Output "  README-largemaps.txt"
    Write-Output ""
    Write-Output $zipPath
}
finally {
    Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
}
