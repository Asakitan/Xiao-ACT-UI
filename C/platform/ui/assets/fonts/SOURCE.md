# Product UI fonts

Existing project font copied byte-for-byte from the frozen legacy asset, not from a system font install.

- Family: SAO UI / Regular
- SHA-256: 475e3483b109a12342116ce159211b3b2af733caa70b9822d6d306e66a4adf1e
- Related author reference: https://www.deviantart.com/darkblackswords/art/Sword-Art-Online-Font-Download-426603647
- Repository source index: https://github.com/SAO-UI/sao-assets
- The existing font contains no license text. The referenced fan-font author limits use to personal/fan work. This is not a new commercial redistribution grant; review the original terms before distribution.
- Chinese resource: `ZhuZiAYuanJWD.ttf`, copied unchanged from `python/assets/fonts/ZhuZiAYuanJWD.ttf`.
- Chinese family reported by the file: `方正FW筑紫A圆 简 D`; DirectWrite resolves the embedded collection's actual family name.
- Chinese SHA-256: b82a8d44e1e75dcc9aa83292ec4bcd00c2b9e89838244a33b7d1a480287166b0.
- Both files are private RCDATA resources (701/702). All native text roles, including technical values, share the Latin/CJK mapping; no machine-wide registration occurs.
- HTML uses the same bytes with character-range font faces. SAO UI has proportional advances; terminal-cell alignment and unsupported-glyph behavior are not implied by changing the font family.
