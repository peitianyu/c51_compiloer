# build.ps1 — 使用 MinGW gcc 编译 ttcc.exe
$ErrorActionPreference = "Stop"

$srcDir  = Join-Path $PSScriptRoot "src"
$outFile = Join-Path $PSScriptRoot "ttcc.exe"

# 源文件列表
$files = @(
    "$srcDir\main.c",
    "$srcDir\core\c11_lowering.c",
    "$srcDir\core\codegen_c51.c",
    "$srcDir\core\embed_toolchain.c",
    "$srcDir\core\lexer.c",
    "$srcDir\core\parser.c",
    "$srcDir\core\pp.c",
    "$srcDir\core\sexpr.c",
    "$srcDir\core\verbose.c"
)

Write-Host "Compiling ttcc.exe ..." -ForegroundColor Cyan

# 清除 cosmocc 环境变量干扰，让 gcc 使用标准 include 路径
$env:C_INCLUDE_PATH = $null
$env:CPATH = $null

gcc.exe -o $outFile `
    -I"$srcDir\core" -I"$srcDir" `
    $files -lmsvcrt

if ($LASTEXITCODE -eq 0) {
    Write-Host "OK: $outFile" -ForegroundColor Green
} else {
    Write-Host "FAILED" -ForegroundColor Red
    exit 1
}
