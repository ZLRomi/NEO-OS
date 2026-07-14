#include "css.h"
#include "font_noto.h"
#include <stdint.h>

static int str_eq_ci(const char* a, const char* b) {
    int i = 0;
    while (a[i] && b[i]) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
        i++;
    }
    return a[i] == b[i];
}

static int str_len(const char* s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void str_copy(char* d, const char* s, int max) {
    int i = 0;
    while (s[i] && i < max - 1) { d[i] = s[i]; i++; }
    d[i] = '\0';
}

static int is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static int is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static int is_digit(char c) {
    return c >= '0' && c <= '9';
}

static int is_ident(char c) {
    return is_alpha(c) || is_digit(c) || c == '-' || c == '_';
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

uint32_t css_parse_color(const char* s) {
    if (!s || !*s) return 0;
    while (is_space(*s)) s++;
    if (*s == '#') {
        s++;
        int r = 0, g = 0, b = 0;
        int h0 = hex_val(s[0]), h1 = hex_val(s[1]), h2 = hex_val(s[2]), h3 = hex_val(s[3]), h4 = hex_val(s[4]), h5 = hex_val(s[5]);
        if (h0 >= 0 && h1 >= 0 && h2 >= 0 && h3 >= 0 && h4 >= 0 && h5 >= 0) {
            r = (h0 << 4) | h1; g = (h2 << 4) | h3; b = (h4 << 4) | h5;
            return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        }
        int only0 = hex_val(s[0]), only1 = hex_val(s[1]), only2 = hex_val(s[2]);
        if (only0 >= 0 && only1 >= 0 && only2 >= 0) {
            r = (only0 << 4) | only0; g = (only1 << 4) | only1; b = (only2 << 4) | only2;
            return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        }
        return 0;
    }
    if (s[0] == 'r' && s[1] == 'g' && s[2] == 'b' && s[3] == '(') {
        int v[3] = {0, 0, 0};
        int idx = 0;
        int i = 4;
        while (s[i] && s[i] != ')' && idx < 3) {
            while (is_space(s[i]) || s[i] == ',') i++;
            int n = 0;
            while (is_digit(s[i])) { n = n * 10 + (s[i] - '0'); i++; }
            v[idx++] = n;
        }
        return ((uint32_t)(v[0] & 0xFF) << 16) | ((uint32_t)(v[1] & 0xFF) << 8) | (uint32_t)(v[2] & 0xFF);
    }
    if (str_eq_ci(s, "black"))   return 0x000000;
    if (str_eq_ci(s, "white"))   return 0xFFFFFF;
    if (str_eq_ci(s, "red"))     return 0xFF0000;
    if (str_eq_ci(s, "green"))   return 0x008000;
    if (str_eq_ci(s, "blue"))    return 0x0000FF;
    if (str_eq_ci(s, "yellow"))  return 0xFFFF00;
    if (str_eq_ci(s, "cyan"))    return 0x00FFFF;
    if (str_eq_ci(s, "magenta")) return 0xFF00FF;
    if (str_eq_ci(s, "gray") || str_eq_ci(s, "grey")) return 0x808080;
    if (str_eq_ci(s, "silver"))  return 0xC0C0C0;
    if (str_eq_ci(s, "maroon"))  return 0x800000;
    if (str_eq_ci(s, "olive"))   return 0x808000;
    if (str_eq_ci(s, "lime"))    return 0x00FF00;
    if (str_eq_ci(s, "aqua"))    return 0x00FFFF;
    if (str_eq_ci(s, "teal"))    return 0x008080;
    if (str_eq_ci(s, "navy"))    return 0x000080;
    if (str_eq_ci(s, "purple"))  return 0x800080;
    if (str_eq_ci(s, "orange"))  return 0xFFA500;
    if (str_eq_ci(s, "pink"))    return 0xFFC0CB;
    if (str_eq_ci(s, "brown"))   return 0xA52A2A;
    return 0;
}

static int parse_length(const char* s, int default_val) {
    if (!s || !*s) return default_val;
    while (is_space(*s)) s++;
    int n = 0;
    int got = 0;
    while (is_digit(*s)) { n = n * 10 + (*s - '0'); s++; got = 1; }
    if (!got) return default_val;
    while (is_space(*s) || *s == 'p' || *s == 'x' || *s == 'e' || *s == 'm' || *s == '%' || *s == 'r' || *s == 'e' || *s == 'm') {
        if (*s == '\0') break;
        s++;
    }
    if (n > 200) n = 200;
    return n;
}

int css_is_block_tag(const char* tag) {
    static const char* block_tags[] = {
        "div","p","h1","h2","h3","h4","h5","h6","ul","ol","li","table","tr","td","th",
        "header","footer","nav","section","main","article","aside","figure","figcaption",
        "form","fieldset","blockquote","pre","address","body","html","hr","details",0
    };
    for (int i = 0; block_tags[i]; i++) {
        if (str_eq_ci(tag, block_tags[i])) return 1;
    }
    return 0;
}

void css_engine_init(css_engine_t* eng) {
    if (!eng) return;
    for (int i = 0; i < CSS_MAX_NODES; i++) {
        eng->nodes[i].kind = DOM_ELEMENT;
        eng->nodes[i].parent = -1;
        eng->nodes[i].child_count = 0;
        eng->nodes[i].next_sibling = -1;
        eng->nodes[i].tag[0] = '\0';
        eng->nodes[i].id[0] = '\0';
        eng->nodes[i].class_count = 0;
        eng->nodes[i].text[0] = '\0';
        eng->nodes[i].is_block = 0;
        eng->nodes[i].display_inline = 0;
        eng->nodes[i].color = 0xFF000000;
        eng->nodes[i].bg_color = 0;
        eng->nodes[i].font_size = 16;
        eng->nodes[i].font_bold = 0;
        eng->nodes[i].font_italic = 0;
        eng->nodes[i].underline = 0;
        for (int k = 0; k < 4; k++) {
            eng->nodes[i].margin[k] = 0;
            eng->nodes[i].padding[k] = 0;
            eng->nodes[i].border_w[k] = 0;
        }
        eng->nodes[i].border_color = 0;
        eng->nodes[i].x = 0; eng->nodes[i].y = 0;
        eng->nodes[i].w = 0; eng->nodes[i].h = 0;
        eng->nodes[i].content_x = 0; eng->nodes[i].content_y = 0;
        eng->nodes[i].content_w = 0; eng->nodes[i].content_h = 0;
        eng->nodes[i].laid_out = 0;
        eng->nodes[i].link_index = -1;
        eng->nodes[i].href[0] = '\0';
    }
    eng->node_count = 0;
    eng->root = -1;
    eng->render_len = 0;
    eng->render_buf[0] = '\0';
    eng->links_resolved = 0;
    eng->link_count = 0;
    eng->active = 0;
    eng->sheet.rule_count = 0;
}

static int alloc_node(css_engine_t* eng) {
    if (eng->node_count >= CSS_MAX_NODES) return -1;
    int idx = eng->node_count++;
    dom_node_t* n = &eng->nodes[idx];
    n->parent = -1;
    n->child_count = 0;
    n->next_sibling = -1;
    n->kind = DOM_ELEMENT;
    n->tag[0] = '\0';
    n->id[0] = '\0';
    n->class_count = 0;
    n->text[0] = '\0';
    n->is_block = 1;
    n->display_inline = 0;
    n->color = 0xFF000000;
    n->bg_color = 0;
    n->font_size = 16;
    n->font_bold = 0;
    n->font_italic = 0;
    n->underline = 0;
    for (int k = 0; k < 4; k++) {
        n->margin[k] = 0;
        n->padding[k] = 0;
        n->border_w[k] = 0;
    }
    n->border_color = 0x00888888;
    n->x = 0; n->y = 0;
    n->w = 0; n->h = 0;
    n->content_x = 0; n->content_y = 0;
    n->content_w = 0; n->content_h = 0;
    n->laid_out = 0;
    n->link_index = -1;
    n->href[0] = '\0';
    return idx;
}

static void parse_selector(const char* src, css_selector_t* out) {
    out->part_count = 0;
    out->specificity = 0;
    int i = 0;
    while (src[i] && out->part_count < CSS_MAX_SEL_PARTS) {
        while (is_space(src[i])) i++;
        if (!src[i]) break;
        css_sel_part_t part;
        part.kind = CSS_SEL_TAG;
        part.name[0] = '\0';
        int ni = 0;
        if (src[i] == '.') {
            part.kind = CSS_SEL_CLASS;
            i++;
            while (is_ident(src[i]) && ni < CSS_NAME_LEN - 1) part.name[ni++] = src[i++];
            part.name[ni] = '\0';
            out->specificity += 10;
        } else if (src[i] == '#') {
            part.kind = CSS_SEL_ID;
            i++;
            while (is_ident(src[i]) && ni < CSS_NAME_LEN - 1) part.name[ni++] = src[i++];
            part.name[ni] = '\0';
            out->specificity += 100;
        } else if (src[i] == '*') {
            part.kind = CSS_SEL_UNIVERSAL;
            part.name[0] = '\0';
            i++;
            out->specificity += 0;
        } else {
            part.kind = CSS_SEL_TAG;
            while (is_ident(src[i]) && ni < CSS_NAME_LEN - 1) part.name[ni++] = src[i++];
            part.name[ni] = '\0';
            if (ni == 0) {
                while (src[i] && !is_space(src[i]) && src[i] != ',') i++;
                continue;
            }
            out->specificity += 1;
        }
        out->parts[out->part_count++] = part;
    }
}

int css_parse_stylesheet(css_stylesheet_t* sheet, const char* src) {
    sheet->rule_count = 0;
    if (!src) return 0;
    int i = 0;
    int parsed = 0;
    while (src[i] && sheet->rule_count < CSS_MAX_RULES) {
        while (is_space(src[i]) || src[i] == '<' || src[i] == '!' || src[i] == '-' || src[i] == '-') i++;
        if (!src[i]) break;
        if (src[i] == '/' && src[i+1] == '*') {
            i += 2;
            while (src[i] && !(src[i] == '*' && src[i+1] == '/')) i++;
            if (src[i]) i += 2;
            continue;
        }
        if (src[i] == '@') {
            while (src[i] && src[i] != ';' && src[i] != '{') i++;
            if (src[i] == '{') {
                int depth = 1; i++;
                while (src[i] && depth > 0) {
                    if (src[i] == '{') depth++;
                    else if (src[i] == '}') depth--;
                    if (depth > 0) i++;
                }
                if (src[i] == '}') i++;
            } else if (src[i] == ';') i++;
            continue;
        }
        char sel_buf[128];
        int si = 0;
        while (src[i] && src[i] != '{' && src[i] != '}' && si < 127) {
            sel_buf[si++] = src[i++];
        }
        sel_buf[si] = '\0';
        if (src[i] != '{') {
            if (src[i] == '}') { i++; continue; }
            break;
        }
        i++;
        char decl_buf[512];
        int di = 0;
        while (src[i] && src[i] != '}' && di < 511) {
            decl_buf[di++] = src[i++];
        }
        decl_buf[di] = '\0';
        if (src[i] == '}') i++;

        char* p = sel_buf;
        while (*p) {
            while (is_space(*p)) p++;
            if (!*p) break;
            char* comma = p;
            while (*comma && *comma != ',') comma++;
            int next_start = 0;
            if (*comma == ',') { *comma = '\0'; next_start = (int)((comma + 1) - sel_buf); }
            if (sheet->rule_count >= CSS_MAX_RULES) break;
            css_rule_t* rule = &sheet->rules[sheet->rule_count];
            parse_selector(p, &rule->selector);
            rule->prop_count = 0;
            int dj = 0;
            while (decl_buf[dj] && rule->prop_count < CSS_MAX_PROPS) {
                while (decl_buf[dj] && (is_space(decl_buf[dj]) || decl_buf[dj] == ';')) dj++;
                if (!decl_buf[dj]) break;
                char name[CSS_PROP_LEN];
                int ni2 = 0;
                while (decl_buf[dj] && decl_buf[dj] != ':' && ni2 < CSS_PROP_LEN - 1) {
                    if (!is_space(decl_buf[dj])) name[ni2++] = decl_buf[dj];
                    dj++;
                }
                name[ni2] = '\0';
                if (decl_buf[dj] != ':') break;
                dj++;
                char value[CSS_VAL_LEN];
                int vi = 0;
                while (decl_buf[dj] && decl_buf[dj] != ';' && vi < CSS_VAL_LEN - 1) {
                    if (!is_space(decl_buf[dj]) && decl_buf[dj] != '\n' && decl_buf[dj] != '\r') {
                        value[vi++] = decl_buf[dj];
                    }
                    dj++;
                }
                value[vi] = '\0';
                if (ni2 > 0 && vi > 0) {
                    str_copy(rule->props[rule->prop_count].name, name, CSS_PROP_LEN);
                    str_copy(rule->props[rule->prop_count].value, value, CSS_VAL_LEN);
                    rule->prop_count++;
                }
                if (decl_buf[dj] == ';') dj++;
            }
            if (rule->prop_count > 0 && rule->selector.part_count > 0) {
                sheet->rule_count++;
                parsed++;
            }
            if (next_start > 0) p = sel_buf + next_start;
            else break;
        }
    }
    return parsed;
}

static void apply_property(dom_node_t* node, const char* name, const char* value) {
    if (str_eq_ci(name, "color")) {
        node->color = css_parse_color(value);
    } else if (str_eq_ci(name, "background-color") || str_eq_ci(name, "background")) {
        node->bg_color = css_parse_color(value);
    } else if (str_eq_ci(name, "font-size")) {
        node->font_size = parse_length(value, 16);
        if (node->font_size < 8) node->font_size = 8;
        if (node->font_size > 32) node->font_size = 32;
    } else if (str_eq_ci(name, "font-weight")) {
        if (str_eq_ci(value, "bold") || str_eq_ci(value, "700") || str_eq_ci(value, "bolder")) {
            node->font_bold = 1;
        } else {
            node->font_bold = 0;
        }
    } else if (str_eq_ci(name, "font-style")) {
        node->font_italic = str_eq_ci(value, "italic") || str_eq_ci(value, "oblique");
    } else if (str_eq_ci(name, "text-decoration")) {
        node->underline = (value[0] == 'u');
    } else if (str_eq_ci(name, "margin")) {
        int v = parse_length(value, 0);
        for (int k = 0; k < 4; k++) node->margin[k] = v;
    } else if (str_eq_ci(name, "margin-top")) node->margin[0] = parse_length(value, 0);
    else if (str_eq_ci(name, "margin-right")) node->margin[1] = parse_length(value, 0);
    else if (str_eq_ci(name, "margin-bottom")) node->margin[2] = parse_length(value, 0);
    else if (str_eq_ci(name, "margin-left")) node->margin[3] = parse_length(value, 0);
    else if (str_eq_ci(name, "padding")) {
        int v = parse_length(value, 0);
        for (int k = 0; k < 4; k++) node->padding[k] = v;
    }
    else if (str_eq_ci(name, "padding-top")) node->padding[0] = parse_length(value, 0);
    else if (str_eq_ci(name, "padding-right")) node->padding[1] = parse_length(value, 0);
    else if (str_eq_ci(name, "padding-bottom")) node->padding[2] = parse_length(value, 0);
    else if (str_eq_ci(name, "padding-left")) node->padding[3] = parse_length(value, 0);
    else if (str_eq_ci(name, "border-width") || str_eq_ci(name, "border")) {
        int v = parse_length(value, 0);
        for (int k = 0; k < 4; k++) node->border_w[k] = v;
    }
    else if (str_eq_ci(name, "border-color")) {
        node->border_color = css_parse_color(value);
    }
    else if (str_eq_ci(name, "display")) {
        if (str_eq_ci(value, "inline")) { node->display_inline = 1; node->is_block = 0; }
        else if (str_eq_ci(value, "block")) { node->display_inline = 0; node->is_block = 1; }
    }
}

int css_parse_inline_style(dom_node_t* node, const char* style_str) {
    if (!node || !style_str) return 0;
    int i = 0;
    int applied = 0;
    while (style_str[i]) {
        char name[CSS_PROP_LEN];
        int ni = 0;
        while (style_str[i] && style_str[i] != ':' && !is_space(style_str[i]) && ni < CSS_PROP_LEN - 1) {
            name[ni++] = style_str[i++];
        }
        name[ni] = '\0';
        while (style_str[i] && (is_space(style_str[i]) || style_str[i] == ':')) i++;
        char value[CSS_VAL_LEN];
        int vi = 0;
        while (style_str[i] && style_str[i] != ';' && vi < CSS_VAL_LEN - 1) {
            if (!is_space(style_str[i])) value[vi++] = style_str[i];
            i++;
        }
        value[vi] = '\0';
        if (ni > 0 && vi > 0) {
            apply_property(node, name, value);
            applied++;
        }
        while (style_str[i] == ';' || is_space(style_str[i])) i++;
    }
    return applied;
}

static int match_simple(const dom_node_t* node, const css_sel_part_t* part) {
    if (part->kind == CSS_SEL_UNIVERSAL) return 1;
    if (node->kind != DOM_ELEMENT) return 0;
    if (part->kind == CSS_SEL_TAG) {
        return str_eq_ci(node->tag, part->name);
    }
    if (part->kind == CSS_SEL_CLASS) {
        for (int i = 0; i < node->class_count; i++) {
            if (str_eq_ci(node->classes[i], part->name)) return 1;
        }
        return 0;
    }
    if (part->kind == CSS_SEL_ID) {
        return str_eq_ci(node->id, part->name);
    }
    return 0;
}

static int match_selector(const css_engine_t* eng, int node_idx, const css_selector_t* sel) {
    if (node_idx < 0 || node_idx >= eng->node_count) return 0;
    const dom_node_t* node = &eng->nodes[node_idx];
    int part_idx = sel->part_count - 1;
    if (part_idx < 0) return 0;
    if (!match_simple(node, &sel->parts[part_idx])) return 0;
    part_idx--;
    int cur = node->parent;
    while (part_idx >= 0 && cur >= 0) {
        if (match_simple(&eng->nodes[cur], &sel->parts[part_idx])) {
            part_idx--;
            cur = eng->nodes[cur].parent;
        } else {
            cur = eng->nodes[cur].parent;
        }
    }
    return part_idx < 0;
}

void css_apply_styles(css_engine_t* eng) {
    if (!eng) return;
    for (int i = 0; i < eng->node_count; i++) {
        dom_node_t* node = &eng->nodes[i];
        if (node->kind != DOM_ELEMENT) continue;
        int best_spec = -1;
        const css_rule_t* best_rule = 0;
        for (int r = 0; r < eng->sheet.rule_count; r++) {
            const css_rule_t* rule = &eng->sheet.rules[r];
            if (match_selector(eng, i, &rule->selector)) {
                if (rule->selector.specificity > best_spec) {
                    best_spec = rule->selector.specificity;
                    best_rule = rule;
                }
            }
        }
        if (best_rule) {
            for (int p = 0; p < best_rule->prop_count; p++) {
                apply_property(node, best_rule->props[p].name, best_rule->props[p].value);
            }
        }
    }
    for (int i = 0; i < eng->node_count; i++) {
        dom_node_t* node = &eng->nodes[i];
        if (node->kind != DOM_ELEMENT) continue;
        if (node->parent >= 0) {
            dom_node_t* parent = &eng->nodes[node->parent];
            if (node->color == 0xFF000000) node->color = parent->color;
            if (node->font_size == 16 && parent->font_size != 16) node->font_size = parent->font_size;
        }
        if (node->color == 0xFF000000) node->color = 0x000000;
    }
}

static void extract_attr(const char* tag_str, const char* attr_name, char* out, int max) {
    out[0] = '\0';
    int i = 0;
    while (tag_str[i] && tag_str[i] != ' ' && tag_str[i] != '>' && tag_str[i] != '/') i++;
    while (tag_str[i]) {
        while (tag_str[i] == ' ' || tag_str[i] == '\t' || tag_str[i] == '\n' || tag_str[i] == '\r') i++;
        if (!tag_str[i] || tag_str[i] == '>' || tag_str[i] == '/') break;
        int j = i;
        while (tag_str[j] && tag_str[j] != '=' && tag_str[j] != ' ' && tag_str[j] != '>' && tag_str[j] != '/') j++;
        int name_len = j - i;
        int matches = (name_len == str_len(attr_name));
        if (matches) {
            int k;
            for (k = 0; k < name_len; k++) {
                char a = tag_str[i + k];
                char b = attr_name[k];
                if (a >= 'A' && a <= 'Z') a += 32;
                if (b >= 'A' && b <= 'Z') b += 32;
                if (a != b) { matches = 0; break; }
            }
        }
        i = j;
        while (tag_str[i] == ' ' || tag_str[i] == '\t') i++;
        if (tag_str[i] == '=') {
            i++;
            while (tag_str[i] == ' ' || tag_str[i] == '\t') i++;
            char quote = 0;
            if (tag_str[i] == '"' || tag_str[i] == '\'') { quote = tag_str[i]; i++; }
            int oi = 0;
            if (matches) {
                while (tag_str[i] && tag_str[i] != '>' && oi < max - 1) {
                    if (quote && tag_str[i] == quote) break;
                    if (!quote && (tag_str[i] == ' ' || tag_str[i] == '\t')) break;
                    out[oi++] = tag_str[i++];
                }
                out[oi] = '\0';
                return;
            } else {
                while (tag_str[i] && tag_str[i] != '>') {
                    if (quote && tag_str[i] == quote) break;
                    if (!quote && (tag_str[i] == ' ' || tag_str[i] == '\t')) break;
                    i++;
                }
            }
            if (quote && tag_str[i] == quote) i++;
        }
    }
}

static int is_self_closing(const char* tag) {
    return str_eq_ci(tag, "br") || str_eq_ci(tag, "hr") || str_eq_ci(tag, "img") ||
           str_eq_ci(tag, "input") || str_eq_ci(tag, "meta") || str_eq_ci(tag, "link") ||
           str_eq_ci(tag, "area") || str_eq_ci(tag, "base") || str_eq_ci(tag, "col") ||
           str_eq_ci(tag, "source");
}

static int is_skippable(const char* tag) {
    return str_eq_ci(tag, "script") || str_eq_ci(tag, "style") || str_eq_ci(tag, "head") || str_eq_ci(tag, "svg");
}

int dom_parse_html(css_engine_t* eng, const char* html) {
    if (!eng || !html) return -1;
    eng->node_count = 0;
    int root = alloc_node(eng);
    if (root < 0) return -1;
    str_copy(eng->nodes[root].tag, "document", CSS_TAG_LEN);
    eng->nodes[root].is_block = 1;
    eng->root = root;

    int parent_stack[16];
    int sp = 0;
    parent_stack[sp++] = root;

    int i = 0;
    int style_text_idx = -1;
    char style_buf[CSS_SRC_LEN];
    int style_len = 0;
    int in_skippable = 0;
    char skip_tag[16];
    skip_tag[0] = '\0';

    while (html[i]) {
        if (in_skippable) {
            char close[24];
            int ci = 0;
            close[ci++] = '<'; close[ci++] = '/';
            int k = 0;
            while (skip_tag[k] && k < 14) { close[ci++] = skip_tag[k++]; }
            close[ci] = '\0';
            int found = 1;
            for (int k2 = 0; k2 < ci; k2++) {
                char a = html[i + k2];
                char b = close[k2];
                if (a >= 'A' && a <= 'Z') a += 32;
                if (a != b) { found = 0; break; }
            }
            if (found && html[i] == '<' && html[i+1] == '/') {
                if (str_eq_ci(skip_tag, "style")) {
                    style_buf[style_len] = '\0';
                    css_parse_stylesheet(&eng->sheet, style_buf);
                }
                while (html[i] && html[i] != '>') i++;
                if (html[i] == '>') i++;
                in_skippable = 0;
                skip_tag[0] = '\0';
                if (sp > 1) sp--;
                continue;
            }
            if (style_text_idx >= 0 && str_eq_ci(skip_tag, "style")) {
                if (style_len < CSS_SRC_LEN - 1) style_buf[style_len++] = html[i];
            }
            i++;
            continue;
        }
        if (html[i] == '<' && html[i+1] == '!') {
            while (html[i] && html[i] != '>') i++;
            if (html[i] == '>') i++;
            continue;
        }
        if (html[i] == '<' && html[i+1] == '/') {
            int j = i + 2;
            char tag[CSS_TAG_LEN];
            int ti = 0;
            while (html[j] && html[j] != '>' && html[j] != ' ' && ti < CSS_TAG_LEN - 1) {
                char c = html[j++];
                if (c >= 'A' && c <= 'Z') c += 32;
                tag[ti++] = c;
            }
            tag[ti] = '\0';
            while (html[j] && html[j] != '>') j++;
            if (html[j] == '>') j++;
            if (sp > 1) {
                int found = 0;
                for (int k = sp - 1; k >= 1; k--) {
                    if (str_eq_ci(eng->nodes[parent_stack[k]].tag, tag)) {
                        sp = k;
                        found = 1;
                        break;
                    }
                }
                if (!found) {
                    if (str_eq_ci(tag, "html") || str_eq_ci(tag, "body")) {
                        // keep root
                    }
                }
            }
            i = j;
            continue;
        }
        if (html[i] == '<') {
            int j = i + 1;
            char tag[CSS_TAG_LEN];
            int ti = 0;
            while (html[j] && html[j] != '>' && html[j] != ' ' && html[j] != '/' && ti < CSS_TAG_LEN - 1) {
                char c = html[j++];
                if (c >= 'A' && c <= 'Z') c += 32;
                tag[ti++] = c;
            }
            tag[ti] = '\0';
            char tag_full[256];
            int tfi = 0;
            while (html[j] && html[j] != '>' && tfi < 255) tag_full[tfi++] = html[j++];
            tag_full[tfi] = '\0';
            int self_close = 0;
            if (tfi > 0 && tag_full[tfi - 1] == '/') { self_close = 1; tag_full[tfi - 1] = '\0'; }
            if (html[j] == '>') j++;

            if (ti == 0) { i = j; continue; }

            int node_idx = alloc_node(eng);
            if (node_idx < 0) { i = j; continue; }
            str_copy(eng->nodes[node_idx].tag, tag, CSS_TAG_LEN);
            eng->nodes[node_idx].is_block = css_is_block_tag(tag);
            if (str_eq_ci(tag, "a") || str_eq_ci(tag, "span") || str_eq_ci(tag, "b") ||
                str_eq_ci(tag, "i") || str_eq_ci(tag, "em") || str_eq_ci(tag, "strong") ||
                str_eq_ci(tag, "code") || str_eq_ci(tag, "sub") || str_eq_ci(tag, "sup")) {
                eng->nodes[node_idx].is_block = 0;
                eng->nodes[node_idx].display_inline = 1;
            }

            char id_buf[CSS_ID_LEN];
            extract_attr(tag_full, "id", id_buf, CSS_ID_LEN);
            if (id_buf[0]) str_copy(eng->nodes[node_idx].id, id_buf, CSS_ID_LEN);

            char class_buf[128];
            extract_attr(tag_full, "class", class_buf, 128);
            if (class_buf[0]) {
                int ci = 0;
                while (class_buf[ci] && eng->nodes[node_idx].class_count < CSS_MAX_CLASSES) {
                    while (class_buf[ci] == ' ') ci++;
                    if (!class_buf[ci]) break;
                    int st = ci;
                    while (class_buf[ci] && class_buf[ci] != ' ') ci++;
                    int len = ci - st;
                    if (len >= CSS_NAME_LEN) len = CSS_NAME_LEN - 1;
                    for (int k = 0; k < len; k++) eng->nodes[node_idx].classes[eng->nodes[node_idx].class_count][k] = class_buf[st + k];
                    eng->nodes[node_idx].classes[eng->nodes[node_idx].class_count][len] = '\0';
                    eng->nodes[node_idx].class_count++;
                }
            }

            char style_attr[256];
            extract_attr(tag_full, "style", style_attr, 256);
            if (style_attr[0]) {
                css_parse_inline_style(&eng->nodes[node_idx], style_attr);
            }

            if (str_eq_ci(tag, "a")) {
                char href_buf[CSS_HREF_LEN];
                extract_attr(tag_full, "href", href_buf, CSS_HREF_LEN);
                if (href_buf[0]) {
                    str_copy(eng->nodes[node_idx].href, href_buf, CSS_HREF_LEN);
                }
            }

            if (sp > 0) {
                int p = parent_stack[sp - 1];
                if (eng->nodes[p].child_count < CSS_MAX_CHILDREN) {
                    int idx = eng->nodes[p].child_count++;
                    eng->nodes[p].children[idx] = node_idx;
                    if (idx > 0) {
                        eng->nodes[eng->nodes[p].children[idx - 1]].next_sibling = node_idx;
                    }
                }
                eng->nodes[node_idx].parent = p;
            }

            if (is_skippable(tag)) {
                in_skippable = 1;
                str_copy(skip_tag, tag, 16);
                if (str_eq_ci(tag, "style")) {
                    style_text_idx = node_idx;
                    style_len = 0;
                    style_buf[0] = '\0';
                }
                i = j;
                continue;
            }
            if (!self_close && !is_self_closing(tag)) {
                if (sp < 16) parent_stack[sp++] = node_idx;
            }
            i = j;
            continue;
        }
        int text_start = i;
        while (html[i] && html[i] != '<') i++;
        int text_end = i;
        int has_text = 0;
        for (int k = text_start; k < text_end; k++) {
            if (!is_space(html[k])) { has_text = 1; break; }
        }
        if (!has_text) continue;
        if (sp == 0) continue;
        int node_idx = alloc_node(eng);
        if (node_idx < 0) continue;
        eng->nodes[node_idx].kind = DOM_TEXT;
        eng->nodes[node_idx].is_block = 0;
        eng->nodes[node_idx].display_inline = 1;
        int ti2 = 0;
        int prev_space = 1;
        for (int k = text_start; k < text_end && ti2 < CSS_TEXT_LEN - 1; k++) {
            char c = html[k];
            if (is_space(c)) {
                if (!prev_space) {
                    eng->nodes[node_idx].text[ti2++] = ' ';
                    prev_space = 1;
                }
            } else {
                eng->nodes[node_idx].text[ti2++] = c;
                prev_space = 0;
            }
        }
        if (ti2 > 0 && eng->nodes[node_idx].text[ti2 - 1] == ' ') ti2--;
        eng->nodes[node_idx].text[ti2] = '\0';
        if (ti2 == 0) {
            eng->node_count--;
            continue;
        }
        int p = parent_stack[sp - 1];
        if (eng->nodes[p].child_count < CSS_MAX_CHILDREN) {
            int idx = eng->nodes[p].child_count++;
            eng->nodes[p].children[idx] = node_idx;
            if (idx > 0) {
                eng->nodes[eng->nodes[p].children[idx - 1]].next_sibling = node_idx;
            }
        }
        eng->nodes[node_idx].parent = p;
    }

    return eng->node_count;
}

static int measure_text_width(const dom_node_t* node) {
    if (node->kind != DOM_TEXT) return 0;
    int w = 0;
    int i = 0;
    while (node->text[i]) {
        uint8_t b0 = (uint8_t)node->text[i];
        if (b0 >= 0x80) {
            if ((b0 & 0xE0) == 0xC0) { w += FONT_WIDTH; i += 2; }
            else if ((b0 & 0xF0) == 0xE0) { w += FONT_WIDTH; i += 3; }
            else if ((b0 & 0xF8) == 0xF0) { w += FONT_WIDTH; i += 4; }
            else { w += FONT_WIDTH; i += 1; }
        } else {
            w += FONT_WIDTH;
            i += 1;
        }
    }
    return w;
}

static void layout_block(css_engine_t* eng, int node_idx, int x, int y, int avail_w, int* out_h) {
    dom_node_t* node = &eng->nodes[node_idx];
    node->x = x + node->margin[3];
    node->y = y + node->margin[0];
    node->w = avail_w - node->margin[1] - node->margin[3];
    if (node->w < 0) node->w = 0;
    int inner_x = node->x + node->border_w[3] + node->padding[3];
    int inner_y = node->y + node->border_w[0] + node->padding[0];
    int inner_w = node->w - node->border_w[1] - node->border_w[3] - node->padding[1] - node->padding[3];
    if (inner_w < 0) inner_w = 0;
    int cur_y = inner_y;
    int cur_x = inner_x;
    int line_max_ascent = FONT_HEIGHT;
    int total_h = 0;

    for (int c = 0; c < node->child_count; c++) {
        int child_idx = node->children[c];
        dom_node_t* child = &eng->nodes[child_idx];
        if (child->kind == DOM_TEXT) {
            int tw = measure_text_width(child);
            if (cur_x + tw > inner_x + inner_w && cur_x > inner_x) {
                cur_x = inner_x;
                cur_y += line_max_ascent;
                line_max_ascent = FONT_HEIGHT;
            }
            child->x = cur_x;
            child->y = cur_y;
            child->w = tw;
            child->h = FONT_HEIGHT;
            cur_x += tw;
            total_h = (cur_y + FONT_HEIGHT) - inner_y;
        } else {
            if (child->display_inline) {
                int sub_h = 0;
                int start_x = cur_x;
                if (child->child_count == 0) {
                    child->x = cur_x;
                    child->y = cur_y;
                    child->w = 0;
                    child->h = FONT_HEIGHT;
                }
                for (int sc = 0; sc < child->child_count; sc++) {
                    int sc_idx = child->children[sc];
                    layout_block(eng, sc_idx, cur_x, cur_y, inner_x + inner_w - cur_x, &sub_h);
                    cur_x = eng->nodes[sc_idx].x + eng->nodes[sc_idx].w;
                    if (eng->nodes[sc_idx].y > cur_y) cur_y = eng->nodes[sc_idx].y;
                }
                if (child->child_count == 0) {
                    child->w = 0;
                    child->h = FONT_HEIGHT;
                } else {
                    child->x = start_x;
                    child->y = cur_y;
                    child->w = cur_x - start_x;
                    child->h = FONT_HEIGHT;
                }
                total_h = (cur_y + FONT_HEIGHT) - inner_y;
            } else {
                if (cur_x > inner_x) {
                    cur_x = inner_x;
                    cur_y += line_max_ascent;
                    line_max_ascent = FONT_HEIGHT;
                }
                int sub_h = 0;
                layout_block(eng, child_idx, inner_x, cur_y, inner_w, &sub_h);
                cur_y += sub_h;
                if (child->margin[0] > 0) cur_y += child->margin[0];
                total_h = cur_y - inner_y;
                cur_x = inner_x;
            }
        }
    }
    int content_h = total_h;
    if (content_h < FONT_HEIGHT) content_h = FONT_HEIGHT;
    node->content_x = inner_x;
    node->content_y = inner_y;
    node->content_w = inner_w;
    node->content_h = content_h;
    node->h = content_h + node->border_w[0] + node->border_w[2] + node->padding[0] + node->padding[2];
    if (out_h) *out_h = node->h;
    node->laid_out = 1;
}

void css_layout(css_engine_t* eng, int x, int y, int w, int h) {
    (void)h;
    if (!eng || eng->root < 0) return;
    int dummy = 0;
    layout_block(eng, eng->root, x, y, w, &dummy);
}

static void render_append(css_engine_t* eng, const char* s) {
    int i = 0;
    while (s[i] && eng->render_len < CSS_RENDER_BYTES - 2) {
        eng->render_buf[eng->render_len++] = s[i++];
    }
    eng->render_buf[eng->render_len] = '\0';
}

static void render_append_char(css_engine_t* eng, char c) {
    if (eng->render_len < CSS_RENDER_BYTES - 2) {
        eng->render_buf[eng->render_len++] = c;
        eng->render_buf[eng->render_len] = '\0';
    }
}

static void render_append_int(css_engine_t* eng, int v) {
    char buf[12];
    int i = 0;
    if (v == 0) { render_append_char(eng, '0'); return; }
    if (v < 0) { render_append_char(eng, '-'); v = -v; }
    while (v > 0 && i < 11) { buf[i++] = '0' + (v % 10); v /= 10; }
    while (i > 0) render_append_char(eng, buf[--i]);
}

static void render_append_node(css_engine_t* eng, int node_idx, int* cy) {
    dom_node_t* node = &eng->nodes[node_idx];
    if (node->kind == DOM_TEXT) {
        if (node->text[0]) {
            render_append(eng, "\x02");
            render_append_int(eng, node->x);
            render_append_char(eng, ',');
            render_append_int(eng, node->y);
            render_append_char(eng, ',');
            render_append_int(eng, (int)(node->color & 0xFFFFFF));
            render_append_char(eng, ',');
            render_append_int(eng, node->font_bold ? 1 : 0);
            render_append_char(eng, ',');
            render_append_int(eng, node->font_italic ? 1 : 0);
            render_append_char(eng, ',');
            render_append_int(eng, node->underline ? 1 : 0);
            render_append_char(eng, ',');
            render_append_int(eng, node->link_index);
            render_append_char(eng, ';');
            int i = 0;
            while (node->text[i] && eng->render_len < CSS_RENDER_BYTES - 8) {
                render_append_char(eng, node->text[i++]);
            }
            render_append(eng, "\x03");
        }
        return;
    }
    if (node->bg_color != 0 && node->kind == DOM_ELEMENT && node->content_w > 0 && node->content_h > 0) {
        render_append(eng, "\x04");
        render_append_int(eng, node->content_x);
        render_append_char(eng, ',');
        render_append_int(eng, node->content_y);
        render_append_char(eng, ',');
        render_append_int(eng, node->content_w);
        render_append_char(eng, ',');
        render_append_int(eng, node->content_h);
        render_append_char(eng, ',');
        render_append_int(eng, (int)(node->bg_color & 0xFFFFFF));
        render_append(eng, "\x05");
    }
    for (int c = 0; c < node->child_count; c++) {
        render_append_node(eng, node->children[c], cy);
    }
}

int css_render_to_buffer(css_engine_t* eng) {
    if (!eng || eng->root < 0) return 0;
    eng->render_len = 0;
    eng->render_buf[0] = '\0';
    int cy = 0;
    render_append_node(eng, eng->root, &cy);
    eng->render_buf[eng->render_len] = '\0';
    return eng->render_len;
}
