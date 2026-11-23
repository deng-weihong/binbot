# Runtime fonts

The device UI loads these fonts through LVGL FreeType:

- `NotoSansSC-wght.ttf` is the upstream Chinese variable font.
- `Manrope-wght.ttf` is the upstream numeric variable font.
- `NotoSansSC-Medium.ttf` and `NotoSansSC-SemiBold.ttf` are the runtime Chinese fonts.
- `Manrope-Medium.ttf` and `Manrope-SemiBold.ttf` are the runtime numeric fonts.
- `DejaVuSans-IPA.ttf` is a small DejaVu Sans subset used as the fallback for
  IPA symbols in the vocabulary screen. Its license is included in
  `DejaVuSans-LICENSE.txt`.

Both files come from the official Google Fonts repository and are licensed under the SIL Open Font License. The corresponding license texts are stored beside the fonts.

The runtime files are fixed-weight instances generated from the upstream
variable fonts. This prevents FreeType from silently selecting the variable
fonts' Thin/ExtraLight defaults on the embedded target.
