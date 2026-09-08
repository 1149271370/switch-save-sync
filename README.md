# 潜水员戴夫 PC↔Switch 存档转换

把 PC 端 SteamSData 里的 `.sav` 存档与 Switch JKSV 导出的无后缀存档互转。

## 使用步骤

1. Switch 先启动一次完整版《潜水员戴夫》，推进到首次能存档或自动存档后退出。
2. 用 JKSV 新建备份（如 `baseline`），并关闭 `Export saves to ZIP`。
3. 把 `JKSV/DAVE THE DIVER/baseline/` 整个文件夹拷到电脑。
4. 打开 `DaveDiverSaveTransfer.exe`。
5. PC 存档目录一般会自动检测到；Switch 目录选择刚拷下来的 `baseline` 文件夹。
6. 选择方向后点击“预览”确认文件，再点击“开始转换”。
7. 转换完成后把 `baseline` 文件夹放回 SD 卡原位置，再从 JKSV 恢复该备份。

每次转换前工具都会把目标目录自动备份到 `%LOCALAPPDATA%\DaveDiverSaveTransfer\backups\`。

## DBI/MTP 直连模式

如果 Switch 已经通过 DBI 的 MTP Responder 连接电脑，可以不用手动复制存档目录：

1. 在 DBI 中开启 `Run MTP responder`，让电脑里出现 `此电脑 > Switch`。
2. 打开工具后勾选“直连 DBI/MTP Switch”。
3. PC 存档目录保持自动检测，选择方向后点击“预览”和“开始转换”。
4. PC → Switch 会自动导出并备份当前 Switch 存档、写入 PC 存档并校验文件。
5. Switch → PC 会自动导出 Switch 存档、补上 `.sav` 后覆盖 PC 存档。

直连模式会把 Switch 原存档备份到
`%LOCALAPPDATA%\DaveDiverSaveTransfer\backups\`。

## 重新构建 exe

在 Windows 下双击 `build.bat`，产物输出到 `dist\DaveDiverSaveTransfer.exe`。

## 注意事项

- 转换只改文件名后缀，不改文件内容。
- 试玩版与完整版、版本差距过大的存档不建议互转。
- 导入后若黑屏、卡读取或进新档，请先确认 Switch 与 PC 的游戏版本一致或足够接近。
