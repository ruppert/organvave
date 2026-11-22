/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Guilherme Ruppert (OrganVave) */
/* The screenshots of the site and the README: every page of the organ's screen as the firmware draws it (oa_draw into
 * host_screen, the same frames the device shows), written as DIR/<name>.ppm (240 x 240). Each page is set as the host
 * test sets it (oa_page, oa_gl and the pop-ups by hand); the firmware does the drawing.
 *   cc -w -Ibuild/gen -Ifirmware/src -Itests -o /tmp/organ_screens tools/organ_screens.c -lm && /tmp/organ_screens DIR */
#define CHECK_TAG "organ screens"
#include "editor_host.h"

static const char *dir;

static void shot(const char *name)
{
    char path[256];
    FILE *f;
    uint32_t i;
    ui.force = 1;
    oa_draw();
    snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
    f = fopen(path, "wb");
    if (!f)
        return;
    fprintf(f, "P6\n240 240\n255\n");
    for (i = 0; i < 240u * 240u; i++) {
        uint16_t c = (uint16_t)((host_screen[i] >> 8) | (host_screen[i] << 8));
        unsigned char px[3] = {(unsigned char)(((c >> 11) & 31) * 255 / 31), (unsigned char)(((c >> 5) & 63) * 255 / 63),
                               (unsigned char)((c & 31) * 255 / 31)};
        fwrite(px, 1, 3, f);
    }
    fclose(f);
}

static void page(uint32_t p, const char *name)            /* a page of HOME's list: 0 HOME, 1 DRAWBARS, 2 PERC & VIB ... */
{
    oa_gl = 0;
    oa_sv = 0;
    oa_pp_until = 0;
    oa_vp_until = 0;
    oa_page = p;
    shot(name);
}

static void glo(uint32_t g, const char *name)             /* a GLOBAL screen (GLO's list and its sub-screens) */
{
    oa_sv = 0;
    oa_page = 0;
    oa_gl = g;
    shot(name);
}

int main(int argc, char **argv)
{
    dir = argc > 1 ? argv[1] : ".";
    reset();
    panel.magic = 0;
    panel_init();                                         /* (main.c: the panel's ids as the boot leaves them) */
    oa_init();

    page(0, "home");
    page(1, "drawbars");
    page(2, "perc_vib");
    page(3, "rotary");
    page(4, "drive");
    page(5, "reverb");
    page(6, "delay");
    page(7, "eq");
    page(8, "preset_settings");
    page(9, "lower_pedal");

    oa_page = 0;
    oa_gl = 0;
    oa_sv = 0;
    oa_sv_open();
    shot("save");
    oa_sv = 0;

    oa_pp_until = fm1_ms + 100000u;
    shot("preset_popup");
    oa_pp_until = 0;

    glo(1, "global");
    glo(6, "midi_settings");
    glo(2, "midi_cc");
    glo(7, "midi_monitor");
    glo(3, "about");
    glo(8, "benchmark");
    glo(5, "support");
    glo(4, "factory_reset");
    oa_gl = 0;
    return 0;
}
