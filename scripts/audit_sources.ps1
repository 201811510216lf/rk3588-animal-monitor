# audit_sources.ps1 — 只读审计源项目 A/B（生成 Markdown 摘要）
# 用法：powershell -NoProfile -ExecutionPolicy Bypass -File scripts\audit_sources.ps1 [输出文件]
# 保证：只读操作，不修改源项目任何文件。

param(
    [string]$OutFile = "docs\migration\source_audit_script_output.md"
)

$ErrorActionPreference = 'Stop'

$A = "E:\embedded\双路 ISP 采集与双画面单路推流重构\多路视频"
$B = Join-Path $A "demo_multhread_decode_infer_mulmodel"

$lines = @()
$lines += "# 脚本审计输出（audit_sources.ps1）"
$lines += ""
$lines += "> 生成时间：$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')。只读操作。"
$lines += ""

foreach ($entry in @(@("A", $A), @("B", $B)))
{
    $name = $entry[0]
    $path = $entry[1]
    $lines += "## 项目 $name"
    $lines += ""
    if (-not (Test-Path $path))
    {
        $lines += "- **路径不存在**: $path"
        $lines += ""
        continue
    }
    $files = Get-ChildItem -Recurse -File $path -ErrorAction SilentlyContinue
    $srcFiles = $files | Where-Object { $_.Extension -match '\.(c|cpp|h|hpp)$' }
    $totalSize = ($files | Measure-Object -Property Length -Sum).Sum
    $lines += "- 路径：``$path``"
    $lines += "- Git 仓库：$(if (Test-Path (Join-Path $path '.git')) { '是' } else { '否' })"
    $lines += "- 文件总数：$($files.Count)；源码文件(.c/.cpp/.h/.hpp)：$($srcFiles.Count)；总大小：$([math]::Round($totalSize/1MB,1)) MB"
    $lines += ""
    $lines += "### 源码清单（$name）"
    $lines += ""
    foreach ($f in ($srcFiles | Sort-Object FullName))
    {
        $rel = $f.FullName.Substring($path.Length + 1)
        $lines += "- $rel"
    }
    $lines += ""
}

$outPath = Join-Path $PSScriptRoot "..\$OutFile"
$lines | Set-Content -Path $outPath -Encoding UTF8
Write-Host "已写入：$outPath"
