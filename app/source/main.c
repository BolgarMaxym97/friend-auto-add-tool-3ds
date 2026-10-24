/*
 * Friend Auto Add Tool: adds every friend code of a shared base, leaving a provisional
 * friend ("заявка") where the other console has not added us yet. Plan and decisions:
 * the README; screens: app.c.
 */
#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>

#include "app.h"
#include "ui.h"

int main(void)
{
    romfsInit();
    acInit();
    ptmuInit();
    gfxInitDefault();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS * 4);
    C2D_Prepare();
    C3D_RenderTarget *top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    C3D_RenderTarget *bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);

    static App app;
    bool ready = ui_init();
    app_init(&app);

    bool touching = false;
    touchPosition last = {0, 0};
    while (ready && aptMainLoop() && !app.quit) {
        hidScanInput();
        Input in = {.down = hidKeysDown(), .repeat = hidKeysDownRepeat(), .held = hidKeysHeld()};
        if (in.held & KEY_TOUCH) {
            hidTouchRead(&last);
            touching = true;
        } else if (touching) {
            touching = false;
            in.tapped = true;
            in.tap_x = last.px;
            in.tap_y = last.py;
        }
        app_update(&app, &in);

        ui_frame_begin();
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        C2D_TargetClear(top, COL_BG);
        C2D_SceneBegin(top);
        app_draw_top(&app);
        C2D_TargetClear(bottom, COL_BG);
        C2D_SceneBegin(bottom);
        app_draw_bottom(&app);
        C3D_FrameEnd(0);
    }

    app_exit(&app);
    ui_exit();
    C2D_Fini();
    C3D_Fini();
    gfxExit();
    ptmuExit();
    acExit();
    romfsExit();
    return 0;
}
