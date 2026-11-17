# Third-party notes: shaders

## crt.glsl

`crt.glsl` was written for this project. Its approach is modelled on **crt-lottes** by
Timothy Lottes (libretro `glsl-shaders/crt/shaders/crt-lottes.glsl` and
`crt-lottes-fast`), which its author placed in the **public domain**:

- a Gaussian beam profile per source scanline, evaluated at output-pixel centres;
- a short horizontal Gaussian (here about 0.5 source px);
- shading in approximately linear light;
- an optional aperture-grille mask with brightness compensation.

No code was copied from crt-lottes, and no GPL shader (zfast_crt, crt-easymode,
crt-geom, crt-pi and others) was used as a source. Attribution is given as a courtesy;
public-domain code carries no licence obligations.
