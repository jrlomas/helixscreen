# Printer Images

Shipped artwork for the printers HelixScreen knows about. Three places draw it: the
first-run configuration wizard (`src/ui/ui_wizard_printer_identify.cpp`), the Printer
Image picker overlay (`src/ui/ui_overlay_printer_image.cpp`), and the home panel's
printer widget (`src/ui/panel_widgets/printer_image_widget.cpp`).

77 PNGs, ~25 MB. PNG is the only format here; nothing loads a `.jpg` or `.webp` from
this directory.

## Which printer gets which image

`assets/config/printer_database.json` owns that mapping — each entry's `image` field
names a file in this directory. This README does not list them, because a
hand-maintained list drifts from the database silently.

```bash
scripts/check_printer_images.py --list   # entries whose image is not on disk
```

That gate fails the build when the database names art that does not exist. It matters
because a missing image is otherwise invisible: `get_prerendered_printer_path()` falls
through to `generic-corexy` and the user just sees a CoreXY frame for their bed-slinger.

## How an image reaches the screen

`src/system/prerendered_images.cpp#get_prerendered_printer_path` resolves in this
order, degrading rather than failing:

1. `prerendered/<name>-<300|150>.bin` — LVGL binary, LZ4-compressed, no PNG decode
2. `<name>.png` — this directory, decoded at full resolution by lodepng
3. `generic-corexy` at the same tier, then its PNG

The tier comes from `src/system/prerender_size_class.cpp#get_printer_image_size`:
screen width >= 600 selects 300px, narrower selects 150px.

**Step 2 is the expensive one.** These PNGs are large (see below), LVGL's image cache
is disabled (`LV_CACHE_DEF_SIZE 0` in `lv_conf.h`), and the widget draws with
`inner_align="contain"`, so a miss means decoding the full-resolution bitmap and
scaling it at draw time. Measured on a K1C: 1347 ms to first paint from
`creality-k1c.png`, 78 ms from `creality-k1c-300.bin`.

Note that the fallback in step 2 logs at `trace` while step 1 logs at `debug`, so a
device silently taking the slow path shows no `[Prerendered]` line at debug level.

### Generating the renders

```bash
make gen-printer-images      # -> build/assets/images/printers/prerendered/*.bin
make list-printer-images     # what it would write
```

Build artifacts, not committed. Every `package-*` target depends on this, and the
`deploy-*` targets generate it when the output directory is empty — so a cross-built
device gets the tiers. A plain dev checkout, `make install`, and the Android build do
not, and take the PNG path.

`scripts/platform_manifest.py#prune_assets` then drops the tier a fixed panel cannot
select, and drops the source PNGs only when every database entry has a render at the
kept size.

## Image specifications

- **Format:** PNG, RGBA or palette
- **Background:** transparent preferred
- **Content:** full printer view, centered

**Dimensions are not currently standardized.** The shipped set ranges from 169x180 to
2507x1885, and only 6 of 76 are 750x930. New art should land near the low end: the
300px tier is the largest any panel asks for, so pixels beyond ~800px on the long edge
cost disk, RAM and decode time on every platform that falls back to the PNG, and buy
nothing on the ones that do not.

```bash
magick input.jpg -resize 800x800 -background none -gravity center output.png
```

## Adding new art

1. Source a product photo, alpha-cut the background
2. Resize per above, save here with a `vendor-model` filename
3. Point the `printer_database.json` entry's `image` field at it
4. `scripts/check_printer_images.py` to confirm it resolves

## Custom images

Users add their own without touching this directory:

1. Drop a PNG/JPEG/BMP/GIF into `config/custom_images/` in the install directory
2. Open the picker (Home Panel -> tap printer image -> Printer Manager -> tap image)
3. Custom images appear under the "Custom" section

**Limits:** 5 MB, 2048x2048 (`MAX_FILE_SIZE` / `MAX_IMAGE_DIMENSION` in
`src/system/printer_image_manager.cpp`). On first picker open,
`#PrinterImageManager::auto_import_raw_images` converts each raw file to `-300.bin`
and `-150.bin` beside it, so custom art skips the PNG decode that shipped art hits.

Selection is stored as `"display.printer_image": "custom:<filename>"` (no extension);
shipped art uses `"shipped:<name>"`, and an unset value means auto-detect from the
detected printer type.
