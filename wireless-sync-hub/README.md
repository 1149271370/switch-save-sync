# Switch Save Sync Hub

基于 NX-Save-Sync 的 PC 端改造，把 Switch 存档同步到 PC 的任意游戏目录。

## 工作方式

- Switch 端使用 NX-Save-Sync 的 NRO：负责挂载 Switch 内部存档，并通过 WiFi/HTTP 收发 ZIP。
- PC 端使用本目录的 `sync_hub.py`：保存每个游戏的 Title ID、PC 目录和转换规则，记录成功历史。
- 转换规则支持 PC 端 `.sav` 后缀互转，也支持直接复制无后缀的模拟器存档。

## 安装与运行

```powershell
python -m pip install -r desktop/requirements.txt
python desktop/sync_hub.py
```

Windows 独立 exe 可在 `dist\SwitchSaveSyncHub.exe` 找到；
也可以双击 `desktop\build_pc.bat` 自行打包。

## 编译 Switch NRO

NRO 源码在 `nro-v2/`。本仓库的 GitHub Actions 会在推送后自动用 devkitPro
镜像构建 `SwitchSaveSyncHub.nro`，并把产物发布到仓库的 NRO Release。

本地构建需要 devkitPro：

```bash
cd nro-v2
make
```

如果本机已经导入 devkitPro 的 Docker 镜像，也可以直接运行：

```bash
cd nro-v2
./build-local.sh
```

产物输出为 `nro-v2/SwitchSaveSyncHub.nro`。

## 首次使用

1. 在 Switch 启动 NX-Save-Sync NRO，选择用户。
2. 在 PC Hub 中填写 Switch 的 IP，并添加游戏：
   - Title ID 使用 NX-Save-Sync 下载包内的 16 位十六进制 ID。
   - PC 目录填游戏在电脑上的存档目录。
   - 若 PC 存档是 `GameSave_*.sav`，转换规则选“PC 需要 .sav 后缀”。
3. Switch 端选好游戏并进入 Send 启动服务器，PC Hub 点“Switch → PC”。
4. PC Hub 点“PC → Switch”后，Switch 端进入 Receive 并连接 PC。

每次同步前会自动备份目标目录，历史会记录在
`%LOCALAPPDATA%\SwitchSaveSyncHub\config.json`。

## 说明

PC 端转换只负责文件后缀和目录结构，不做游戏内部二进制转换。若某个游戏的 PC/Switch 存档内部结构不同，需要为它单独扩展转换器。
