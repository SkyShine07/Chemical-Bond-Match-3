# 项目脚本登记

| 脚本 | 用途 | 输入 | 输出 | 命令示例 | 维护备注 |
|---|---|---|---|---|---|
| `build_editor.ps1` | 触发 ChemicalBondEditor 的 UBT 编译验证 | 无（路径在脚本内固定：UE 5.7、ChemicalBond.uproject） | UBT 编译日志；退出码 0=成功 | `powershell -ExecutionPolicy Bypass -File scripts\build_editor.ps1` | 2026-07-01 新建，供 Censor 交付前编译验证调用 |
| `iwata-inspect-blueprints.py` | 检视 Blueprint 资产 | — | — | — | 既有 |
| `setup_playground_bp.py` | 配置 playground 蓝图 | — | — | — | 既有 |
