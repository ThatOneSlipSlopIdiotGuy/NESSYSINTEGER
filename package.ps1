<#
    package.ps1 - build NESSYSINTEGER and turn it into a Scoop-installable release.

    Test on this PC (no GitHub needed):
        .\package.ps1
        ...then, in a SECOND terminal:   scoop install .\dist\nessysinteger.json

    Publish for real (so "scoop update" works):
        .\package.ps1 -Version 0.7.1 -Repo yourname/NESSYSINTEGER
        Upload dist\NESSYSINTEGER-0.7.1-win64.zip to a GitHub release tagged v0.7.1,
        and put dist\nessysinteger.json in your Scoop bucket's bucket\ folder.
#>
param(
    [string]$Version = "0.7.1",
    [string]$Repo = "",
    [switch]$Local,
    [int]$Port = 8765
)
$ErrorActionPreference = "Stop"
Set-Location -LiteralPath $PSScriptRoot

Write-Host "== Building" -ForegroundColor Cyan
& cmd.exe /c build.bat
if ($LASTEXITCODE -ne 0) { throw "Build failed - see the compiler output above." }

$dist = Join-Path $PSScriptRoot "dist"
$stage = Join-Path $dist "stage"
$zipName = "NESSYSINTEGER-$Version-win64.zip"
$zip = Join-Path $dist $zipName

Write-Host "== Packaging $zipName" -ForegroundColor Cyan
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path (Join-Path $stage "tests\roms") | Out-Null
Copy-Item -Path "NESSYSINTEGER.exe", "nessys_cli.exe", "README.md" -Destination $stage
Copy-Item -Path "tests\roms\*.nes" -Destination (Join-Path $stage "tests\roms")
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zip
Remove-Item $stage -Recurse -Force
$hash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLower()

if (-not $Repo) { $Local = $true }
$repoText = if ($Repo) { $Repo } else { "YOUR-NAME/NESSYSINTEGER" }
$homepage = "https://github.com/$repoText"
$url = if ($Local) { "http://localhost:$Port/$zipName" } else { "$homepage/releases/download/v$Version/$zipName" }

$manifest = Get-Content -Raw -LiteralPath "scoop\nessysinteger.json"
$vars = [ordered]@{ '@VERSION@' = $Version; '@URL@' = $url; '@HASH@' = $hash; '@HOMEPAGE@' = $homepage; '@REPO@' = $repoText }
foreach ($k in $vars.Keys) { $manifest = $manifest.Replace($k, [string]$vars[$k]) }
$manifestPath = Join-Path $dist "nessysinteger.json"
[System.IO.File]::WriteAllText($manifestPath, $manifest)

Write-Host ""
Write-Host "Release zip : $zip"
Write-Host "SHA256      : $hash"
Write-Host "Manifest    : $manifestPath"
Write-Host "Download URL: $url"

if (-not $Local) {
    Write-Host ""
    Write-Host "Next: create GitHub release v$Version in $Repo, attach $zipName," -ForegroundColor Yellow
    Write-Host "then copy nessysinteger.json into your bucket repo's bucket\ folder and push." -ForegroundColor Yellow
    return
}

Write-Host ""
Write-Host "Serving dist\ at http://localhost:$Port/ - now open a SECOND terminal and run:" -ForegroundColor Yellow
Write-Host "    scoop install `"$manifestPath`"" -ForegroundColor Yellow
Write-Host "Press Ctrl+C here after Scoop says it was installed." -ForegroundColor Yellow
$listener = New-Object System.Net.HttpListener
$listener.Prefixes.Add("http://localhost:$Port/")
$listener.Start()
try {
    while ($listener.IsListening) {
        $pending = $listener.GetContextAsync()
        while (-not $pending.AsyncWaitHandle.WaitOne(200)) { }      # lets Ctrl+C through
        $ctx = $pending.GetAwaiter().GetResult()
        $name = [System.Uri]::UnescapeDataString($ctx.Request.Url.AbsolutePath.TrimStart('/'))
        $file = Join-Path $dist $name
        if ($name -and -not $name.Contains('..') -and (Test-Path -LiteralPath $file -PathType Leaf)) {
            $bytes = [System.IO.File]::ReadAllBytes($file)
            $ctx.Response.ContentType = "application/octet-stream"
            $ctx.Response.ContentLength64 = $bytes.Length
            if ($ctx.Request.HttpMethod -ne "HEAD") { $ctx.Response.OutputStream.Write($bytes, 0, $bytes.Length) }
            Write-Host "  served $name"
        } else {
            $ctx.Response.StatusCode = 404
        }
        $ctx.Response.Close()
    }
} finally {
    $listener.Stop()
}
