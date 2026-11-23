# Home decorative surfaces

These images provide the App home screen backgrounds.
Labels, icons, state, and touch targets remain live LVGL objects.

- `page`: the radial highlight over the 180° page gradient.
- `weather`: the white 160° card gradient with the orange circle blurred by 24 CSS pixels.
- `ai`: the gray 160° surface with the orange ellipse blurred by 64 CSS pixels.
- `plan`, `quick`, `settings`: the gray 160° surface.
- `focus`, `todo`: the white 160° card gradient and white inset highlight.
- `reminder`: the dark 160° gradient.

The current Figma Make frame is native 720×720. Each bitmap matches its LVGL object's bounds and is drawn without scaling; LVGL clips rounded corners. The source gradients keep their Figma colors.

The icons at their home-screen sizes are in `assets/home/icons`.

PNG files are the inspectable source renders. Runtime uses `.bin` files with an LVGL v9 little-endian RGB888 header and BGR pixels. The built-in decoder streams rows instead of allocating a complete page PNG in the LVGL heap. Deploy `assets/home` to `/oem/assets/home` to include both surfaces and icons.
