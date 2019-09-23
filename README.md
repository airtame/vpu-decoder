## Credits
This work is heavily based upon libimxvpuapi (https://github.com/Freescale/libimxvpuapi) version 0.10.3. While there is little that remains from original code, we learned a great deal about how to "talk to the VPU" from libimxvpuapi sources. Note that this knowledge is not available elsewhere (for example VPU documentation we had access to ("i.MX 6Dual/6Quad VPU Application Programming Interface Linux Reference Manual") has nothing to say about many subjects, such as wrapping VP8 frames into IVF, or hack that lets one decode JPEGs on VPU). And so our work is very much "derived work". Hats off to original authors!

### Please note:
We were using libimxvpuapi version 0.10.3. Current version is 2.0 and appears to be very much rewritten. Everything that we have to say here is with respect to 0.10.3. You should consider using latest version of libimxvpuapi.

## Why yet another VPU wrapper library
Some time ago (around January 2018) we tried to use libimxvpuapi 0.10.3 on i.mx6 to decode and
display Full HD, bidirectionally compressed (with B-frames and reordering) h264 streams
(think YouTube videos). And we ran into several problems:
1) Decoding took waaaay too much DMA memory. In fact decoding of some streams used up all DMA memory available, and that was that
2) For some streams, there was a problem with decoding, usually revolving around SPS/PPS not being available (already consumed before encoder reopening), hangs
3) For some streams, we got nondeterministic output (like, decoding of the same short clip could give us anything between 53 and 61 output frames)
4) Reordered frames had wrong timestamps and other metadata
5) Small problems here and there, like unreliable h264 cropping information

Now, not all of the problems described above were libimxvpuapi fault. For example:
- h264 is very complex format, and one should use some kind of stream parser on top of low-level decoder like libimxvpuapi. Indeed, comments in imxvpuapi.h seem to suggest it: "if the environment is a framework like GStreamer or libav/FFmpeg, it is likely this will never have to be done, since these have their own parsers that detect parameter changes and initiate reinitializations."
- VPU decoder is decent, but has some quirks, for example it won't behave correctly if you don't supply whole, separate h264 NALs at a time into bitstream buffer, it has performance problems handling "multislice" h264 pictures, on some streams it doesn't decode cropping information from SPS correctly, and so on
- our particular i.mx6 was a low memory one
- etc

Still, what we wanted was compact, efficient and reliable solution for h264 (and vp8, but that is much simpler) playback on i.mx6 hardware. At first we considered just changing
libimxvpuapi, but in the end decided against it. This was because:
- libimxvpuapi supports very wide array of encoders, decoders, formats, features, options, even VPU SDKs (both vpu and fsl) making just fully understanding it a major effort. And we wanted to have something working quick.
- libimxvpuapi is intended to be complete solution for VPU, but not complete solution for playing back video files. As mentioned above, it lacks h264 stream parser, it had no provisions for displaying images, and so on.

One might say that where libimxvpuapi was very wide and not very tall library, we wanted to have something not very wide but tall :-)

## vpu-decoder
We started by cutting and pasting the parts of libimxvpuapi responsible for h264 decoding. Then, very simple and very crude decoder was put together, just to make sure that one can indeed run VPU with that small amount of code. When it started to work, we put simple h264 parser on top, and started iterating. Along the way, VP8 support was added, as well as some preliminary and experimental JPEG code. We also wrote simple playback app using just Linux framebuffer and i.mx6 g2d blitter chip. But first and foremost, we made sure that libimxvpuapi h264 limitations weren't there, so:
1) Decoder was made to use as little memory as possible. In some cases - and with some streams - it means half as much or even less. In practice, we found out that we can reliably decode two _different_ h264 streams _at the same time_ where libimxvpuapi couldn't decode even single one of them.
2) We can decode a lot of streams that libimxvpuapi couldn't (at least on our HW)
3) We know of no nondeterministic behavior now. 
4) Metadata gets preserved when h264 frames get reordered
5) We have our own SPS parsing code, so cropping information is reliable, even when decoding enhanced streams, such as the ones encoded with so called h264 Fidelity Range Extensions.

## Known limitations
1) Decoding only
2) Support for h264 or vp8 streams. It doesn't handle interlaced images at all (come on, it is 2019 already). There is experimental support for JPEG, but...don't use it yet, just don't.
3) Slow decoding of multislice (several NALs per single image) h264 streams. This is basically VPU bug/limitation we know how to work around, but that work is not finished yet.
4) h264 standard is huge, and there were several change/amendments after 2003 release. We don't know much about these. Streams using newer features _may_ work, but...consider yourself warned.
5) Other than the comments in the code, there is no documentation.
6) Finally, this is still in the early stage of the development, and we cannot provide any support. Please consider yourself warned.

## How to build and test
`scripts/build.sh` should build the library and `vpu_playback` tool
One can use `vpu_playback` to play back raw "Annex B" h264 and IVF-wrapped VP8 streams, like that:
`vpu_playback framebuffer stream0[@offset0] [stream1[@offset1]]...`
So for example:
`vpu_playback /dev/fb0 annex_b.h264` will play back `annex_b.h264` on `/dev/fb0`
`vpu_playback /dev/fb0 annex_b.h264@400000` will do the same, but starting from offset `400000`
`vpu_playback /dev/fb0 annex_b.h264 vp8.ivf` will try to play back two streams at once
and so on.

## Authors

Michal Adamczak (michal@airtame.com) - programming, testing, bugfixes
Pierre Fourgeaud (https://github.com/pierrefourgeaud) - code reviews, testing, bugfixes
Razvan Lar - code reviews, code structure suggestions, testing, bugfixes
Vasile Popescu - h264 bitstream and SPS parsing code
