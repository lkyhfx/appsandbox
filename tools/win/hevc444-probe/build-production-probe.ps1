param([switch]$Run)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$output = Join-Path $repo 'build/issue3'
New-Item -ItemType Directory -Force $output | Out-Null
Push-Location $repo
try {
    & cl /nologo /c /W3 /MT /O2 /utf-8 "/Fo:$output/vm_video_decode.obj" src/backend_win/vm_video_decode.c
    if ($LASTEXITCODE) { throw 'Production C decoder build failed' }
    & cl /nologo /EHsc /std:c++17 /MT /O2 /utf-8 "/Fo:$output/" "/Fe:$output/production-backend-probe.exe" src/backend_win/vm_video_decode_d3d12.cpp tools/win/hevc444-probe/production-backend-probe.cpp "$output/vm_video_decode.obj" /link d3d11.lib d3d12.lib dxgi.lib dxguid.lib mf.lib mfplat.lib mfuuid.lib ole32.lib
    if ($LASTEXITCODE) { throw 'Production backend probe link failed' }
    if ($Run) {
        & "$output/production-backend-probe.exe"
        if ($LASTEXITCODE) { throw "Production backend probe failed: $LASTEXITCODE" }
    }
} finally { Pop-Location }
