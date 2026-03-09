#!/usr/bin/env python3
"""
系统监控器 - 带GUI和系统托盘功能
"""
import psutil
import requests
import time
import win32gui
import win32process
import os
import sys
import pystray
from PIL import Image, ImageDraw
import threading
import configparser
import winreg


class SystemMonitor:
    def __init__(self):
        self.esp_ip = "192.168.1.9"
        self.esp_port = 80
        self.running = False
        self.thread = None
        self.fps_counter = 0
        self.last_time = time.time()
        self.config_file = "config.ini"
        self.load_config()
        
    def load_config(self):
        """加载配置"""
        if os.path.exists(self.config_file):
            config = configparser.ConfigParser()
            config.read(self.config_file)
            if "ESP32" in config:
                self.esp_ip = config["ESP32"].get("ip", "192.168.1.100")
                self.esp_port = int(config["ESP32"].get("port", "80"))
        else:
            self.save_config()
    
    def save_config(self):
        """保存配置"""
        config = configparser.ConfigParser()
        config["ESP32"] = {
            "ip": self.esp_ip,
            "port": str(self.esp_port)
        }
        with open(self.config_file, "w") as f:
            config.write(f)
    
    def get_active_window_title(self):
        """获取当前活动窗口的标题"""
        try:
            hwnd = win32gui.GetForegroundWindow()
            title = win32gui.GetWindowText(hwnd)
            return title
        except Exception as e:
            return "Unknown"
    
    def get_cpu_usage(self):
        """获取CPU使用率"""
        return psutil.cpu_percent(interval=0.1)
    
    def get_memory_usage(self):
        """获取内存使用率"""
        memory = psutil.virtual_memory()
        return memory.percent
    
    def get_gpu_usage(self):
        """获取GPU使用率（模拟值）"""
        import random
        return random.uniform(10, 80)
    
    def calculate_fps(self):
        """计算当前窗口的FPS"""
        current_time = time.time()
        elapsed = current_time - self.last_time
        
        if elapsed >= 1.0:
            # 这里使用固定值模拟FPS，实际项目中可以使用更准确的方法
            # 例如使用win32api获取窗口的实际刷新率
            import random
            fps = random.uniform(30, 120)  # 模拟30-120之间的FPS
            self.last_time = current_time
            return fps
        return self.last_fps
    
    def __init__(self):
        self.esp_ip = "192.168.1.100"
        self.esp_port = 80
        self.running = False
        self.thread = None
        self.fps_counter = 0
        self.last_time = time.time()
        self.last_fps = 60  # 默认FPS值
        self.config_file = "config.ini"
        self.load_config()
    
    def send_data_to_esp(self, fps, cpu, gpu, mem):
        """发送数据到ESP32"""
        try:
            url = f"http://{self.esp_ip}:{self.esp_port}/api/system"
            data = {
                "fps": int(fps),
                "cpu": int(cpu),
                "gpu": int(gpu),
                "mem": int(mem)
            }
            response = requests.post(url, data=data, timeout=2)
            return response.status_code == 200
        except Exception as e:
            print(f"发送数据失败: {e}")
            return False
    
    def monitor_thread(self):
        """监控线程"""
        while self.running:
            # 获取系统信息
            fps = self.calculate_fps()
            cpu = self.get_cpu_usage()
            gpu = self.get_gpu_usage()
            mem = self.get_memory_usage()
            window_title = self.get_active_window_title()
            
            # 发送数据到ESP32
            success = self.send_data_to_esp(fps, cpu, gpu, mem)
            
            # 显示信息
            status = "成功" if success else "失败"
            print(f"窗口: {window_title[:30]:<30} FPS: {fps:.1f} | CPU: {cpu:.1f}% | GPU: {gpu:.1f}% | 内存: {mem:.1f}% | 发送: {status}")
            
            # 每1秒发送一次数据
            time.sleep(1)
    
    def start(self):
        """开始监控"""
        if not self.running:
            self.running = True
            self.thread = threading.Thread(target=self.monitor_thread, daemon=True)
            self.thread.start()
            print("监控已启动")
    
    def stop(self):
        """停止监控"""
        if self.running:
            self.running = False
            if self.thread:
                self.thread.join(timeout=2)
            print("监控已停止")


def create_tray_icon():
    """创建系统托盘图标"""
    # 创建一个简单的图标
    image = Image.new('RGB', (64, 64), color='blue')
    draw = ImageDraw.Draw(image)
    draw.text((10, 20), 'SM', fill='white', font_size=30)
    
    monitor = SystemMonitor()
    
    def on_quit(icon, item):
        monitor.stop()
        icon.stop()
    
    def on_start(icon, item):
        monitor.start()
    
    def on_stop(icon, item):
        monitor.stop()
    
    menu = pystray.Menu(
        pystray.MenuItem('启动监控', on_start),
        pystray.MenuItem('停止监控', on_stop),
        pystray.MenuItem('退出', on_quit)
    )
    
    icon = pystray.Icon('系统监控器', image, '系统监控器', menu)
    return icon, monitor


def add_to_startup():
    """添加到开机自启动"""
    try:
        # 获取当前脚本的路径
        script_path = os.path.abspath(__file__)
        
        # 打开注册表
        key = winreg.OpenKey(winreg.HKEY_CURRENT_USER, 
                           r'SOFTWARE\Microsoft\Windows\CurrentVersion\Run',
                           0, winreg.KEY_SET_VALUE)
        
        # 添加自启动项
        winreg.SetValueEx(key, "SystemMonitor", 0, winreg.REG_SZ, script_path)
        winreg.CloseKey(key)
        print("已添加到开机自启动")
    except Exception as e:
        print(f"添加开机自启动失败: {e}")


def remove_from_startup():
    """从开机自启动中移除"""
    try:
        key = winreg.OpenKey(winreg.HKEY_CURRENT_USER, 
                           r'SOFTWARE\Microsoft\Windows\CurrentVersion\Run',
                           0, winreg.KEY_SET_VALUE)
        
        winreg.DeleteValue(key, "SystemMonitor")
        winreg.CloseKey(key)
        print("已从开机自启动中移除")
    except Exception as e:
        print(f"移除开机自启动失败: {e}")


def main():
    """主函数"""
    # 检查是否以管理员权限运行
    if not os.name == 'nt':
        print("此功能仅支持Windows系统")
        return
    
    # 安装依赖
    print("正在检查依赖...")
    try:
        import pystray
        import PIL
    except ImportError:
        print("正在安装缺失的依赖...")
        import subprocess
        subprocess.check_call([sys.executable, "install_deps.py"])
    
    # 添加到开机自启动
    add_to_startup()
    
    # 创建系统托盘图标
    icon, monitor = create_tray_icon()
    
    # 自动开始监控
    monitor.start()
    
    # 运行系统托盘
    icon.run()


if __name__ == "__main__":
    main()
