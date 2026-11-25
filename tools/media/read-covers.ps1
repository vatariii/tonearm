# Saves the cover art and track info for what Apple Music is playing, using the
# Windows media session (the same feed Tonearm reads). No screen control.
# Run in Windows PowerShell (not PowerShell 7):
#   powershell -ExecutionPolicy Bypass -File read-covers.ps1
# Queue one song from each album below, press play, then run this. It saves each
# cover and skips to the next track by itself. Stops after 15 minutes.

$albums = [ordered]@{ 'sos'='SOS'; 'wonder'='Wonder'; 'merry-christmas'='Merry Christmas'; 'positions'='Positions'; '143'='143' }
$out = Join-Path $env:USERPROFILE 'Downloads\Tonearm covers'
New-Item -ItemType Directory -Force -Path $out | Out-Null

Add-Type -AssemblyName System.Runtime.WindowsRuntime
$asTask = [System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
  $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and
  $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' } | Select-Object -First 1
function Await($op, [Type]$t) { $task = $asTask.MakeGenericMethod($t).Invoke($null, @($op)); $task.Wait(-1) | Out-Null; $task.Result }

$null = [Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager, Windows.Media.Control, ContentType=WindowsRuntime]
$null = [Windows.Storage.Streams.DataReader, Windows.Storage.Streams, ContentType=WindowsRuntime]
$mgr = Await ([Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager]::RequestAsync()) ([Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager])

# Apple Music on Windows leaves AlbumTitle empty and reports "Artist <em dash> Album" as the artist.
$sep = " $([char]0x2014) "
function Split-Props($p) {
  $artist = "$($p.Artist)".Trim(); $album = "$($p.AlbumTitle)".Trim()
  if (-not $album) { $i = $artist.LastIndexOf($sep); if ($i -gt 0) { $album = $artist.Substring($i + $sep.Length).Trim(); $artist = $artist.Substring(0, $i).Trim() } }
  [pscustomobject]@{ Title = "$($p.Title)".Trim(); Artist = $artist; Album = $album }
}
function Get-Session { $mgr.GetSessions() | Where-Object { $_.SourceAppUserModelId -match 'AppleMusic|AppleInc' } | Select-Object -First 1 }
if (-not (Get-Session)) { Write-Host 'Apple Music is not playing anything. Open it, press play, and run this again.'; exit 1 }

$saved = @{}; $manifest = @()
$deadline = (Get-Date).AddMinutes(15); $lastChange = Get-Date; $lastKey = ''
while ($saved.Count -lt $albums.Count -and (Get-Date) -lt $deadline) {
  $s = Get-Session
  if ($s) {
    $p = Await ($s.TryGetMediaPropertiesAsync()) ([Windows.Media.Control.GlobalSystemMediaTransportControlsSessionMediaProperties])
    $m = Split-Props $p
    $key = "$($m.Title)|$($m.Album)"
    if ($key -ne $lastKey) { $lastKey = $key; $lastChange = Get-Date; Write-Host "Now playing: $($m.Title) / $($m.Artist) / $($m.Album)" }
    $slug = $null
    foreach ($k in $albums.Keys) { if (-not $saved[$k] -and $m.Album -like "*$($albums[$k])*") { $slug = $k; break } }
    if ($slug) {
      Start-Sleep -Milliseconds 1500
      $p = Await ($s.TryGetMediaPropertiesAsync()) ([Windows.Media.Control.GlobalSystemMediaTransportControlsSessionMediaProperties])
      $m = Split-Props $p
      if (-not $p.Thumbnail) { Write-Host "No cover reported yet for $($m.Album), waiting" }
      if ($p.Thumbnail) {
        $stream = Await ($p.Thumbnail.OpenReadAsync()) ([Windows.Storage.Streams.IRandomAccessStreamWithContentType])
        $reader = [Windows.Storage.Streams.DataReader]::new($stream.GetInputStreamAt(0))
        $n = Await ($reader.LoadAsync([uint32]$stream.Size)) ([uint32])
        $bytes = New-Object byte[] $n; $reader.ReadBytes($bytes)
        $ext = if ($bytes[0] -eq 0x89) { 'png' } else { 'jpg' }
        $file = "$slug.$ext"; [IO.File]::WriteAllBytes((Join-Path $out $file), $bytes)
        Add-Type -AssemblyName System.Drawing
        $img = [System.Drawing.Image]::FromFile((Join-Path $out $file)); $size = "$($img.Width)x$($img.Height)"; $img.Dispose()
        $manifest += [ordered]@{ file=$file; title=$m.Title; artist=$m.Artist; album=$m.Album; rawArtist="$($p.Artist)"; size=$size }
        $saved[$slug] = $true
        Write-Host "Saved $file ($size)"
        $null = Await ($s.TrySkipNextAsync()) ([bool]); $lastChange = Get-Date
      }
    } elseif (((Get-Date) - $lastChange).TotalSeconds -gt 45) {
      $null = Await ($s.TrySkipNextAsync()) ([bool]); $lastChange = Get-Date
    }
  }
  Start-Sleep -Seconds 2
}
$manifest | ConvertTo-Json | Set-Content -Encoding UTF8 (Join-Path $out 'covers.json')
Write-Host "Done: $($saved.Count) of $($albums.Count) covers in $out"
