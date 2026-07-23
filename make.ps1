# One-click build for KiCad Forge
$env:PATH = "D:\msys64\mingw64\bin;$env:PATH"
xmake f --toolchain=clang -m debug -c
xmake build
Write-Host "Done. Output: build\mingw\x86_64\debug\KiCad_Forge.exe"
