param(
    [string]$Qt = 'C:\Qt\6.10.1\msvc2022_64',
    [string]$Baseline = 'HEAD~1',
    [string]$Output = 'build\glass-bench'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root
$out = [IO.Path]::GetFullPath($Output)
if (Test-Path $out) { throw "Use a new output directory: $out" }
New-Item -ItemType Directory -Path $out | Out-Null
$ref = (& git rev-parse "$Baseline^{commit}").Trim()
if ($LASTEXITCODE -ne 0) { throw 'Cannot resolve baseline' }
& git archive --format=zip --output="$out\baseline.zip" $ref
if ($LASTEXITCODE -ne 0) { throw 'Cannot archive baseline' }
Expand-Archive "$out\baseline.zip" "$out\source-before"
# Identical fixed-workload instrumentation; historical rendering code stays untouched.
Copy-Item "$root\tests\gtest\glass_benchmark_test.cpp" "$out\source-before\tests\gtest\glass_benchmark_test.cpp"
Add-Content "$out\source-before\CMakeLists.txt" "`ntarget_sources(ashdesk_ui_tests PRIVATE tests/gtest/glass_benchmark_test.cpp)"
# These local dependency binaries are deliberately not tracked in Git.
foreach ($dir in 'googletest\include', 'googletest\lib', 'openssl\include', 'openssl\lib', 'openssl\bin') {
    Copy-Item -Recurse "$root\third_party\$dir" "$out\source-before\third_party\$dir"
}
$manifest = [ordered]@{
    baseline_commit = $ref
    after_base_commit = (& git rev-parse HEAD).Trim()
    configuration = 'RelWithDebInfo; Ninja'
    qt_version = (& "$Qt\bin\qmake.exe" -query QT_VERSION)
    sources = @{}
}
foreach ($variant in 'before', 'after') {
    $src = if ($variant -eq 'before') { "$out\source-before" } else { $root }
    $build = "$out\$variant"
    & "$PSScriptRoot\vsrun.cmd" cmake -S "$src" -B "$build" -G Ninja `
        -DCMAKE_BUILD_TYPE=RelWithDebInfo `
        "-DCMAKE_PREFIX_PATH=$Qt" *> "$out\$variant-configure.txt"
    if ($LASTEXITCODE -ne 0) { throw "$variant configure failed; see log" }
    $targets = @('ashdesk_ui_tests', 'ashdesk')
    if ($variant -eq 'after') { $targets += 'test_glass' }
    & "$PSScriptRoot\vsrun.cmd" cmake --build "$build" --parallel 2 --target $targets `
        *> "$out\$variant-build.txt"
    if ($LASTEXITCODE -ne 0) { throw "$variant build failed; see log" }
    & "$Qt\bin\windeployqt.exe" --release --no-translations --no-compiler-runtime "$build\ashdesk_ui_tests.exe" `
        *> "$out\$variant-deploy.txt"
    if ($LASTEXITCODE -ne 0) { throw "$variant Qt deployment failed; see log" }
    Copy-Item "$Qt\plugins\platforms\qoffscreen.dll" "$build\platforms\qoffscreen.dll"
    $hashes = @{}
    $files = @(Get-ChildItem "$src\ui" -Recurse -File)
    $files += Get-Item "$src\CMakeLists.txt", "$src\tests\gtest\desktop_ui_test.cpp", "$src\tests\gtest\glass_benchmark_test.cpp"
    if ($variant -eq 'after') {
        $files += Get-ChildItem "$src\core\kernels\ad_glass*"
        $files += Get-Item "$src\tests\test_glass.c"
    }
    foreach ($file in $files) {
        $name = $file.FullName.Substring($src.Length + 1)
        $hashes[$name] = (Get-FileHash $file.FullName -Algorithm SHA256).Hash
    }
    $manifest.sources[$variant] = $hashes
}
$manifest | ConvertTo-Json -Depth 6 | Set-Content "$out\build.json" -Encoding utf8
Write-Output "Built baseline $ref and worktree in $out"
