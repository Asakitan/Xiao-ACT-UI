# Shim: proto content moved to plugins/star_resonance_plugin/proto/
# This __init__.py remains so existing `from proto import ...` keeps working
# during the transition period (Phase 5A). Will be removed in Phase 5E.
try:
    from plugins.star_resonance_plugin.proto import *  # noqa: F401,F403
except ImportError:
    pass
