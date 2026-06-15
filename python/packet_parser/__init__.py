# Shim: packet_parser content moved to plugins/star_resonance_plugin/protocol/
try:
    from plugins.star_resonance_plugin.protocol import *  # noqa: F401,F403
except ImportError:
    pass
