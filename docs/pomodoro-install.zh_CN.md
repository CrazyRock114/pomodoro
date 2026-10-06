<p align="right">
  <a href="pomodoro-install.md">English</a> · <strong>简体中文</strong>
</p>

# 番茄钟固件安装

通过从 `0x0` 烧录合并镜像，把三键番茄钟固件安装到任意 FoloToy AI Passport
（ESP32-C3，8MB Flash）。

## 准备

- 一台装有 [esptool](https://docs.espressif.com/projects/esptool/) 的电脑
  （`pip install esptool`）。
- 一根**可传数据**的 USB 线（纯充电线无法识别设备）。
- 合并镜像 `FoloToy-AI-Passport-full.bin`：从
  [GitHub Release](https://github.com/CrazyRock114/pomodoro/releases) 下载，
  或用 `./tools/validate.sh --firmware` 在本地构建产出
  `build/FoloToy-AI-Passport-full.bin`。

## 安装（macOS / Linux）

```bash
./tools/install-firmware.sh FoloToy-AI-Passport-full.bin
```

脚本会自动探测 USB 串口；接了多个设备时可用
`./tools/install-firmware.sh <镜像> <端口>` 显式指定。

## 安装（Windows）

```bat
tools\install-firmware.bat FoloToy-AI-Passport-full.bin COM5
```

也可以使用乐鑫 **Flash Download Tool**：芯片类型选 ESP32-C3，镜像加载到
偏移 `0x0`，点 START。

## 说明

- 烧录完成后设备自动重启进入番茄钟应用。
- 烧录合并镜像会覆盖从 `0x0` 起的整个区域（含 NVS），先前固件保存的设置会被重置。
- 按键：UP/DOWN 调整分钟数（长按连续步进），OK 开始/暂停，设置页长按 OK 返回；
  设置项包含步幅、提醒方式（静音闪屏/蜂鸣/语音）、音量、自动休眠时长。
  闲置时设备自动休眠，任意按键唤醒。

相关：[发布页](https://github.com/CrazyRock114/pomodoro/releases)。
