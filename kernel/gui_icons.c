#include "gui_internal.h"
#include <stdint.h>

#include "generated/asset_terminal_32.h"
#include "generated/asset_about_32.h"
#include "generated/asset_calc_32.h"
#include "generated/asset_demo_32.h"
#include "generated/asset_terminal_16.h"
#include "generated/asset_about_16.h"
#include "generated/asset_calc_16.h"
#include "generated/asset_demo_16.h"
#include "generated/asset_user_32.h"
#include "generated/asset_user_16.h"
#include "generated/asset_power_16.h"
#include "generated/asset_folder_16.h"
#include "generated/asset_settings_16.h"
#include "generated/asset_weather_32.h"
#include "generated/asset_news_32.h"
#include "generated/asset_mail_32.h"
#include "generated/asset_photos_32.h"
#include "generated/asset_refresh_16.h"
#include "generated/asset_toolbar_16.h"
#include "generated/asset_cascade_16.h"
#include "generated/asset_showdesktop_16.h"
#include "generated/asset_browser_32.h"
#include "generated/asset_browser_16.h"

void draw_icon_terminal(int x, int y) {
    image_blit(x, y, asset_terminal_32, ASSET_TERMINAL_32_W, ASSET_TERMINAL_32_H);
}

void draw_icon_terminal_small(int x, int y) {
    image_blit(x, y, asset_terminal_16, ASSET_TERMINAL_16_W, ASSET_TERMINAL_16_H);
}

void draw_icon_about(int x, int y) {
    image_blit(x, y, asset_about_32, ASSET_ABOUT_32_W, ASSET_ABOUT_32_H);
}

void draw_icon_about_small(int x, int y) {
    image_blit(x, y, asset_about_16, ASSET_ABOUT_16_W, ASSET_ABOUT_16_H);
}

void draw_icon_calc(int x, int y) {
    image_blit(x, y, asset_calc_32, ASSET_CALC_32_W, ASSET_CALC_32_H);
}

void draw_icon_calc_small(int x, int y) {
    image_blit(x, y, asset_calc_16, ASSET_CALC_16_W, ASSET_CALC_16_H);
}

void draw_icon_demo(int x, int y) {
    image_blit(x, y, asset_demo_32, ASSET_DEMO_32_W, ASSET_DEMO_32_H);
}

void draw_icon_demo_small(int x, int y) {
    image_blit(x, y, asset_demo_16, ASSET_DEMO_16_W, ASSET_DEMO_16_H);
}

void draw_icon_user(int x, int y) {
    image_blit(x, y, asset_user_32, ASSET_USER_32_W, ASSET_USER_32_H);
}

void draw_icon_user_small(int x, int y) {
    image_blit(x, y, asset_user_16, ASSET_USER_16_W, ASSET_USER_16_H);
}

void draw_icon_power(int x, int y) {
    image_blit(x, y, asset_power_16, ASSET_POWER_16_W, ASSET_POWER_16_H);
}

void draw_icon_folder(int x, int y) {
    image_blit(x, y, asset_folder_16, ASSET_FOLDER_16_W, ASSET_FOLDER_16_H);
}

void draw_icon_folder_small(int x, int y) {
    image_blit(x, y, asset_folder_16, ASSET_FOLDER_16_W, ASSET_FOLDER_16_H);
}

void draw_icon_settings(int x, int y) {
    image_blit(x, y, asset_settings_16, ASSET_SETTINGS_16_W, ASSET_SETTINGS_16_H);
}

void draw_icon_weather(int x, int y) {
    image_blit(x, y, asset_weather_32, ASSET_WEATHER_32_W, ASSET_WEATHER_32_H);
}

void draw_icon_news(int x, int y) {
    image_blit(x, y, asset_news_32, ASSET_NEWS_32_W, ASSET_NEWS_32_H);
}

void draw_icon_mail(int x, int y) {
    image_blit(x, y, asset_mail_32, ASSET_MAIL_32_W, ASSET_MAIL_32_H);
}

void draw_icon_photos(int x, int y) {
    image_blit(x, y, asset_photos_32, ASSET_PHOTOS_32_W, ASSET_PHOTOS_32_H);
}

void draw_icon_refresh(int x, int y) {
    image_blit(x, y, asset_refresh_16, ASSET_REFRESH_16_W, ASSET_REFRESH_16_H);
}

void draw_icon_toolbar(int x, int y) {
    image_blit(x, y, asset_toolbar_16, ASSET_TOOLBAR_16_W, ASSET_TOOLBAR_16_H);
}

void draw_icon_cascade(int x, int y) {
    image_blit(x, y, asset_cascade_16, ASSET_CASCADE_16_W, ASSET_CASCADE_16_H);
}

void draw_icon_showdesktop(int x, int y) {
    image_blit(x, y, asset_showdesktop_16, ASSET_SHOWDESKTOP_16_W, ASSET_SHOWDESKTOP_16_H);
}

void draw_icon_browser(int x, int y) {
    image_blit(x, y, asset_browser_32, ASSET_BROWSER_32_W, ASSET_BROWSER_32_H);
}

void draw_icon_browser_small(int x, int y) {
    image_blit(x, y, asset_browser_16, ASSET_BROWSER_16_W, ASSET_BROWSER_16_H);
}
