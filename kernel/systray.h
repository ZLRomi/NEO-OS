#ifndef SYSTRAY_H
#define SYSTRAY_H

#define SYSTRAY_VOLUME   1
#define SYSTRAY_NETWORK   2
#define SYSTRAY_CLOCK     3

#define SYSTRAY_ICON_COUNT 3

typedef struct {
    int type;
    int x;
    int y;
    int hover;
    int popup_open;
} systray_icon_t;

void systray_init(void);
void systray_render(int taskbar_x, int taskbar_y, int taskbar_w, int taskbar_h);
int  systray_handle_click(int mx, int my);
void systray_handle_hover(int mx, int my);

#endif
