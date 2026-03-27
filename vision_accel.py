# -*- coding: utf-8 -*-
"""
Lightweight OpenCV acceleration helpers.

This keeps screen-vision preprocessing on the built-in OpenCV/OpenCL path
when the local machine supports it, without introducing extra packaged GPU
dependencies.
"""

from __future__ import annotations

import logging

import cv2

log = logging.getLogger('sao_auto.vision_accel')

_OPENCL_INIT = False
_OPENCL_ENABLED = False


def _ensure_opencl() -> bool:
    global _OPENCL_INIT, _OPENCL_ENABLED
    if _OPENCL_INIT:
        return _OPENCL_ENABLED

    _OPENCL_INIT = True
    try:
        have_opencl = bool(getattr(cv2.ocl, 'haveOpenCL', lambda: False)())
        if have_opencl:
            cv2.ocl.setUseOpenCL(True)
            _OPENCL_ENABLED = bool(cv2.ocl.useOpenCL())
    except Exception:
        _OPENCL_ENABLED = False

    if _OPENCL_ENABLED:
        log.info('[Vision] OpenCL acceleration enabled via OpenCV UMat')
    else:
        log.info('[Vision] OpenCL acceleration unavailable, using CPU OpenCV path')
    return _OPENCL_ENABLED


def to_accel(image):
    if image is None:
        return None
    if _ensure_opencl():
        try:
            return cv2.UMat(image)
        except Exception:
            return image
    return image


def from_accel(image):
    if hasattr(image, 'get'):
        try:
            return image.get()
        except Exception:
            return image
    return image


def gaussian_blur(image, ksize, sigma=0):
    return from_accel(cv2.GaussianBlur(to_accel(image), ksize, sigma))


def cvt_color(image, code):
    return from_accel(cv2.cvtColor(to_accel(image), code))


def in_range(image, lower, upper):
    return from_accel(cv2.inRange(to_accel(image), lower, upper))


def morphology(image, op, kernel, iterations=1):
    return from_accel(
        cv2.morphologyEx(to_accel(image), op, kernel, iterations=iterations)
    )


def dilate(image, kernel, iterations=1):
    return from_accel(cv2.dilate(to_accel(image), kernel, iterations=iterations))


def canny(image, threshold1, threshold2):
    return from_accel(cv2.Canny(to_accel(image), threshold1, threshold2))
