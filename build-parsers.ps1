$ErrorActionPreference = 'Stop'
$sourceRoot = 'D:\cp2077SourceCode\extracted\root\R6.Root\Mainline\dev\src'
$toolsRoot = Join-Path $PSScriptRoot 'build-tools\winflexbison'
$generatorRoot = Join-Path $PSScriptRoot 'generated\redReflection'
New-Item -ItemType Directory -Path $generatorRoot -Force | Out-Null
& (Join-Path $toolsRoot 'win_bison.exe') "--defines=$generatorRoot\scriptExpressionParser_bison.cxx.h" -o "$generatorRoot\scriptExpressionParser_bison.cxx" "$sourceRoot\common\redReflection\src\scriptExpressionParser.bison"
if ($LASTEXITCODE -ne 0) {throw 'Original expression grammar generation failed'}
& (Join-Path $toolsRoot 'win_bison.exe') "--defines=$generatorRoot\bison_tokens.h" -o "$generatorRoot\bison_unused.cxx" "$sourceRoot\common\scriptCompiler\src\scriptFileParser.bison"
if ($LASTEXITCODE -ne 0) {throw 'Original script grammar token generation failed'}
& (Join-Path $toolsRoot 'win_flex.exe') -o "$generatorRoot\scripts.cxx" --never-interactive --batch -Cfe "$sourceRoot\..\internal\RedLexer\native\src\scripts.flex"
if ($LASTEXITCODE -ne 0) {throw 'Original script lexer generation failed'}
[ordered]@{Source='https://github.com/lexxmark/winflexbison/releases/tag/v2.5.25';Version='2.5.25';ArchiveSHA256='8d324b62be33604b2c45ad1dd34ab93d722534448f55a16ca7292de32b6ac135';GeneratedFiles=@(Get-ChildItem -LiteralPath $generatorRoot -File | ForEach-Object {@{Name=$_.Name;SHA256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash}})} | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $generatorRoot 'generation.json') -Encoding utf8
