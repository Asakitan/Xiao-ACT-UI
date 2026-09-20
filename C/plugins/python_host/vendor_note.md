# python_host — CPython 分发来源

## 选项 1: vcpkg (推荐 dev)

```
vcpkg install python3
```

Windows构建通过 ``find_package(Python3 3.11 COMPONENTS Development.Embed QUIET)``
取得headers。MSVC仅消费``Python3::Python``的include目录，并用
``/NODEFAULTLIB:python3xx.lib``阻断``pyconfig.h``的默认导入库提示；运行时由
``py_import_stubs.cpp``在``python_home``中动态加载DLL并解析required exports。
找不到dev headers时host以unavailable形态构建，不中止整个native产品构建。

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

## 旧 Python 产品冻结态 (Nuitka onedir)

该路线只属于legacy Python产品，不是当前native launcher的runtime contract。native
``python_host``必须收到明确``python_home``，不会从``argv[0]``猜测或下载runtime。

## 关键 Python 依赖 (随包 vendor)

平台自己不装 pip, 但把常用第三方包 vendor 进 ``Lib/site-packages/``:

- ``numpy`` (mem_probe cython 相关)
- ``opencv-python`` (hide_seek 插件用)
- ``pillow`` (UI 缩略图)
- ``lupa`` (旧 lua 插件的兼容路径, 新平台走 lua_host 就不必)
- ``pythonnet`` (旧 csharp 插件的兼容路径, 新平台走 csharp_host 就不必)

## 版本兼容

构建面要求CPython 3.11+ Development.Embed；运行时DLL的major/minor必须与构建时
required-export表匹配。当前live provider probe证据为CPython 3.11.0；其他版本需按
相同load/export/init/shutdown矩阵单独验收。
