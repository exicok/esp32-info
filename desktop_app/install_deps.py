#!/usr/bin/env python3
"""
自动安装依赖包的脚本
"""
import subprocess
import sys


def install_dependencies():
    """安装依赖包"""
    print("正在安装依赖包...")
    try:
        # 升级pip
        subprocess.check_call([sys.executable, "-m", "pip", "install", "--upgrade", "pip"])
        # 安装依赖
        subprocess.check_call([sys.executable, "-m", "pip", "install", "psutil", "requests", "pywin32", "pystray", "Pillow"])
        print("依赖包安装成功！")
        return True
    except Exception as e:
        print(f"安装依赖包失败: {e}")
        return False


if __name__ == "__main__":
    install_dependencies()
