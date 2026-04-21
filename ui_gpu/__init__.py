"""GPU-native rewrite of the SAO popup menu.

Replaces the legacy hybrid Tk + chroma-key + GPU painter pipeline in
``sao_theme.py`` with a single interactive ``GpuOverlayWindow`` that
owns all menu/child-bar/HUD pixels and input. A small chroma-key
``tk.Toplevel`` shell is kept only to host an optional ``left_widget``
factory frame and to receive the global Alt+A hotkey.

Public surface kept identical to the old class so ``sao_gui.py``
construction site does not change::

    from ui_gpu import SAOPopUpMenu
"""

from .popup import SAOPopUpMenu

__all__ = ["SAOPopUpMenu"]
