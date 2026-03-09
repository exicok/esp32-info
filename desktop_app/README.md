# 系统监控器桌面软件

## 功能
- 获取当前活动窗口的FPS
- 监控CPU使用率
- 监控GPU使用率（模拟值）
- 监控内存使用率
- 通过WiFi发送数据到ESP32
- 系统托盘图标（GUI版本）
- 开机自启动（GUI版本）

## 运行环境
- Windows系统
- Python 3.7或更高版本

## 版本选择

### 命令行版本
- **文件**: `system_monitor.py`
- **特点**: 简单直接，适合调试和查看详细输出
- **显示**: 会显示控制台窗口，实时输出监控信息

### GUI版本（推荐）
- **文件**: `system_monitor_gui.py`
- **特点**: 系统托盘图标，开机自启动
- **显示**: 会显示控制台窗口，同时有系统托盘图标

## 安装和使用
1. 安装Python 3.7+
2. 安装依赖：
   ```
   pip install -r requirements.txt
   ```
3. 确保ESP32已连接到WiFi网络
4. 运行程序：
   ```
   python system_monitor_gui.py
   ```
   或
   ```
   python system_monitor.py
   ```

## 配置
- **配置文件**: `config.ini`（GUI版本自动生成）
- **默认ESP32 IP**: 192.168.1.100
- 可以手动编辑config.ini文件修改ESP32的IP地址

## 数据格式
软件通过HTTP POST请求发送数据到ESP32的`/api/system`端点，数据格式为：
- fps: 当前窗口的FPS
- cpu: CPU使用率（百分比）
- gpu: GPU使用率（百分比）
- mem: 内存使用率（百分比）

## 注意事项
- ESP32和电脑需要在同一WiFi网络中
- 首次运行可能需要安装依赖包
- 程序会显示控制台窗口，可以看到实时监控信息
- GUI版本会在系统托盘显示图标，可以右键控制
