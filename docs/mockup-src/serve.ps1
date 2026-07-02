$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$port = 8934
$listener = New-Object System.Net.HttpListener
$listener.Prefixes.Add("http://localhost:$port/")
$listener.Start()
Write-Host "Serving $root on http://localhost:$port/"

$mime = @{
  ".html" = "text/html"
  ".css"  = "text/css"
  ".js"   = "application/javascript"
  ".svg"  = "image/svg+xml"
  ".png"  = "image/png"
}

$shotsDir = Join-Path $root "shots"
if (-not (Test-Path $shotsDir)) { New-Item -ItemType Directory -Path $shotsDir | Out-Null }

while ($listener.IsListening) {
  $context = $listener.GetContext()
  $request = $context.Request
  $response = $context.Response
  $path = $request.Url.LocalPath

  if ($request.HttpMethod -eq "POST" -and $path -eq "/upload") {
    $name = $request.QueryString["name"]
    if ([string]::IsNullOrWhiteSpace($name)) { $name = "shot.png" }
    $name = [System.IO.Path]::GetFileName($name)
    $outPath = Join-Path $shotsDir $name
    $ms = New-Object System.IO.MemoryStream
    $request.InputStream.CopyTo($ms)
    [System.IO.File]::WriteAllBytes($outPath, $ms.ToArray())
    $response.StatusCode = 200
    $response.OutputStream.Close()
    Write-Host "Saved $outPath"
    continue
  }

  if ($path -eq "/") { $path = "/index.html" }
  $filePath = Join-Path $root ($path.TrimStart("/"))

  if (Test-Path $filePath -PathType Leaf) {
    $ext = [System.IO.Path]::GetExtension($filePath)
    $contentType = if ($mime.ContainsKey($ext)) { $mime[$ext] } else { "application/octet-stream" }
    $bytes = [System.IO.File]::ReadAllBytes($filePath)
    $response.ContentType = $contentType
    $response.ContentLength64 = $bytes.Length
    $response.OutputStream.Write($bytes, 0, $bytes.Length)
  } else {
    $response.StatusCode = 404
  }
  $response.OutputStream.Close()
}
