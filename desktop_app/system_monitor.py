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


def send_data_to_esp(fps, cpu, gpu, mem):
    """发送数据到ESP32"""
    try:
        url = f"http://{ESP_IP}:{ESP_PORT}/api/system"
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
            mem = get_memory_usage()
            window_title = get_active_window_title()
            
            # 发送数据到ESP32
            success = send_data_to_esp(fps, cpu, gpu, mem)
            
            # 显示信息
            status = "成功" if success else "失败"
            print(f"窗口: {window_title[:30]:<30} FPS: {fps:.1f} | CPU: {cpu:.1f}% | GPU: {gpu:.1f}% | 内存: {mem:.1f}% | 发送: {status}")
            
            # 每1秒发送一次数据
            time.sleep(1)
    except KeyboardInterrupt:
        print("\n监控器已停止")


if __name__ == "__main__":
    main()
