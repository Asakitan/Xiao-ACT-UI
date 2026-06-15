# -*- coding: utf-8 -*-
"""
SAO Auto — Npcap 自动安装

检测系统是否安装了 Npcap (wpcap.dll),
未安装时自动下载并静默安装。
"""

import os
import sys
import subprocess
import logging
import urllib.request
import tempfile

logger = logging.getLogger('sao_auto.npcap')

# 固定版本 — 避免 API 不兼容
NPCAP_VERSION = '1.80'
NPCAP_URL = f'https://npcap.com/dist/npcap-{NPCAP_VERSION}.exe'
NPCAP_DLL_PATH = os.path.join(
    os.environ.get('SystemRoot', r'C:\Windows'),
    'System32', 'Npcap', 'wpcap.dll'
)


def is_npcap_installed() -> bool:
    """检查 Npcap 是否已安装"""
    return os.path.isfile(NPCAP_DLL_PATH)


def ensure_npcap(silent: bool = True) -> tuple:
    """
    确保 Npcap 已安装。

    Returns:
        (success: bool, message: str)
    """
    if is_npcap_installed():
        logger.info(f'[Npcap] 已安装: {NPCAP_DLL_PATH}')
        return True, 'Npcap 已安装'

    logger.info(f'[Npcap] 未检测到 wpcap.dll, 开始自动安装...')

    # ── 下载 ──
    try:
        tmp_dir = tempfile.gettempdir()
        installer_path = os.path.join(tmp_dir, f'npcap-{NPCAP_VERSION}.exe')

        # 如果已有缓存的安装包, 跳过下载
        if not os.path.isfile(installer_path) or os.path.getsize(installer_path) < 100_000:
            logger.info(f'[Npcap] 下载 {NPCAP_URL} ...')
            print(f'[Npcap] 正在下载 Npcap {NPCAP_VERSION} ...')
            urllib.request.urlretrieve(NPCAP_URL, installer_path)
            logger.info(f'[Npcap] 下载完成: {installer_path}')
        else:
            logger.info(f'[Npcap] 使用缓存安装包: {installer_path}')

    except Exception as e:
        msg = f'Npcap 下载失败: {e}'
        logger.error(f'[Npcap] {msg}')
        return False, msg

    # ── 静默安装 ──
    try:
        cmd = [installer_path]
        if silent:
            # /S = 静默  /winpcap_mode=no = 不安装 WinPcap 兼容层
            cmd.extend(['/S', '/winpcap_mode=no'])

        logger.info(f'[Npcap] 执行安装: {" ".join(cmd)}')
        print(f'[Npcap] 正在静默安装 Npcap {NPCAP_VERSION} ...')

        # 需要管理员权限 — 使用 ShellExecuteW runas
        import ctypes
        ret = ctypes.windll.shell32.ShellExecuteW(
            None, 'runas', installer_path,
            '/S /winpcap_mode=no' if silent else '',
            None, 0  # SW_HIDE
        )
        # ShellExecuteW 返回 >32 表示成功
        if ret <= 32:
            msg = f'Npcap 安装启动失败 (ShellExecute 返回 {ret})'
            logger.error(f'[Npcap] {msg}')
            return False, msg

        # 等待安装完成 (最长 60 秒)
        import time
        for i in range(60):
            time.sleep(1)
            if is_npcap_installed():
                logger.info(f'[Npcap] 安装成功! ({i+1}s)')
                print(f'[Npcap] 安装成功!')
                return True, 'Npcap 安装成功'

        # 超时但再检查一次
        if is_npcap_installed():
            return True, 'Npcap 安装成功'

        msg = 'Npcap 安装超时 (60s), 请手动安装: https://npcap.com'
        logger.warning(f'[Npcap] {msg}')
        return False, msg

    except Exception as e:
        msg = f'Npcap 安装失败: {e}'
        logger.error(f'[Npcap] {msg}')
        return False, msg
