$ErrorActionPreference = 'Stop'
$sourceRoot = 'D:\cp2077SourceCode\extracted\root\R6.Root\Mainline\dev\src'
$sdkRoot = 'D:\cp2077SourceCode\tools\emsdk'
$env:EMSDK_PYTHON = Join-Path $sdkRoot 'python\3.13.3_64bit\python.exe'
& $env:EMSDK_PYTHON (Join-Path $PSScriptRoot 'stage-port.py')
if ($LASTEXITCODE -ne 0) { throw 'Could not stage original source adaptations' }
$objectDir = Join-Path $PSScriptRoot 'build\objects'
New-Item -ItemType Directory -Path $objectDir -Force | Out-Null
$commonArgs = @('-std=c++17','-fshort-wchar',"-I$PSScriptRoot\src","-I$sourceRoot", '-O0', '-g', '-ferror-limit=5', '-ffast-math','-fno-finite-math-only','-D__FAST_MATH__=1', '-msimd128', '-msse4.1', '-pthread', '-DRED_COMPILER_CLANG', '-DRED_CONFIGURATION_FINAL','-DNDEBUG','-DRELEASE','-DNO_EDITOR', '-DUNICODE', '-D_UNICODE', '-sMEMORY64=1', '-ivfsoverlay', (Join-Path $PSScriptRoot 'clang-vfs.json'))
$includes = @("-I$sourceRoot\games\cpLauncher\src", "-I$sourceRoot\games\cpLauncher", "-I$sourceRoot\common\engine\src", "-I$sourceRoot\common\engine\include", "-I$PSScriptRoot\generated\commProtocol")
$compiler = Join-Path $sdkRoot 'upstream\emscripten\em++.py'
$units = @(
    @{ Name='mainBrowser'; Path=(Join-Path $PSScriptRoot 'src\mainBrowser.cpp') },
    @{ Name='mainRed'; Path=(Join-Path $sourceRoot 'games\cpLauncher\src\mainRed.cpp') },
    @{ Name='gameApp'; Path=(Join-Path $sourceRoot 'common\engine\src\gameApp.cpp') },
    @{ Name='moduleDependencies'; Path=(Join-Path $PSScriptRoot 'src\module-dependencies.cpp') }
)
foreach ($unit in $units) {
    $compileLog = Join-Path $PSScriptRoot "build\$($unit.Name).log"
    & $env:EMSDK_PYTHON $compiler @commonArgs @includes -c $unit.Path -o (Join-Path $objectDir "$($unit.Name).o") 2>&1 | Out-File -LiteralPath $compileLog -Encoding utf8
    if ($LASTEXITCODE -ne 0) {
        Select-String -LiteralPath $compileLog -Pattern ': error:|fatal error' | ForEach-Object { $_.Line }
        throw "Original engine compilation blocked at $($unit.Name); see $compileLog"
    }
}
Write-Output 'Bootstrap objects compiled. Full engine dependencies must compile and link before browser launch.'

