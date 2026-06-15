# Shim: net content moved to plugins/star_resonance_plugin/net/
try:
    from plugins.star_resonance_plugin.net import *  # noqa: F401,F403
except ImportError:
    pass
