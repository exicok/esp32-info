#!/usr/bin/env python3
import psutil
import requests
import time
import win32gui
import win32process
import os

# ESP32的IP地址
ESP_IP = "192.168.1.100"
ESP_PORT = 80

# 全局变量
fps_counter = 0
last_time = time.time()
last_fps = 60  # 默认FPS值


def get_active_window_title():
    """获取当前活动窗口的标题"""
    try:
        hwnd = win32gui.GetForegroundWindow()
        title = win32gui.GetWindowText(hwnd)
        return title
    except Exception as e:
        return "Unknown"


def get_cpu_usage():
    """获取CPU使用率"""
    return psutil.cpu_percent(interval=0.1)


def get_memory_usage():
    """获取内存使用率"""
    memory = psutil.virtual_memory()
    return memory.percent


def get_gpu_usage():
    """获取GPU使用率（模拟值）"""
    # 实际项目中可以使用pynvml等库获取真实GPU使用率
    import random
    return random.uniform(10, 80)


def get_gpu_fan_speed():
    """获取GPU风扇速度（模拟值）"""
    # 实际项目中可以使用pynvml等库获取真实风扇速度
    import random
    return random.randint(1000, 3000)


def calculate_fps():
    """计算当前窗口的FPS"""
    global last_time, last_fps
    current_time = time.time()
    elapsed = current_time - last_time
    
    if elapsed >= 1.0:
        # 这里使用随机值模拟FPS变化
        import random
        fps = random.uniform(30, 120)  # 模拟30-120之间的FPS
        last_time = current_time
        last_fps = fps
        return fps
    return last_fps


def get_network_info():
    """获取网络信息"""
    try:
        import socket
        # 获取网络名称
        network_name = "Unknown"
        try:
            import subprocess
            result = subprocess.run(['netsh', 'wlan', 'show', 'interfaces'], 
                                  capture_output=True, text=True, encoding='gbk')
            for line in result.stdout.split('\n'):
                if 'SSID' in line and 'BSSID' not in line:
                    network_name = line.split(':')[1].strip()
                    break
        except:
            pass
        
        # 获取本机IP
        hostname = socket.gethostname()
        ip_address = socket.gethostbyname(hostname)
        
        # 获取WiFi连接设备数量（简化实现）
        wifi_devices = "1"  # 默认显示1（本机）
        
        return network_name, ip_address, wifi_devices
    except Exception as e:
        print(f"获取网络信息失败: {e}")
        return "Unknown", "0.0.0.0", "0"


def send_data_to_esp(fps, data1, data2, data3, data4):
    """发送数据到ESP32"""
    try:
        url = f"http://{ESP_IP}:{ESP_PORT}/api/system"
        
        # 获取网络信息
        network_name, pc_ip, wifi_devices = get_network_info()
        
        data = {
            "fps": int(fps),
            "data1": data1,
            "data2": data2,
            "data3": data3,
            "data4": data4,
            "network": network_name,
            "pcip": pc_ip,
            "devices": wifi_devices
        }
        response = requests.post(url, data=data, timeout=2)
        return response.status_code == 200
    except Exception as e:
        print(f"发送数据失败: {e}")
        return False


def main():
    """主函数"""
    print("系统监控器启动...")
    print(f"ESP32 IP: {ESP_IP}")
    print("按Ctrl+C退出")
    
    try:
        while True:
            # 获取系统信息
            fps = calculate_fps()
            cpu = get_cpu_usage()
            gpu = get_gpu_usage()
            fan_speed = get_gpu_fan_speed()
            mem = get_memory_usage()
            window_title = get_active_window_title()
            
            # 准备第二页的4个数据项
            # 卡片1: 处理器占有率
            data1 = f"{cpu:.1f}%"
            # 卡片2: GPU使用率和显卡风扇速度
            data2 = f"GPU: {gpu:.1f}%\nGPU Fan: {fan_speed} RPM"
            # 卡片3: 内存使用率
            data3 = f"{mem:.1f}%"
            # 卡片4: 当前窗口名称（最多28个字符，支持2行显示）
            data4 = window_title[:28] if len(window_title) > 28 else window_title
            
            # 发送数据到ESP32
            success = send_data_to_esp(fps, data1, data2, data3, data4)
            
            # 显示信息
            status = "成功" if success else "失败"
            print(f"窗口: {window_title[:30]:<30} FPS: {fps:.1f} | CPU: {cpu:.1f}% | GPU: {gpu:.1f}% | 风扇: {fan_speed} RPM | 内存: {mem:.1f}% | 发送: {status}")
            
            # 每1秒发送一次数据
            time.sleep(1)
    except KeyboardInterrupt:
        print("\n监控器已停止")


if __name__ == "__main__":
    main()
