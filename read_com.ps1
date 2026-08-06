$port = New-Object System.IO.Ports.SerialPort COM9,115200,'None',8,'One'
$port.DtrEnable = $true
$port.RtsEnable = $true
$port.Open()
Start-Sleep -Milliseconds 200
$port.DtrEnable = $false
$port.RtsEnable = $false
Start-Sleep -Seconds 5
$data = $port.ReadExisting()
$port.Close()
Write-Host $data
