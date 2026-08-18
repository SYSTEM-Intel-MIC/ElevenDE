/*
 * elevende-keybind - ElevenDE shortcut management engine.
 *
 * Merges the system shortcut defaults (/usr/local/share/elevende-shell/
 * shortcuts.json) with user overrides (~/.config/elevende/shortcuts.json),
 * then regenerates the Openbox rc.xml from rc.template.xml (marker
 * <!--ELEVENDE-KEYBINDS-->) and hot-reloads Openbox.
 *
 * The Settings app edits the user JSON and calls:  elevende-keybind --apply
 * The session script also calls --apply before starting Openbox.
 *
 * Commands:
 *   --apply     generate ~/.config/elevende/rc.xml + openbox --reconfigure
 *   --merged    print the merged shortcut set as JSON to stdout
 *   --reset     delete the user override file
 *
 * Shortcut entry fields:
 *   id, name, keys (Openbox syntax e.g. "W-e", "C-A-Delete"),
 *   type ("exec" | "wm" | "sas"), command, enabled (bool)
 *
 *   type=exec  -> <action name="Execute"><command>...</command></action>
 *   type=wm    -> command is a ';' separated list of Openbox action names
 *   type=sas   -> informational only (sas-screen daemon owns the hotkey)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <X11/Xlib.h>

/* Screen size for expanding HalfLeft/HalfRight snap actions (vanilla
 * openbox has no HalfLeft action). Falls back to 1920x1080 when no
 * display is available (e.g. generating offline). */
static int g_scr_w = 1920, g_scr_h = 1080;
static void detect_screen_size(void) {
    Display *d = XOpenDisplay(NULL);
    if (!d) return;
    const int s = DefaultScreen(d);
    g_scr_w = DisplayWidth(d, s);
    g_scr_h = DisplayHeight(d, s);
    XCloseDisplay(d);
}

#define SHARE_DIR   "/usr/local/share/elevende-shell"
#define TEMPLATE    SHARE_DIR "/rc.template.xml"
#define FALLBACK_RC SHARE_DIR "/rc.xml"
#define SYS_JSON    SHARE_DIR "/shortcuts.json"
#define MARKER      "<!--ELEVENDE-KEYBINDS-->"

/* ============================ tiny JSON parser =========================== */

typedef struct JVal {
    enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } type;
    int bval;
    double nval;
    char *sval;                 /* unescaped string */
    struct JVal *child;         /* first child (array elems / object kvs) */
    struct JVal *next;
    char *key;                  /* for object members */
} JVal;

typedef struct { const char *p; const char *end; int err; } JP;

static void jskip(JP *j)
{
    while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' ||
                             *j->p == '\n' || *j->p == '\r'))
        j->p++;
}

static JVal *jnew(int type)
{
    JVal *v = calloc(1, sizeof *v);
    if (v) v->type = type;
    return v;
}

static void jfree(JVal *v)
{
    while (v) {
        JVal *n = v->next;
        jfree(v->child);
        free(v->sval);
        free(v->key);
        free(v);
        v = n;
    }
}

static JVal *jparse(JP *j);

static char *jparse_string(JP *j)
{
    if (j->p >= j->end || *j->p != '"') { j->err = 1; return NULL; }
    j->p++;
    size_t cap = 32, len = 0;
    char *out = malloc(cap);
    if (!out) { j->err = 1; return NULL; }
    while (j->p < j->end && *j->p != '"') {
        char c = *j->p++;
        if (c == '\\' && j->p < j->end) {
            char e = *j->p++;
            switch (e) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'u': {
                /* \uXXXX: decode BMP to UTF-8 (surrogates pass as '?') */
                if (j->end - j->p < 4) { j->err = 1; free(out); return NULL; }
                unsigned int cp = 0;
                for (int i = 0; i < 4; i++) {
                    char h = *j->p++;
                    cp <<= 4;
                    if (h >= '0' && h <= '9') cp |= h - '0';
                    else if (h >= 'a' && h <= 'f') cp |= h - 'a' + 10;
                    else if (h >= 'A' && h <= 'F') cp |= h - 'A' + 10;
                    else { j->err = 1; free(out); return NULL; }
                }
                if (cp >= 0xD800 && cp <= 0xDFFF) {
                    if (len + 1 >= cap) { cap *= 2; out = realloc(out, cap); }
                    out[len++] = '?';
                    continue;
                }
                char utf[4]; int ul = 0;
                if (cp < 0x80) utf[ul++] = cp;
                else if (cp < 0x800) {
                    utf[ul++] = 0xC0 | (cp >> 6);
                    utf[ul++] = 0x80 | (cp & 0x3F);
                } else {
                    utf[ul++] = 0xE0 | (cp >> 12);
                    utf[ul++] = 0x80 | ((cp >> 6) & 0x3F);
                    utf[ul++] = 0x80 | (cp & 0x3F);
                }
                while (len + ul + 1 >= cap) { cap *= 2; out = realloc(out, cap); }
                memcpy(out + len, utf, ul);
                len += ul;
                continue;
            }
            default: c = e; break;
            }
        }
        if (len + 2 >= cap) { cap *= 2; out = realloc(out, cap); if (!out) { j->err = 1; return NULL; } }
        out[len++] = c;
    }
    if (j->p >= j->end) { j->err = 1; free(out); return NULL; }
    j->p++;                     /* closing quote */
    out[len] = 0;
    return out;
}

static JVal *jparse(JP *j)
{
    jskip(j);
    if (j->p >= j->end) { j->err = 1; return NULL; }
    const char c = *j->p;
    if (c == '{') {
        j->p++;
        JVal *obj = jnew(J_OBJ);
        JVal **tail = &obj->child;
        jskip(j);
        if (j->p < j->end && *j->p == '}') { j->p++; return obj; }
        for (;;) {
            jskip(j);
            char *key = jparse_string(j);
            if (!key) { j->err = 1; jfree(obj); return NULL; }
            jskip(j);
            if (j->p >= j->end || *j->p != ':') { free(key); j->err = 1; jfree(obj); return NULL; }
            j->p++;
            JVal *val = jparse(j);
            if (!val) { free(key); jfree(obj); return NULL; }
            val->key = key;
            *tail = val;
            tail = &val->next;
            jskip(j);
            if (j->p < j->end && *j->p == ',') { j->p++; continue; }
            if (j->p < j->end && *j->p == '}') { j->p++; break; }
            j->err = 1; jfree(obj); return NULL;
        }
        return obj;
    }
    if (c == '[') {
        j->p++;
        JVal *arr = jnew(J_ARR);
        JVal **tail = &arr->child;
        jskip(j);
        if (j->p < j->end && *j->p == ']') { j->p++; return arr; }
        for (;;) {
            JVal *val = jparse(j);
            if (!val) { jfree(arr); return NULL; }
            *tail = val;
            tail = &val->next;
            jskip(j);
            if (j->p < j->end && *j->p == ',') { j->p++; continue; }
            if (j->p < j->end && *j->p == ']') { j->p++; break; }
            j->err = 1; jfree(arr); return NULL;
        }
        return arr;
    }
    if (c == '"') {
        JVal *v = jnew(J_STR);
        v->sval = jparse_string(j);
        if (!v->sval) { jfree(v); return NULL; }
        return v;
    }
    if (!strncmp(j->p, "true", 4))  { j->p += 4; JVal *v = jnew(J_BOOL); v->bval = 1; return v; }
    if (!strncmp(j->p, "false", 5)) { j->p += 5; JVal *v = jnew(J_BOOL); v->bval = 0; return v; }
    if (!strncmp(j->p, "null", 4))  { j->p += 4; return jnew(J_NULL); }
    /* number */
    char *endp = NULL;
    double n = strtod(j->p, &endp);
    if (endp == j->p) { j->err = 1; return NULL; }
    j->p = endp;
    JVal *v = jnew(J_NUM);
    v->nval = n;
    return v;
}

static const JVal *jget(const JVal *obj, const char *key)
{
    if (!obj || obj->type != J_OBJ) return NULL;
    for (const JVal *c = obj->child; c; c = c->next)
        if (c->key && !strcmp(c->key, key))
            return c;
    return NULL;
}

static const char *jstr(const JVal *obj, const char *key, const char *dflt)
{
    const JVal *v = jget(obj, key);
    return (v && v->type == J_STR && v->sval) ? v->sval : dflt;
}

static int jbool(const JVal *obj, const char *key, int dflt)
{
    const JVal *v = jget(obj, key);
    return (v && v->type == J_BOOL) ? v->bval : dflt;
}

static char *read_file(const char *path, size_t *outlen)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)n, f);
    buf[rd] = 0;
    fclose(f);
    if (outlen) *outlen = rd;
    return buf;
}

/* ============================ shortcut model =========================== */

typedef struct Shortcut {
    char *id, *name, *keys, *type, *command;
    int enabled;
    struct Shortcut *next;
} Shortcut;

static Shortcut *sc_find(Shortcut *list, const char *id)
{
    for (Shortcut *s = list; s; s = s->next)
        if (s->id && !strcmp(s->id, id))
            return s;
    return NULL;
}

static char *xstrdup(const char *s) { return s ? strdup(s) : NULL; }

static void sc_absorb(Shortcut **list, const char *json)
{
    if (!json) return;
    JP j = { json, json + strlen(json), 0 };
    JVal *root = jparse(&j);
    if (!root) return;
    const JVal *arr = jget(root, "shortcuts");
    if (arr && arr->type == J_ARR) {
        for (const JVal *e = arr->child; e; e = e->next) {
            if (e->type != J_OBJ) continue;
            const char *id = jstr(e, "id", "");
            if (!*id) continue;
            Shortcut *s = sc_find(*list, id);
            if (!s) {
                s = calloc(1, sizeof *s);
                s->id = strdup(id);
                s->enabled = 1;
                s->next = *list;
                *list = s;
            }
            const char *v;
            if ((v = jstr(e, "name", NULL)))    { free(s->name);    s->name = xstrdup(v); }
            if ((v = jstr(e, "keys", NULL)))    { free(s->keys);    s->keys = xstrdup(v); }
            if ((v = jstr(e, "type", NULL)))    { free(s->type);    s->type = xstrdup(v); }
            if ((v = jstr(e, "command", NULL))) { free(s->command); s->command = xstrdup(v); }
            s->enabled = jbool(e, "enabled", s->enabled);
        }
    }
    jfree(root);
}

static void sc_free(Shortcut *list)
{
    while (list) {
        Shortcut *n = list->next;
        free(list->id); free(list->name); free(list->keys);
        free(list->type); free(list->command);
        free(list);
        list = n;
    }
}

/* ============================ XML generation =========================== */

static void xml_escape(FILE *out, const char *s)
{
    for (; *s; s++) {
        switch (*s) {
        case '&':  fputs("&amp;", out); break;
        case '<':  fputs("&lt;", out); break;
        case '>':  fputs("&gt;", out); break;
        case '"':  fputs("&quot;", out); break;
        case '\'': fputs("&apos;", out); break;
        default:   fputc(*s, out); break;
        }
    }
}

static void gen_keybind(FILE *out, const Shortcut *s)
{
    if (!s->enabled || !s->keys || !*s->keys)
        return;
    if (s->type && !strcmp(s->type, "sas"))
        return;   /* owned by the sas-screen daemon, not Openbox */
    fprintf(out, "    <keybind key=\"%s\">\n", s->keys);
    if (s->type && !strcmp(s->type, "wm") && s->command) {
        char *dup = strdup(s->command);
        for (char *tok = strtok(dup, ";"); tok; tok = strtok(NULL, ";")) {
            if (!strcmp(tok, "HalfLeft")) {
                /* Win+Left: left half of the screen */
                fprintf(out, "      <action name=\"MoveResizeTo\">"
                             "<x>0</x><y>0</y>"
                             "<width>%d</width><height>%d</height>"
                             "</action>\n", g_scr_w / 2, g_scr_h);
            } else if (!strcmp(tok, "HalfRight")) {
                fprintf(out, "      <action name=\"MoveResizeTo\">"
                             "<x>%d</x><y>0</y>"
                             "<width>%d</width><height>%d</height>"
                             "</action>\n", g_scr_w / 2, g_scr_w / 2, g_scr_h);
            } else {
                fprintf(out, "      <action name=\"%s\"/>\n", tok);
            }
        }
        free(dup);
    } else if (s->command && *s->command) {
        fputs("      <action name=\"Execute\"><command>", out);
        xml_escape(out, s->command);
        fputs("</command></action>\n", out);
    } else {
        fprintf(out, "    </keybind>\n");
        return;
    }
    fprintf(out, "    </keybind>\n");
}

static void user_config_dir(char *buf, size_t n)
{
    const char *home = getenv("HOME");
    snprintf(buf, n, "%s/.config/elevende", home ? home : "/tmp");
}

static int apply(const Shortcut *list)
{
    detect_screen_size();
    char dir[1024], outpath[1280];
    user_config_dir(dir, sizeof dir);
    char mkdirp[1024];
    snprintf(mkdirp, sizeof mkdirp, "%s/.config", getenv("HOME") ? getenv("HOME") : "/tmp");
    mkdir(mkdirp, 0755);
    mkdir(dir, 0755);
    snprintf(outpath, sizeof outpath, "%s/rc.xml", dir);

    size_t tlen = 0;
    char *tmpl = read_file(TEMPLATE, &tlen);
    if (!tmpl) {
        fprintf(stderr, "elevende-keybind: template %s missing, falling back\n", TEMPLATE);
        tmpl = read_file(FALLBACK_RC, &tlen);
    }
    if (!tmpl) {
        fprintf(stderr, "elevende-keybind: no rc template available\n");
        return 1;
    }

    /* build the keybind fragment in memory */
    char *frag = NULL;
    size_t fraglen = 0;
    FILE *mf = open_memstream(&frag, &fraglen);
    if (!mf) { free(tmpl); return 1; }
    for (const Shortcut *s = list; s; s = s->next)
        gen_keybind(mf, s);
    fclose(mf);

    /* splice fragment at the marker position */
    FILE *out = fopen(outpath, "wb");
    if (!out) {
        fprintf(stderr, "elevende-keybind: cannot write %s\n", outpath);
        free(tmpl); free(frag);
        return 1;
    }
    char *marker = strstr(tmpl, MARKER);
    if (marker) {
        fwrite(tmpl, 1, (size_t)(marker - tmpl), out);
        fwrite(frag, 1, fraglen, out);
        fputs(marker + strlen(MARKER), out);
    } else {
        /* no marker: write template then append fragment before </keyboard>
         * if present, otherwise just append */
        char *kb = strstr(tmpl, "</keyboard>");
        if (kb) {
            fwrite(tmpl, 1, (size_t)(kb - tmpl), out);
            fwrite(frag, 1, fraglen, out);
            fputs(kb, out);
        } else {
            fwrite(tmpl, 1, tlen, out);
            fwrite(frag, 1, fraglen, out);
        }
    }
    fclose(out);
    free(tmpl);
    free(frag);

    printf("elevende-keybind: wrote %s\n", outpath);

    /* hot reload if a session is up */
    if (getenv("DISPLAY")) {
        int rc = system("openbox --reconfigure >/dev/null 2>&1");
        if (rc != 0)
            fprintf(stderr, "elevende-keybind: note: openbox --reconfigure unavailable "
                            "(session not running?); changes apply next login\n");
    }
    return 0;
}

static char *esc_json(const char *s, char *buf, size_t n)
{
    size_t o = 0;
    for (; *s && o + 7 < n; s++) {
        const unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            buf[o++] = '\\';
            buf[o++] = (char)c;
        } else if (c < 0x20) {
            o += (size_t)snprintf(buf + o, n - o, "\\u%04x", c);
        } else {
            buf[o++] = (char)c;
        }
    }
    buf[o] = 0;
    return buf;
}

static void print_merged(const Shortcut *list)
{
    char e_id[1024], e_name[1024], e_keys[512], e_type[128], e_cmd[2048];
    printf("{\n  \"version\": 1,\n  \"shortcuts\": [\n");
    int first = 1;
    for (const Shortcut *s = list; s; s = s->next) {
        if (!first) printf(",\n");
        first = 0;
        esc_json(s->id ? s->id : "", e_id, sizeof e_id);
        esc_json(s->name ? s->name : "", e_name, sizeof e_name);
        esc_json(s->keys ? s->keys : "", e_keys, sizeof e_keys);
        esc_json(s->type ? s->type : "exec", e_type, sizeof e_type);
        esc_json(s->command ? s->command : "", e_cmd, sizeof e_cmd);
        printf("    { \"id\": \"%s\", \"name\": \"%s\", \"keys\": \"%s\", "
               "\"type\": \"%s\", \"command\": \"%s\", \"enabled\": %s }",
               e_id, e_name, e_keys, e_type, e_cmd,
               s->enabled ? "true" : "false");
    }
    printf("\n  ]\n}\n");
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "--apply";

    char userjson[1280];
    char dir[1024];
    user_config_dir(dir, sizeof dir);
    snprintf(userjson, sizeof userjson, "%s/shortcuts.json", dir);

    if (!strcmp(mode, "--reset")) {
        if (unlink(userjson) == 0)
            printf("elevende-keybind: removed %s\n", userjson);
        else
            printf("elevende-keybind: nothing to remove\n");
        return 0;
    }

    Shortcut *list = NULL;
    char *sys = read_file(SYS_JSON, NULL);
    if (!sys)
        fprintf(stderr, "elevende-keybind: warning: %s not found\n", SYS_JSON);
    sc_absorb(&list, sys);
    free(sys);
    char *user = read_file(userjson, NULL);
    sc_absorb(&list, user);
    free(user);

    int rc = 0;
    if (!strcmp(mode, "--merged"))
        print_merged(list);
    else
        rc = apply(list);

    sc_free(list);
    return rc;
}
