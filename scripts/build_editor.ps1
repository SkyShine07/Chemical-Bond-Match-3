# 触发 ChemicalBondEditor 的 UBT 编译验证（Win64 Development）。
# 用法: powershell -ExecutionPolicy Bypass -File scripts\build_editor.ps1
# 退出码 0 表示编译成功，非 0 表示失败。

$ErrorActionPreference = 'Stop'

$EngineRoot = 'D:\UnrealEngine\UE_5.7'
$BuildBat   = Join-Path $EngineRoot 'Engine\Build\BatchFiles\Build.bat'
$Project    = 'D:\Unreal Projects\Chemical-Bond-Match-3\ChemicalBond.uproject'
$Target     = 'ChemicalBondEditor'
$Platform   = 'Win64'
$Config     = 'Development'

if (-not (Test-Path $BuildBat)) { Write-Error "Build.bat 不存在: $BuildBat"; exit 2 }
if (-not (Test-Path $Project))  { Write-Error "项目文件不存在: $Project"; exit 2 }

Write-Host "[build_editor] $Target $Platform $Config"
& $BuildBat $Target $Platform $Config -Project="$Project" -WaitMutex -NoHotReload
$code = $LASTEXITCODE
Write-Host "[build_editor] UBT exit code = $code"
exit $code
