# Banner sources

`banner.html` (1280x400) and `social.html` (1280x640) are the sources of `../banner.png` and
`../social-preview.png`. Original artwork; fonts are Anton and Barlow Semi Condensed from Google Fonts.

Re-render with a headless Chromium-based browser at 1x, for example Microsoft Edge:

```powershell
msedge --headless=new --hide-scrollbars --force-device-scale-factor=1 --window-size=1280,400 --virtual-time-budget=10000 --screenshot="$PWD\banner.png" "file:///$PWD/banner.html"
msedge --headless=new --hide-scrollbars --force-device-scale-factor=1 --window-size=1280,640 --virtual-time-budget=10000 --screenshot="$PWD\social-preview.png" "file:///$PWD/social.html"
```
