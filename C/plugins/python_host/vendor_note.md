# python_host — CPython 分发来源

## 选项 1: vcpkg (推荐 dev)

```
vcpkg install python3
```

链接 ``Python3::Python``。CMake ``find_package(Python3 3.11 COMPONENTS
Development.Embed REQUIRED)``。

## 选项 2: 官方 embeddable package (推荐发布)

Windows Python embeddable zip (从 https://www.python.org/downloads/windows/
拉 "Windows embeddable package (64-bit)"):

```
runtime/python-embed/
    python.exe
    python3.dll
    python3XX.dll
    python3XX.zip     (stdlib 打包)
    Lib/site-packages/   (SAO 自己按需 vendor pip 包进这)
```

在 py_host_config.python_home 里指向这个目录, PYTHONHOME + PYTHONPATH
在 Py_Initialize 之前设好。

## 冻结态 (Nuitka onedir 打包)

Nuitka 直接嵌入 CPython, 不需要额外 embed 分发。python_host.py_host_config
把 python_home 传 ``dirname(argv[0])`` + 猜 python3XX.dll 位置。

## 关键 Python 依赖 (随包 vendor)

平台自己不装 pip, 但把常用第三方包 vendor 进 ``Lib/site-packages/``:

- ``numpy`` (mem_probe cython 相关)
- ``opencv-python`` (hide_seek 插件用)
- ``pillow`` (UI 缩略图)
- ``lupa`` (旧 lua 插件的兼容路径, 新平台走 lua_host 就不必)
- ``pythonnet`` (旧 csharp 插件的兼容路径, 新平台走 csharp_host 就不必)

## 版本兼容

只支持 CPython 3.11 + 3.12 + 3.13。3.10 因为 PEP 657 error tracing 差异
拒。3.14 待评估。
