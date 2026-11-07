#pragma once

// Unpacks the graphic packs compiled into the core (tools/pack_graphic_packs.c)
// into graphicPacks, replacing what is there, when the set there is not the one
// this build carries.
void LibretroGraphicPacks_InstallBundled();
