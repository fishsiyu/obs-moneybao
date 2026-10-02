# OBS-moneybao / OBS-金钱豹

这是一个 OBS 原生视频来源。选择图片后，它会在 OBS 画布内匀速移动，并在碰到任意边缘时反射转向，类似 DVD 屏保标志。

## 可调参数

- 图片文件：选择要显示的图片。
- 移动速度：图片沿当前方向的实际速度，单位像素/秒；设为 0 可暂停，对角移动不会额外加速。
- 初始方向：角度按屏幕坐标计算，0 度向右，90 度向下，45 度向右下。
- 图片大小：相对原图的百分比。
- 重置位置：在来源属性中将图片移回画布中心，当前运动方向保持不变。
- 动态 GIF：按原动画帧率播放；其他静态图片仍可正常使用。

## 构建

Windows 预设面向 Visual Studio 2026、Windows SDK 10.0.26100 和 OBS Studio 32.2.2。首次配置会从 OBS 官方仓库下载并构建开发依赖，需要网络连接。安装到默认 OBS 插件目录需要管理员权限。

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64 --config RelWithDebInfo
cmake --install build_x64 --config RelWithDebInfo
```

构建产物位于 `build_x64/rundir/RelWithDebInfo/`。安装命令会复制到 `C:/ProgramData/obs-studio/plugins/obs-moneybao/`（文件夹名取自 `buildspec.json` 的 `name`）。“来源”中添加“OBS-moneybao”，选择图片并调整速度、方向和大小，重启 OBS 后生效。

## 打包 Release

本地打包：先构建，再把插件安装到一个独立的 release 目录，最后压缩该目录内容。

```powershell
cmake --build --preset windows-x64 --config RelWithDebInfo
cmake --install build_x64 --config RelWithDebInfo --prefix "$PWD/release/RelWithDebInfo"

$zip = "$PWD/release/obs-moneybao-1.0.0-windows-x64.zip"
Remove-Item $zip -ErrorAction SilentlyContinue
Compress-Archive -Path "$PWD/release/RelWithDebInfo/*" -DestinationPath $zip -CompressionLevel Optimal
```

发布到 GitHub：推送一个符合 `主版本.次版本.修订号` 的标签，`push.yaml` 工作流会在 CI 上构建各平台产物并创建 draft release。

```powershell
git tag 1.0.0
git push origin 1.0.0
```
