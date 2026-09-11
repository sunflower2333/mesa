$ErrorActionPreference = 'Stop'
[pscustomobject]@{
    Utc = [DateTime]::UtcNow.ToString('o')
    Desktop = @(Get-Process dwm,explorer | Select-Object ProcessName,Id,SessionId)
    ApplicationRecord = (Get-WinEvent -LogName Application -MaxEvents 1).RecordId
    SystemRecord = (Get-WinEvent -LogName System -MaxEvents 1).RecordId
} | ConvertTo-Json -Depth 4
