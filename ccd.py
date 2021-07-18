from typing import Optional
import vapoursynth as vs

core = vs.core


def ccd(clip: vs.VideoNode, threshold: float = 4, matrix: Optional[str] = None) -> vs.VideoNode:
    if clip.format is None:
        raise ValueError("Variable format is not supported.")

    if None in [clip.width, clip.height]:
        raise ValueError("Variable resolutions are not supported.")

    cwidth = clip.width >> clip.format.subsampling_w
    cheight = clip.height >> clip.format.subsampling_h

    format = clip.format.replace(subsampling_w=0, subsampling_h=0)

    if matrix is None and format.color_family == vs.YUV:
        props = clip.get_frame(0).props
        if "_Matrix" in props.keys():
            matrix_prop = props["_Matrix"]
            if not isinstance(matrix_prop, int):
                raise ValueError("Bad matrix tag")
            matrix = {
                1: "709",
                2: "unspec",
                5: "470bg",
                6: "170m",
                7: "240m",
                9: "2020ncl",
                10: "2020cl",
            }.get(matrix_prop)
            if matrix is None:
                raise ValueError("Unrecognised _Matrix frame prop, please specify matrix manually.")
    elif format.color_family == vs.YCOCG:
        matrix = "ycocg"
    elif format.color_family in [vs.GRAY or vs.COMPAT]:
        raise ValueError("Only RGB, YUV and YCoCg input is supported.")

    if matrix is None or matrix == "unspec":
        if clip.width >= 1280 or clip.height >= 720:
            matrix = "709"
        else:
            matrix = "170m"

    if format.color_family != vs.RGB:
        if clip.format.subsampling_h or clip.format.subsampling_w:
            luma = core.std.ShufflePlanes(clip, 0, vs.GRAY)
            # luma = core.resize.Bicubic(luma, cwidth, cheight)
            rgb_in = core.std.ShufflePlanes([luma, clip], [0, 1, 2], format.color_family)
        else:
            rgb_in = clip
        rgb = core.resize.Bicubic(rgb_in, format=vs.RGBS, matrix_in_s=matrix)
    elif format.sample_type != vs.FLOAT and format.bits_per_sample != 32:
        rgb = core.resize.Point(clip, format=vs.RGBS)
    else:
        rgb = clip

    denoised = core.ccd.CCD(rgb, threshold)

    if format.color_family != vs.RGB:
        denoised = core.resize.Bicubic(denoised, format=format, matrix_s=matrix)
        out = core.std.ShufflePlanes([clip, denoised], [0, 1, 2], format.color_family)
        out = core.resize.Bicubic(out, format=clip.format)
    else:
        denoised = core.resize.Point(denoised, format=format.replace(color_family=vs.YUV), matrix_s=matrix)
        yuv = core.resize.Point(clip, format=format.replace(color_family=vs.GRAY), matrix_s=matrix)
        shuffled = core.std.ShufflePlanes([yuv, denoised], [0, 1, 2], vs.YUV)
        out = core.resize.Point(shuffled, format=format, matrix_in_s=matrix)

    return out
