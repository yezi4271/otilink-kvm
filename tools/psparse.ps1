param([string]$Path)
# psparse.ps1 —— 用 PowerShell 自己的解析器检查脚本语法（比"看 BOM"强得多：
# BOM 丢了之后中文注释会被按 ANSI 读成乱码、还会吞掉换行，表现为莫名其妙的语法错误）
$e = $null
$null = [System.Management.Automation.Language.Parser]::ParseFile($Path, [ref]$null, [ref]$e)
if ($e.Count -gt 0) {
    foreach ($x in $e) { Write-Output ("ERR L" + $x.Extent.StartLineNumber + " [" + $x.ErrorId + "] " + $x.Extent.Text) }
    exit 1
}
Write-Output "ERRS=0"
exit 0
