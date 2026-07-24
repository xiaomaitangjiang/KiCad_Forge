$env:PATH = "D:\msys64\mingw64\bin;$env:PATH"
xmake f -p mingw -m release -c 2>$null
xmake build
if ($LASTEXITCODE -eq 0) { Write-Host "`nDone. Output: build\mingw\x86_64\release\KiCad_Forge.exe" }
