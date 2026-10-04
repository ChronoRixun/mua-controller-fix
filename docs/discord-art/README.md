# Discord Rich Presence art

Artwork for the two Discord applications that MUA Controller Fix uses for Rich Presence:
**Marvel Ultimate Alliance** (MUA1) and **Marvel Ultimate Alliance 2** (MUA2). It belongs to the
same family as the XML2 Fix set (a slab numeral with an extruded shadow, comic halftone dots and a
diagonal corner), drawn in this repo's banner palette. MUA1 is a red **I** with an ink shadow; MUA2
is an ink **II** with a red shadow.

## What to upload

Upload each game's files to its own application in the Discord Developer Portal.

| File | Pixels | Where it goes |
|---|---|---|
| `upload/mua1/app-icon.png` | 1024 x 1024 | MUA1 app: General Information, App Icon |
| `upload/mua1/logo.png` | 1024 x 1024 | MUA1 app: Rich Presence, Art Assets, add image with the asset key `logo` (the large image) |
| `upload/mua2/app-icon.png` | 1024 x 1024 | MUA2 app: General Information, App Icon |
| `upload/mua2/logo.png` | 1024 x 1024 | MUA2 app: Rich Presence, Art Assets, add image with the asset key `logo` (the large image) |

`app-icon.png` and `logo.png` are the same image. Discord names an asset after its file name, so
`logo.png` arrives with the key `logo`; make sure the key reads exactly `logo` before saving.

## Sources

| Source | Renders to |
|---|---|
| `src/mua1-icon.svg` | `upload/mua1/app-icon.png`, `upload/mua1/logo.png` |
| `src/mua2-icon.svg` | `upload/mua2/app-icon.png`, `upload/mua2/logo.png` |

The SVGs are self-contained and need no fonts installed. To re-render at 1x with Microsoft Edge,
run this from this folder in PowerShell:

```powershell
$edge = "${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe"
$flags = '--headless=new', '--hide-scrollbars', '--force-device-scale-factor=1'
foreach ($g in 'mua1', 'mua2') {
  Start-Process -Wait $edge ($flags + '--window-size=1024,1024', "--screenshot=$PWD\upload\$g\logo.png", "file:///$PWD/src/$g-icon.svg")
  Copy-Item "upload\$g\logo.png" "upload\$g\app-icon.png"
}
```

## Originality

All artwork here is original and made for this project. It contains no Marvel, Activision, Disney
or Zoë Mode logos, no characters, costumes or emblems, no title treatments from the games, and no
key art or screenshots, and no words: I and II are the game numbers.

- Numerals: hand-built slab-serif polygons, not a font.
- Colours, from `docs/images/source/banner.html`: cream `#F1E7D3`, red `#C8372A`, ink `#1A181D`,
  halftone blue `#2D5B94`.
