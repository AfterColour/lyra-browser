<p align="center"><img src="art/lyra-icon-256.png" width="128" alt=""></p>

<h1 align="center">Lyra</h1>
<p align="center">A fast, quiet web browser for Windows, built on its own WebKit engine.<br>
Быстрый и спокойный браузер для Windows на собственном движке WebKit.</p>

<p align="center"><a href="https://github.com/AfterColour/lyra-browser/releases/latest/download/LyraSetup.exe"><b>Download for Windows</b></a>
 &nbsp;·&nbsp; <a href="https://github.com/AfterColour/lyra-browser/releases">All versions</a></p>

## What is inside

- Its own interface, drawn by Lyra itself: tabs next to the address, in their own row or down the
  side; split view; pinned tabs; a command palette (Ctrl+K); reader view; translation; picture in picture
- WebKit with GStreamer for video (YouTube and other streaming sites, hardware decoding through Direct3D 11)
- Spell checking in English and Russian, an ad blocker, a password manager, private tabs
- A small installer that needs no administrator rights

Windows 10 and 11, 64-bit.

## Support

If you like Lyra, you can support it on [GitHub Sponsors](https://github.com/sponsors/AfterColour) (other ways: [SUPPORT.md](SUPPORT.md)).

## Source and license

Lyra's own code is in this repository under the [PolyForm Shield License 1.0.0](LICENSE.md): you
may use it for any purpose, read it, change it and share it, but not to make a product that
competes with Lyra.

The WebKit engine with Lyra's changes is open source (LGPL / BSD) at
[AfterColour/lyra-webkit](https://github.com/AfterColour/lyra-webkit). Every release also carries
the source of the LGPL libraries it ships; see `THIRD_PARTY_NOTICES.txt` in the installed folder.

## Building

Lyra builds against the engine in `C:\WebKit\WebKit` (`build-webkit-gst.bat`), then:

```
build.bat                         Lyra into run\
python tools\make-installer.py    dist\LyraSetup.exe
release.bat                       publish a version on GitHub
```

Source layout: [src/README.md](src/README.md).
