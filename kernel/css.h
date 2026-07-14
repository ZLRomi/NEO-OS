#ifndef CSS_H
#define CSS_H

#include <stdint.h>

#define CSS_MAX_RULES      64
#define CSS_MAX_PROPS      8
#define CSS_MAX_SEL_PARTS  4
#define CSS_MAX_NODES      256
#define CSS_MAX_CHILDREN   24
#define CSS_MAX_CLASSES    4
#define CSS_NAME_LEN       24
#define CSS_TAG_LEN        16
#define CSS_PROP_LEN       16
#define CSS_VAL_LEN        32
#define CSS_TEXT_LEN       96
#define CSS_ID_LEN         24
#define CSS_SRC_LEN        8192
#define CSS_RENDER_BYTES   32768
#define CSS_HREF_LEN       128

typedef enum {
    CSS_SEL_TAG = 0,
    CSS_SEL_CLASS = 1,
    CSS_SEL_ID = 2,
    CSS_SEL_UNIVERSAL = 3,
} css_sel_kind_t;

typedef struct {
    css_sel_kind_t kind;
    char name[CSS_NAME_LEN];
} css_sel_part_t;

typedef struct {
    css_sel_part_t parts[CSS_MAX_SEL_PARTS];
    int part_count;
    int specificity;
} css_selector_t;

typedef struct {
    char name[CSS_PROP_LEN];
    char value[CSS_VAL_LEN];
} css_property_t;

typedef struct {
    css_selector_t selector;
    css_property_t props[CSS_MAX_PROPS];
    int prop_count;
} css_rule_t;

typedef struct {
    css_rule_t rules[CSS_MAX_RULES];
    int rule_count;
} css_stylesheet_t;

typedef enum {
    DOM_ELEMENT = 0,
    DOM_TEXT = 1,
} dom_node_kind_t;

typedef struct {
    dom_node_kind_t kind;
    int parent;
    int children[CSS_MAX_CHILDREN];
    int child_count;
    int next_sibling;

    char tag[CSS_TAG_LEN];
    char id[CSS_ID_LEN];
    char classes[CSS_MAX_CLASSES][CSS_NAME_LEN];
    int class_count;
    char text[CSS_TEXT_LEN];
    int is_block;
    int display_inline;

    uint32_t color;
    uint32_t bg_color;
    int font_size;
    int font_bold;
    int font_italic;
    int underline;

    int margin[4];
    int padding[4];
    int border_w[4];
    uint32_t border_color;

    int x, y, w, h;
    int content_x, content_y, content_w, content_h;
    int laid_out;

    int link_index;
    char href[CSS_HREF_LEN];
} dom_node_t;

typedef struct {
    dom_node_t nodes[CSS_MAX_NODES];
    int node_count;
    int root;
    css_stylesheet_t sheet;
    char render_buf[CSS_RENDER_BYTES];
    int render_len;
    int links_resolved;
    int link_count;
    int active;
} css_engine_t;

void css_engine_init(css_engine_t* eng);
int css_parse_stylesheet(css_stylesheet_t* sheet, const char* src);
int css_parse_inline_style(dom_node_t* node, const char* style_str);

int dom_parse_html(css_engine_t* eng, const char* html);
void css_apply_styles(css_engine_t* eng);
void css_layout(css_engine_t* eng, int x, int y, int w, int h);
int css_render_to_buffer(css_engine_t* eng);
uint32_t css_parse_color(const char* s);
int css_is_block_tag(const char* tag);

#endif
