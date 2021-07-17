# CCD - Camcorder Color Denoise

CCD is a simple chroma denoiser. It works by selectively averaging pixels in a 25x25 matrix below
the Euclidean distance threshold in an RGB clip. After denoising, the clip should be converted back
to YUV / YCoCg, and the luma channel should be copied from the input.

Currently, CCD only supports RGBS input and will not copy the original luma for you, so for
convinience, a python wrapper is included which can handle this.

Vapoursynth port of the original filter by [Sergey Stolyarevsky](https://web.archive.org/web/20210116182527/http://acobw.narod.ru/).

## Usage

Plugin - probably shouldn't be used directly!
```
ccd.CCD(clip clip, float threshold=4)
```
Python wrapper
```py
import ccd
ccd.ccd(clip: vs.VideoNode, threshold: float = 4, matrix: Optional[str] = None)
```
_Parameters_

- clip: Input clip. Plugin only supports RGBS, wrapper accepts any format except Gray and Compat.

- threshold: Euclidean distance threshold for including pixel in the matrix. Higher values = more denoising. A good range seems to be 4-10.

- matrix: Colour matrix for the wrapper to use for conversions to and from YUV/RGB. Will be guessed by the wrapper if left unspecified from frame props or frame size. Values are the same as Vapoursynth's [resize](http://www.vapoursynth.com/doc/functions/resize.html).


## How to install

If you're on Windows - congratulations! Just download the binary and wrapper from the
[releases page](https://github.com/End-of-Eternity/vs-ccd/releases), and drop them into their
usual places.

If you're a linux weirdo, then see the compilation instructions for the plugin below.

## Compilation

```sh
meson build
ninja -C build
```

Or you can use cmake - though I don't know how it works, and Scrad added it. Blame him if it fails, not me.

## Dependencies

Vapoursynth, obviously. That's it though :pogchamp:
