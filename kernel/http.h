#ifndef HTTP_H
#define HTTP_H

#include <stdint.h>

int http_fetch(const char* url, char* raw_out, int raw_max,
               char* text_out, int text_max);
int http_get(const char* host, const char* path, char* out, int max_len, int text_only);
int http_get_html(const char* host, const char* path, char* raw_out, int raw_max,
                  char* text_out, int text_max);
void http_html_to_text(const char* html, char* out, int max);
void http_resolve_url(const char* cur_host, const char* cur_path,
                      const char* loc, char* out, int out_max);
void http_resolve_url_full(const char* cur_url, const char* loc, char* out, int out_max);

int http_download_file(const char* url, const char* save_path,
                       int (*progress_cb)(int received, int total, void* ctx),
                       void* ctx);

#endif
