param(
  [string]$Ip = '192.168.8.212',
  [int]$Port = 4901,
  [int]$Seconds = 15,
  [string]$Send = ''
)

$cli = New-Object System.Net.Sockets.TcpClient
try { $cli.Connect($Ip, $Port) } catch { Write-Host "CONNECT FAILED: $_"; return }
Write-Host "Connected to $Ip`:$Port - monitoring for $Seconds s..."
$ns = $cli.GetStream()

if ($Send -ne '') {
  Start-Sleep -Milliseconds 300
  $payload = [Text.Encoding]::ASCII.GetBytes($Send + "`n")
  $ns.Write($payload, 0, $payload.Length)
  $ns.Flush()
  Write-Host (">> sent: '{0}'" -f $Send)
}

$buf = New-Object byte[] 4096
$end = (Get-Date).AddSeconds($Seconds)
$line = ''
while ((Get-Date) -lt $end) {
  if ($ns.DataAvailable) {
    $n = $ns.Read($buf, 0, $buf.Length)
    if ($n -gt 0) {
      $text = [Text.Encoding]::ASCII.GetString($buf, 0, $n)
      foreach ($ch in $text.ToCharArray()) {
        if ($ch -eq "`n") {
          $ts = (Get-Date).ToString('HH:mm:ss.fff')
          Write-Host ("[{0}] {1}" -f $ts, $line.TrimEnd("`r"))
          $line = ''
        } else { $line += $ch }
      }
    }
  } else { Start-Sleep -Milliseconds 50 }
}
if ($line -ne '') { Write-Host ("[partial] {0}" -f $line) }
Write-Host "---- end of capture ----"
$cli.Close()
