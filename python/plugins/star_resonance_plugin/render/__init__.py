# -*- coding: utf-8 -*-
"""Star Resonance plugin render helpers (SkillFX GPU pipeline + Cython kernels).

Game-specific render code that used to live under platform ``render/`` was
relocated here in the 5.0.0 platform/plugin separation pass.  The platform's
``render/`` package only retains generic GPU/overlay infrastructure
(``gpu_renderer``, ``gpu_overlay_window``, ``overlay_render_worker``, etc.).
"""
