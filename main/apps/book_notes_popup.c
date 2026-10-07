/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 * 点划线句子弹想法：居中浮层，想法全文直出（作者小字在上、♥赞数行尾、正文
 * 几行排几行），本页装不下的整条自动移下一页，超页长文独占一页末行省略；
 * 一页排几条排几条，末页自然留白，横线分隔无卡片框。
 * 弹窗秒开：SD 流式游标 + 惰性续读，翻页增量加载；末页再翻 = 读完即收起。
 * 物理键 1/3 翻页、2 关闭；点 X/弹窗外关闭。
 * / Tap-a-highlight popup: centered sheet showing each thought in full — small
 * / author line (heart+likes right-aligned) above the whole body; a thought
 * / that cannot fit moves whole to the next page, page-tall ones stand alone.
 * / Opens instantly via an SD streaming cursor with lazy incremental reads;
 * / flipping past the last page closes the sheet.
 */
#include "book_notes_popup.h"

#include <stdio.h>
#include <string.h>
#include <limits.h>

#include "epdiy.h"
#include "book_layout.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "read_pico_transfer.h"
#include "settings.h"
#include "ui_gesture.h"
#include "ui_kit.h"
#include "ttf_font.h"
#include "ui_menu.h"
#include "weread_notes.h"
#include "weread_service.h"

static const char* NOTES_TAG = "book_notes";

// 浮层几何（fb 为 684x1216 竖坐标）：顶部留出一行正文，下方留 244px 正文带
// 供点击关闭。列表高度自适应：每页从当前条起逐条排版，按实际行数累加高度，
// 排到分页栏上沿为止——内容多则页满，末页自然留白，不固定条数。
// / Sheet geometry: a body line peeks above and a 244 px body strip stays tappable
// / below. Adaptive paging: rows flow from the page start until the pager line,
// / so pages fill by height instead of a fixed row count.
#define NOTES_SHEET_TOP 108
#define NOTES_SHEET_X 24
#define NOTES_SHEET_W 636
#define NOTES_SHEET_BOTTOM 972
#define NOTES_MAX_ROWS 16   ///< 单页热区上限（自适应后极端小字号下的保险）。/ Row-rect cap.
#define NOTES_MAX_PAGES 96  ///< 页起点上限（单句 200 条 / 每页至少 3 条）。/ Page-start cap.
#define NOTES_BODY_PX 38
#define NOTES_META_PX 22
#define NOTES_HL_PX 28
#define NOTES_LINE_CAP 512
#define NOTES_LIST_X 48
#define NOTES_LIST_W 588
#define NOTES_LIST_TOP 302
#define NOTES_PAGER_Y 896
#define NOTES_PAGER_H 72

static struct {
    bool open;
    weread_highlight_t hit;        ///< 命中的划线（含章内序号）。/ Matched highlight.
    weread_note_t* notes;          ///< PSRAM，已载入想法快照。/ PSRAM thoughts loaded so far.
    bool owned;                    ///< notes 归弹窗所有（false = 引用章快照）。/ Owned vs borrowed notes.
    weread_notes_cursor_t* cursor; ///< SD 流式游标，翻页续读。/ SD cursor for lazy paging.
    unsigned capacity;             ///< notes 已分配条数。/ Allocated slots.
    unsigned loaded;               ///< 已从游标读出的条数。/ Thoughts pulled from the cursor.
    unsigned count;
    unsigned page;                 ///< 当前页（0 基）。/ Current page, zero based.
    unsigned pages;                ///< 已知页数下界（自适应排版逐步确定）。/ Known page lower bound.
    unsigned page_start[NOTES_MAX_PAGES]; ///< 每页起始条下标。/ First row index of each page.
    unsigned rows_used[NOTES_MAX_PAGES];  ///< 每页实际渲染条数（渲染时记录）。/ Rows actually laid out per page.
} s_notes;

// 弹窗文本统一强制走阅读 TTF 字体：ui_text 会优先用内置位图字库、缺字才落 TTF，
// 导致弹窗内位图黑体与阅读字体（如仓耳今楷）混排；以下包装与 ui_text 系行为
// 镜像但恒走 TTF 路径，保证弹窗与正文同一字体。
// / Force the reading TTF face for every popup string: ui_text prefers built-in
// / bitmap glyphs and only falls back to the TTF on missing glyphs, which mixes
// / two faces inside one popup. These mirror ui_text but always draw via the TTF.
static int notes_text_width(int px, const char* s) {
    return s && px > 0 ? ttf_text_width_px(px, s) : 0;
}
static void notes_draw_text(uint8_t* fb, int x, int y, int px, const char* s,
                            enum EpdFontFlags align, bool inverted) {
    const int eff = ui_text_effective_px(px);
    ttf_draw_text_px(fb, x, y + ttf_ascender_px(eff), eff, s, align,
                     inverted ? UI_INK_WHITE : UI_INK_BLACK,
                     inverted ? UI_INK_BLACK : UI_INK_WHITE);
}
static void notes_draw_text_vc(uint8_t* fb, int x, int center_y, int px, const char* s,
                               enum EpdFontFlags align, bool inverted) {
    const int eff = ui_text_effective_px(px);
    int above = 0, below = 0;
    ttf_measure_line_px(eff, s, &above, &below);
    notes_draw_text(fb, x, center_y + (above - below) / 2, px, s, align, inverted);
}

// 单行截断与折行绘制辅助；测量必须过 ui_text_effective_px 与绘制同源。
// / Single-line clip and wrap helpers; measuring must match ui_text's effective px.
static void popup_fit(char* dst, size_t cap, const char* src, int px, int width) {
    // 测量必须与绘制同源：ui_text 内部会过 ui_text_effective_px（+2 与系统字号缩放），
    // 直接用原始 px 测量会系统性偏小，折行偏晚导致文字画出卡片。
    // / Measure with the same effective px that ui_text draws at, else wrapping is
    // / systematically too late and text spills past the card edge.
    const int eff = ui_text_effective_px(px);
    size_t n = strnlen(src, cap - 1);
    if (dst != src) {
        memcpy(dst, src, n);
        dst[n] = 0;
    }
    while (n && notes_text_width(eff, dst) > width) {
        --n;
        while (n && ((unsigned char)dst[n] & 0xc0) == 0x80) --n;
        dst[n] = 0;
    }
    // 单行被截断时补省略号（若还放得下）。/ Add an ellipsis when the single line is clipped.
    if (n < strnlen(src, cap - 1)) {
        while (n >= 3 && notes_text_width(eff, dst) + notes_text_width(eff, "…") > width) {
            --n;
            while (n && ((unsigned char)dst[n] & 0xc0) == 0x80) --n;
            dst[n] = 0;
        }
        if (n + 4 < cap) memcpy(dst + n, "…", 4);
    }
}

// 按像素宽折行绘制，超出行数在末行截断并加省略号；行高与测量都用 effective 字号，
// 调用方传入可改写副本（容量需比原文多 4 字节，留给末行省略号）。
// / Wrap by pixel width with an ellipsis on the clipped last line; line height and
// / measuring use the effective px. Caller passes a mutable copy with 4 spare bytes.
static int popup_paragraph(uint8_t* fb, int x, int y, int width, int px, char* text, int lines) {
    const int eff = ui_text_effective_px(px);
    const int step = eff + 10;
    int drawn = 0;
    char* cursor = text;
    for (int row = 0; row < lines && cursor[0]; ++row) {
        const int top = y + row * step;
        size_t keep = strlen(cursor);
        while (keep) {
            const char saved = cursor[keep];
            cursor[keep] = 0;
            const int w = notes_text_width(eff, cursor);
            cursor[keep] = saved;
            if (w <= width) break;
            --keep;
            while (keep && ((unsigned char)cursor[keep] & 0xc0) == 0x80) --keep;
        }
        if (!keep) break;
        const char* rest = cursor + keep;
        while (*rest == ' ') ++rest;
        if (row == lines - 1) {
            // 末行：还有剩余内容则收窄并补省略号。/ Last line: shrink and ellipsize if more remains.
            if (*rest) {
                const int ell_w = notes_text_width(eff, "…");
                while (keep) {
                    const char saved = cursor[keep];
                    cursor[keep] = 0;
                    const int w = notes_text_width(eff, cursor);
                    cursor[keep] = saved;
                    if (w + ell_w + 4 <= width) break;
                    --keep;
                    while (keep && ((unsigned char)cursor[keep] & 0xc0) == 0x80) --keep;
                }
                memcpy(cursor + keep, "…", 4);
            } else {
                cursor[keep] = 0;
            }
            notes_draw_text(fb, x, top, px, cursor, EPD_DRAW_ALIGN_LEFT, false);
            ++drawn;
            break;
        }
        const char next = cursor[keep];
        cursor[keep] = 0;
        notes_draw_text(fb, x, top, px, cursor, EPD_DRAW_ALIGN_LEFT, false);
        cursor[keep] = next;
        cursor += keep;
        while (*cursor == ' ') ++cursor;
        ++drawn;
    }
    return drawn;
}

static void popup_reset(void) {
    if (s_notes.owned) heap_caps_free(s_notes.notes);
    weread_notes_cursor_close(s_notes.cursor);
    s_notes.notes = NULL;
    s_notes.cursor = NULL;
    s_notes.count = s_notes.page = 0;
    s_notes.pages = 1;
    memset(s_notes.page_start, 0, sizeof(s_notes.page_start));
    memset(s_notes.rows_used, 0, sizeof(s_notes.rows_used));
    s_notes.capacity = s_notes.loaded = 0;
    s_notes.open = false;
    s_notes.owned = false;
}

// 惰性加载：把想法从 SD 游标续读到目标条数（翻页只读增量，每条一次寻道）。
// / Lazy load: pull thoughts from the SD cursor up to the target count; paging
// / reads only the increment with one seek per thought.
static void notes_load(unsigned upto) {
    if (!s_notes.cursor) return;
    if (upto > s_notes.count) upto = s_notes.count;
    while (s_notes.loaded < upto) {
        if (s_notes.loaded == s_notes.capacity) {
            const unsigned cap = s_notes.capacity ? s_notes.capacity * 2 : 8;
            weread_note_t* next = (weread_note_t*)heap_caps_realloc(
                s_notes.notes, cap * sizeof(*next), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!next) return;
            s_notes.notes = next;
            s_notes.capacity = cap;
        }
        if (!weread_notes_cursor_next(s_notes.cursor, &s_notes.notes[s_notes.loaded])) {
            // 服务器计数超出实际数据：按实载截断。/ Trust data over the count.
            s_notes.count = s_notes.loaded;
            return;
        }
        ++s_notes.loaded;
    }
}

// ---- 句子级划线想法缓存（underlines + readReviews 全量落盘）：随章切换加载。 ----
// ---- Sentence-level notes cache (underlines + readReviews, fully paged to SD):
// ---- loaded per chapter on switch. ----
static void dec_normalize(char* dst, size_t cap, const char* src);

static struct {
    char book[64];                        ///< 云端书号。/ Remote book id.
    bool opened;                          ///< bind 拿到书号（数据源就绪前提）。/ Book id known.
    char chapter[64];                     ///< 已加载缓存的章 uid。/ Chapter uid of the loaded cache.
    char (*texts)[WEREAD_NOTE_TEXT_CAP];  ///< 归一化划线原文（当前章）。/ Normalized excerpts (this chapter).
    unsigned* ords;                       ///< texts[k] 的章内划线序号。/ Chapter ordinals of texts.
    unsigned count;                       ///< texts 条数（匹配输入）。/ Matching input count.
} s_browse;

static void browse_unload(void) {
    heap_caps_free(s_browse.texts);
    s_browse.texts = NULL;
    heap_caps_free(s_browse.ords);
    s_browse.ords = NULL;
    s_browse.count = 0;
    s_browse.chapter[0] = 0;
}

struct browse_row_ctx {
    char (*texts)[WEREAD_NOTE_TEXT_CAP];
    unsigned* ords;
    unsigned n;
};
// 顺序遍历回调：归一化可匹配的划线原文；序号留给点击读取想法（单文件一次读完，
// 避免 highlight_at 的 O(N²) 重扫冻结 UI）。
// / Sequential row callback: keep matchable excerpts; ordinals feed tap reads
// / (one file pass instead of highlight_at's O(N²) rescans).
static bool browse_row_cb(void* user, const weread_highlight_t* hl) {
    struct browse_row_ctx* ctx = (struct browse_row_ctx*)user;
    dec_normalize(ctx->texts[ctx->n], WEREAD_NOTE_TEXT_CAP, hl->text);
    if (!ctx->texts[ctx->n][0]) return true;  // 无原文不参与映射 / no excerpt, no mapping
    ctx->ords[ctx->n] = hl->index;
    ++ctx->n;
    return true;
}

// 读入某章已缓存划线（PSRAM）；返回 false = 该章缓存不存在或读取失败。
// / Load one chapter's cached highlights into PSRAM; false when absent or unreadable.
static bool browse_load_chapter(uint32_t spine) {
    browse_unload();
    if (!s_browse.opened) return false;
    char uid[64];
    if (!weread_notes_chapter_uid(spine, uid, sizeof(uid)) || !uid[0]) return false;
    const unsigned total = weread_notes_chapter_highlights(spine);
    ESP_LOGI(NOTES_TAG, "browse ch%u uid=%s: highlights=%u", (unsigned)spine, uid, total);
    if (!total) {
        snprintf(s_browse.chapter, sizeof(s_browse.chapter), "%s", uid);
        return true;  // 缓存完整但无人划线。/ Cache complete, no highlights.
    }
    s_browse.texts = (char (*)[WEREAD_NOTE_TEXT_CAP])heap_caps_calloc(
        total, WEREAD_NOTE_TEXT_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_browse.ords = (unsigned*)heap_caps_calloc(
        total, sizeof(unsigned), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_browse.texts || !s_browse.ords) {
        heap_caps_free(s_browse.texts);
        heap_caps_free(s_browse.ords);
        s_browse.texts = NULL;
        s_browse.ords = NULL;
        return false;
    }
    struct browse_row_ctx ctx = {s_browse.texts, s_browse.ords, 0};
    weread_notes_for_each_highlight(spine, browse_row_cb, &ctx);
    s_browse.count = ctx.n;
    snprintf(s_browse.chapter, sizeof(s_browse.chapter), "%s", uid);
    ESP_LOGI(NOTES_TAG, "browse loaded ch%u: %u mark(s)", (unsigned)spine, s_browse.count);
    return true;
}

// ---- 正文划线装饰：章切换时在章节全文中定位偏移区间（纯内存）。 ----
// ---- Body decoration: resolve highlight spans in the chapter text (memory only). ----
static struct {
    uint32_t spine;   ///< 已定位划线的章。/ Chapter whose spans are resolved.
    bool loaded;      ///< 已尝试定位（含空结果）。/ Resolve attempted (empty counts).
    unsigned spans;   ///< 定位成功条数。/ Resolved span count.
} s_dec = {.spine = UINT32_MAX, .loaded = false, .spans = 0};

static void dec_normalize(char* dst, size_t cap, const char* src) {
    size_t w = 0;
    for (const unsigned char* p = (const unsigned char*)src; *p && w + 1 < cap; ++p) {
        if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') continue;
        // 全角空格 U+3000 与零宽空格 U+200B 在 EPUB 排版与划线原文间不对称，一并剔除。
        // / U+3000 and U+200B appear asymmetrically between EPUB lines and markText; strip both.
        if (p[0] == 0xE3 && p[1] == 0x80 && p[2] == 0x80) { p += 2; continue; }
        if (p[0] == 0xE2 && p[1] == 0x80 && p[2] == 0x8B) { p += 2; continue; }
        dst[w++] = (char)*p;
    }
    dst[w] = 0;
}

static void dec_unload(void) {
    book_layout_set_marks(NULL, 0, 0, NULL);
    s_dec.spans = 0;
    s_dec.loaded = false;
}

// 用当前 browse 缓存在章节全文中定位每条划线的源偏移区间（纯内存，跳空白匹配）。
// / Resolve each highlight's source span in the chapter text (memory only).
static void run_decorate(uint32_t spine) {
    dec_unload();
    s_dec.spine = spine;
    s_dec.loaded = true;
    if (!s_browse.count) {
        ESP_LOGI(NOTES_TAG, "decorate ch%u: no chapter reviews", (unsigned)spine);
        return;
    }
    unsigned spans = 0;
    const bool ok = book_layout_set_marks((const char*)s_browse.texts, WEREAD_NOTE_TEXT_CAP,
                                          s_browse.count, &spans);
    s_dec.spans = ok ? spans : 0;
    ESP_LOGI(NOTES_TAG, "decorate ch%u: spans=%u/%u%s", (unsigned)spine, s_dec.spans,
             s_browse.count, ok || !s_browse.count ? "" : " (alloc fail)");
}

static bool wifi_ready(void) {
    read_pico_transfer_status_t st;
    read_pico_transfer_get_status(&st);
    return st.mode == READ_PICO_TRANSFER_MODE_STA && st.network_ready;
}

static uint32_t s_fetch_spine = UINT32_MAX;  ///< 发起拉取时所在章（整本任务）。/ Chapter that started the whole-book fetch.
static bool s_fetch_ran;      ///< 本次绑定整本拉取已跑过（完成/取消后不再自动重启）。
static bool s_fetch_adopted;  ///< 拉取中当前章已提前收割。/ Current chapter adopted mid-fetch.

// 整本划线想法后台拉取：一次发起覆盖全书（时间换数据，断点续传）；
// 离线、任务忙或本绑定已跑过则跳过。
// / Whole-book notes fetch in the background (time for completeness, resumable);
// / skipped when offline, busy, or already ran for this binding.
static void maybe_start_fetch(uint32_t spine) {
    if (spine == UINT32_MAX || !s_browse.opened) return;
    if (s_fetch_spine != UINT32_MAX || s_fetch_ran) return;  // 已在途/已跑过 / in flight or done
    if (!wifi_ready()) {
        ESP_LOGI(NOTES_TAG, "fetch skip ch%u: offline", (unsigned)spine);
        return;
    }
    weread_snapshot_t snap;
    weread_snapshot(&snap);
    if (snap.active) {
        ESP_LOGI(NOTES_TAG, "fetch skip ch%u: worker busy", (unsigned)spine);
        return;
    }
    if (weread_start_notes(s_browse.book)) {
        s_fetch_spine = spine;
        s_fetch_adopted = false;
        ESP_LOGI(NOTES_TAG, "fetch start (whole book) at ch%u book=%s", (unsigned)spine, s_browse.book);
    }
}

// 收割拉取成果：整本任务结束后清标志并重载当前章；任务进行中只要当前章缓存
// 落盘完成即提前收割，不必等整本跑完。每次翻页/重绘（map_page）与章切换
// （set_chapter）都会路过这里。
// / Reap fetch results: reload the current chapter once the task ends; adopt it
// / early when its own cache is complete even while the whole-book task runs.
// / Called from map_page and set_chapter on every repaint/switch.
static void maybe_finish_fetch(uint32_t spine) {
    if (s_fetch_spine == UINT32_MAX) return;
    weread_snapshot_t snap;
    weread_snapshot(&snap);
    if (snap.active) {
        if (s_fetch_adopted || spine == UINT32_MAX || spine != s_dec.spine ||
            !s_browse.opened || s_browse.count) return;
        if (!weread_notes_chapter_done(spine)) return;
        s_fetch_adopted = true;
        ESP_LOGI(NOTES_TAG, "fetch adopt ch%u (task still running)", (unsigned)spine);
        popup_reset();
        if (browse_load_chapter(spine)) run_decorate(spine);
        return;
    }
    s_fetch_spine = UINT32_MAX;
    // 完成/中止后不再自动重启；失败保留重试机会（下次翻章再发起，断点续传）。
    // / No auto restart after success or cancel; failures may retry on the next
    // / flip and resume from the last chapter.
    if (snap.action == WEREAD_NOTES &&
        (snap.state == WEREAD_COMPLETE || snap.state == WEREAD_CANCELLED))
        s_fetch_ran = true;
    ESP_LOGI(NOTES_TAG, "fetch done ch%u state=%d", (unsigned)spine, (int)snap.state);
    if (spine == UINT32_MAX || spine != s_dec.spine || !s_browse.opened) return;
    popup_reset();
    if (browse_load_chapter(spine)) run_decorate(spine);
}

static void load_chapter(uint32_t spine) {
    popup_reset();
    dec_unload();
    s_dec.spine = spine;
    s_dec.loaded = true;
    char uid[64];
    if (!weread_notes_chapter_uid(spine, uid, sizeof(uid)) || !uid[0]) {
        ESP_LOGI(NOTES_TAG, "ch%u: no remote chapter uid", (unsigned)spine);
        return;
    }
    if (browse_load_chapter(spine)) {
        run_decorate(spine);
    } else {
        maybe_start_fetch(spine);  // 缓存缺失→后台整本拉取 / cache miss → background whole-book fetch
    }
}

void book_notes_invalidate(void) {
    // 强制下一次 set_chapter 重算：字体切换重排后行位置全变，同章短路会
    // 保留旧命中缓存导致划线消失；先作废章号即可强制重载。
    // / Force the next set_chapter to reload: after a font switch reflows the
    // / page, the same-spine short circuit would keep stale hit cache and hide
    // / marks; poison the spine id so the reload always runs.
    s_dec.spine = UINT32_MAX;
}

void book_notes_set_chapter(uint32_t spine) {
    if (s_dec.spine == spine) {
        maybe_finish_fetch(spine);
        return;
    }
    // 章切换即同步加载划线并在章节全文中定位偏移区间；此时章节文本已就位。
    // / Load highlights and resolve their source spans right away; chapter text is ready.
    load_chapter(spine);
}
// 排版映射层：输出当前页每条命中划线的屏幕矩形与原文；其余划线计入
// 「不在本页」过滤计数。每次翻页/字号/重绘都会经 render 调到这里。调用方须在
// 绘制锁内（take_line 重放共用静态行缓冲）；此处同时收割后台拉取成果。
// / Layout mapping: log each on-page mark rect with its source text; the rest
// / count as filtered-out. Called from render on every repaint while holding
// / the draw lock (the replay shares the static line buffer); also reaps fetches.
void book_notes_map_page(size_t page) {
    maybe_finish_fetch(s_dec.spine);  // 拉取完成后在本页渲染时即时重载 / reap on repaint
    if (!s_dec.loaded || !s_dec.spans) return;
    book_layout_mark_rect_t rects[16];
    const unsigned n = book_layout_page_mark_rects(page, rects, 16);
    ESP_LOGI(NOTES_TAG, "page %u: %u mark rect(s) on page, %u mark(s) elsewhere",
             (unsigned)page, n, s_dec.spans - n);
    for (unsigned i = 0; i < n; ++i) {
        const EpdRect r = rects[i].rect;
        const unsigned src = book_layout_mark_src(rects[i].mark);
        size_t lo = 0, hi = 0;
        book_layout_mark_span(rects[i].mark, &lo, &hi);
        ESP_LOGI(NOTES_TAG, "  mark[%u] src=%u span=[%zu,%zu) rect=(%d,%d %dx%d) '%.40s'",
                 rects[i].mark, src, lo, hi, r.x, r.y, r.width, r.height,
                 src < s_browse.count ? s_browse.texts[src] : "?");
    }
}

// ---- 阅读时长上报（rt）：tick 累计已读毫秒，5 分钟周期上报，关书补尾巴。 ----
// ---- Reading-time (rt) reports: tick accumulates read ms, 5-min cadence, tail on close. ----
#define WEREAD_REPORT_PERIOD_MS 300000u ///< 周期上报间隔（5 分钟）。/ Periodic cadence.
#define WEREAD_REPORT_TAIL_MS 30000u    ///< 关书尾报下限（更短不值得联网）。/ Tail floor on close.
#define WEREAD_REPORT_MAX_S 600u        ///< 单包 rt 封顶，节奏贴近官方 web 客户端。/ Per-packet rt cap.
static char s_report_book[64];        ///< 空 = 非微信读书书（tick 直返，零开销）。/ empty = not a WeRead book.
static uint32_t s_report_pending_ms;  ///< 自上次上报累计的已读毫秒。/ Read ms since the last report.
static uint16_t s_report_chapter;     ///< tick 记下的最后坐标，关书尾报用。/ Last coords seen, for the tail.
static uint32_t s_report_offset;
static uint8_t s_report_pct;

// 派发一次上报：成功清零累计；周期上报忙拒绝时保留累积（时长不丢，下个周期重试），
// 关书尾报忙拒绝则放弃（≤5 分钟，防书号悬挂串书）。
// / Dispatch one report: clear the accumulator on success; a busy periodic report keeps
// / accumulating for the next cadence, a busy close tail is dropped (≤5 min) to avoid
// / leaking the id into the next book.
static void report_send(uint16_t chapter, uint32_t byte_off, uint8_t pct, bool tail) {
    uint32_t seconds = s_report_pending_ms / 1000;
    if (seconds > WEREAD_REPORT_MAX_S) seconds = WEREAD_REPORT_MAX_S;
    if (!weread_start_read_report(s_report_book, chapter, byte_off, pct, seconds)) {
        if (!tail) return;
        ESP_LOGI(NOTES_TAG, "report tail dropped (busy), pending=%lu ms",
                 (unsigned long)s_report_pending_ms);
    }
    s_report_pending_ms = 0;
}

void book_notes_report_tick(uint32_t elapsed_ms, uint16_t chapter, uint32_t byte_off, uint8_t pct) {
    if (!s_report_book[0]) return;
    s_report_chapter = chapter;
    s_report_offset = byte_off;
    s_report_pct = pct;
    s_report_pending_ms += elapsed_ms;
    if (s_report_pending_ms >= WEREAD_REPORT_PERIOD_MS) report_send(chapter, byte_off, pct, false);
}

void book_notes_report_close(void) {
    if (!s_report_book[0]) {
        s_report_pending_ms = 0;
        return;
    }
    if (s_report_pending_ms >= WEREAD_REPORT_TAIL_MS)
        report_send(s_report_chapter, s_report_offset, s_report_pct, true);
    s_report_pending_ms = 0;
}

bool book_notes_bind(const char* phys_path) {
    book_notes_unbind();
    if (!phys_path || !phys_path[0]) return false;
    // 身份判定在引擎：findBookIdForPath 严格匹配 /WeRead/<书名>.epub（书架记录名）。
    // 虚拟 /WeRead/ 由 HalStorage 映射到书库目录，这里做物理→虚拟前缀换算；
    // 书库外的书（普通导入）直接跳过。非引擎书在反查书架后自然失败，无副作用。
    // / Identity lives in the engine: findBookIdForPath matches /WeRead/<title>.epub.
    // / Virtual /WeRead/ maps onto the library dir; swap the prefix, else skip.
    const char* books = app_settings_books_dir();
    const size_t books_len = books ? strlen(books) : 0;
    if (!books_len || strncmp(phys_path, books, books_len) || phys_path[books_len] != '/') {
        ESP_LOGW(NOTES_TAG, "bind skip (outside library): %s", phys_path);
        return false;
    }
    char virt[272];
    if (snprintf(virt, sizeof(virt), "/WeRead%s", phys_path + books_len) >= (int)sizeof(virt)) return false;
    // 冷启动直接开书时微信读书 app 可能从未 configure，Storage root 为空会让
    // shelf.bin 等虚拟路径全部打不开；这里补一次幂等配置（纯设根目录，不碰网络）。
    // 另外 weread_stop 遗留的取消标志会拦截全部引擎文件读取，空闲期清掉。
    // / Cold starts may skip the weread app entirely, leaving Storage roots empty
    // / so virtual paths fail to open; configure is idempotent and offline-safe.
    // / Also clear a stale cancel flag from weread_stop, which blocks every read.
    (void)weread_configure("/sdcard/.readpico/weread", books);
    ESP_LOGI(NOTES_TAG, "cancel clear: %d", (int)weread_cancel_clear_if_idle());
    const bool ok = weread_notes_bind(virt);
    ESP_LOGI(NOTES_TAG, "bind %s -> %s: %d", phys_path, virt, (int)ok);
    if (!ok) return false;
    // 绑定成功后记录书号；章缓存由 set_chapter 按需加载，缺失时按章拉取。
    // / Record the book id; chapter caches load on demand in set_chapter, fetched
    // / per chapter on miss.
    char book[64];
    if (weread_notes_book_id(book, sizeof(book))) {
        snprintf(s_browse.book, sizeof(s_browse.book), "%s", book);
        s_browse.opened = true;
        // 阅读时长上报目标书号在此捕获（unbind 时 bind 已清，关书靠这里留下的值补尾巴）。
        // / Capture the report target here; unbind runs before book close, so the tail
        // / rides the id captured at open.
        snprintf(s_report_book, sizeof(s_report_book), "%s", book);
        s_report_pending_ms = 0;
        ESP_LOGI(NOTES_TAG, "browse source ready (book=%s)", book);
    } else {
        ESP_LOGI(NOTES_TAG, "browse source unavailable (book=%s)", book);
    }
    return true;
}

void book_notes_unbind(void) {
    // 先补关书尾报再清书号：换书时 bind→unbind 也走这里，尾报归属旧书、坐标是
    // tick 记下的最后位置，正确。/ Flush the close tail before clearing the id: the
    // bind-time unbind on a book switch attributes the tail to the old book, using
    // the last coords recorded by tick.
    book_notes_report_close();
    s_report_book[0] = 0;
    popup_reset();
    dec_unload();
    s_dec.spine = UINT32_MAX;
    browse_unload();
    s_browse.opened = false;
    s_browse.book[0] = 0;
    s_fetch_spine = UINT32_MAX;
    s_fetch_ran = false;
    s_fetch_adopted = false;
    weread_notes_unbind();
}

bool book_notes_bound(void) { return weread_notes_ready(); }
bool book_notes_active(void) { return s_notes.open; }

bool book_notes_tap_mark(uint32_t spine, int mark) {
    (void)spine;  // 点击必然发生在当前章，s_dec.spine 即章上下文。
    if (mark < 0 || !s_browse.opened || !s_browse.count) return false;
    const unsigned src = book_layout_mark_src((unsigned)mark);
    if (src >= s_browse.count) return false;
    const unsigned ordinal = s_browse.ords[src];
    popup_reset();
    // 秒开关键：只打开游标并预载第一页，几十上百条的长句不再一次性全读
    // （旧行为 O(N²) 寻道要卡数秒）；翻页时由 notes_load 按页续读。
    // / Instant open: open the cursor and preload only the first page; long lists
    // / no longer read everything upfront. Later pages pull incrementally.
    unsigned total = 0;
    weread_notes_cursor_t* cursor = weread_notes_cursor_open(s_dec.spine, ordinal, &total);
    if (!cursor || !total) {
        weread_notes_cursor_close(cursor);
        return false;
    }
    s_notes.cursor = cursor;
    s_notes.count = total;
    s_notes.pages = 1;  // 自适应分页：页数随排版逐步确定。/ Pages grow as rows are laid out.
    s_notes.page = 0;
    s_notes.owned = true;
    notes_load(NOTES_MAX_ROWS);  // 首页按最大行数预载，不够渲染时再补。/ Preload, top up while rendering.
    if (!s_notes.loaded || !weread_notes_highlight_at(s_dec.spine, ordinal, &s_notes.hit)) {
        popup_reset();
        return false;
    }
    s_notes.open = true;
    ESP_LOGI(NOTES_TAG, "tap mark%d -> highlight %u: %u thought(s) '%.40s'",
             mark, ordinal, s_notes.count, s_notes.hit.text);
    return true;
}

// 自适应翻页：页边界由渲染时的实际排版确定（page_start/rows_used）。
// 向后翻：下一页起点 = 本页起点 + 本页实排条数；向前翻：回退到已记录的页起点。
// / Adaptive paging: boundaries come from the recorded layout. Forward starts at
// / start+rows_used; backward jumps to the previously recorded page start.
static bool popup_flip(int dir) {
    if (dir < 0) {
        if (!s_notes.page) return false;
        --s_notes.page;
        notes_load(s_notes.page_start[s_notes.page] + NOTES_MAX_ROWS);
        return true;
    }
    const unsigned cur = s_notes.page;
    if (cur + 1 >= NOTES_MAX_PAGES) return false;
    if (!s_notes.rows_used[cur]) return false;  // 未渲染过本页，边界未知。/ Layout unknown yet.
    const unsigned next_start = s_notes.page_start[cur] + s_notes.rows_used[cur];
    if (next_start >= s_notes.count) {
        // 末页再翻 = 想法读完：直接收起弹窗回到正文（返回 true 触发重绘还原）。
        // / Flipping past the last page means the list is finished: close the
        // / sheet and return true so the repaint restores the body text.
        popup_reset();
        return true;
    }
    // 页起点必须每次翻页都记录：渲染期的页数是估算值（只增不减），估算膨胀后
    // cur+1 < pages 恒成立，若只在扩张分支写 page_start，下一页起点会停在 0，
    // 翻页又回到本页。/ The boundary must be recorded on every flip: render
    // inflates `pages` as an estimate, so the growth branch stops firing and the
    // next-page start would stay 0, replaying the same page.
    s_notes.page_start[cur + 1] = next_start;
    if (cur + 1 >= s_notes.pages) s_notes.pages = cur + 2;
    s_notes.page = cur + 1;
    notes_load(next_start + NOTES_MAX_ROWS);
    return true;
}

static EpdRect popup_prev_rect(void) { return (EpdRect){NOTES_LIST_X, NOTES_PAGER_Y, 196, NOTES_PAGER_H}; }
static EpdRect popup_next_rect(void) { return (EpdRect){NOTES_LIST_X + NOTES_LIST_W - 196, NOTES_PAGER_Y, 196, NOTES_PAGER_H}; }
static EpdRect popup_close_rect(void) { return (EpdRect){NOTES_SHEET_X + NOTES_SHEET_W - 72, NOTES_SHEET_TOP + 10, 52, 52}; }

// 点击是否落在浮层之外（含下方正文带）：是则关闭弹窗继续阅读。
// / A tap outside the floating sheet (including the body strip below) closes it.
static bool popup_hit_outside(int x, int y) {
    return x < NOTES_SHEET_X || x > NOTES_SHEET_X + NOTES_SHEET_W ||
           y < NOTES_SHEET_TOP || y > NOTES_SHEET_BOTTOM;
}

bool book_notes_gesture(int type, int x, int y) {
    if (!s_notes.open) return false;
    switch (type) {
        case UI_GESTURE_TAP:
            if (ui_rect_hit(popup_close_rect(), x, y) || popup_hit_outside(x, y)) {
                popup_reset();
                return true;
            }
            if (ui_rect_hit(popup_prev_rect(), x, y)) return popup_flip(-1);
            if (ui_rect_hit(popup_next_rect(), x, y)) return popup_flip(1);
            return false;  // 浮层文字区：想法已全文展示，点击吞掉不重绘。/ Full text shown; dead tap.
        case UI_GESTURE_SWIPE_L:
        case UI_GESTURE_SWIPE_U:
            return popup_flip(1);
        case UI_GESTURE_SWIPE_R:
        case UI_GESTURE_SWIPE_D:
            return popup_flip(-1);
        default:
            return false;
    }
}

bool book_notes_key(int key) {
    if (!s_notes.open) return false;
    if (key == UI_KEY_2) {
        popup_reset();
        return true;
    }
    if (key == UI_KEY_1) return popup_flip(-1);
    if (key == UI_KEY_3) return popup_flip(1);
    return false;
}

void book_notes_close(void) { popup_reset(); }

EpdRect book_notes_area(void) {
    // 开合同一区域（浮层外框）：关闭时同区重绘即可还原被盖住的正文；下方正文带
    // 从未被盖住，无需重绘。/ Same rect for open and close; repainting restores body.
    return (EpdRect){0, NOTES_SHEET_TOP - 2, UI_LOCK_WIDTH,
                     NOTES_SHEET_BOTTOM - NOTES_SHEET_TOP + 4};
}

static void popup_hline(uint8_t* fb, int x1, int x2, int y) {
    epd_fill_rect((EpdRect){x1, y, x2 - x1, 1}, 0x9c, fb);
}

// 像素心形（9x8，灰度 0x30）：替代 meta 行的「赞」字；墨水屏无彩色 emoji，位图最稳。
// / 9x8 pixel heart (gray 0x30) replaces the literal "likes" glyph; EPD has no color emoji.
static void popup_heart(uint8_t* fb, int x, int y) {
    static const char* art[8] = {
        ".XX...XX.", "XXXX.XXXX", "XXXXXXXXX", "XXXXXXXXX",
        ".XXXXXXX.", "..XXXXX..", "...XXX...", "....X....",
    };
    for (int r = 0; r < 8; ++r)
        for (int c = 0; c < 9; ++c)
            if (art[r][c] == 'X') epd_fill_rect((EpdRect){x + c, y + r, 1, 1}, 0x30, fb);
}

// meta 行：作者小字（弱化）居左，心形+点赞数右对齐行尾（参考机样式）；
// 点赞为 0 只画作者。想法正文才是主角。/ Muted small author left, heart+likes
// right-aligned at line end (reference layout); body text stays the lead.
static void popup_meta(uint8_t* fb, int x, int y, const char* author, unsigned likes) {
    char who[WEREAD_NOTE_AUTHOR_CAP + 4];
    snprintf(who, sizeof(who), "%s", author);
    popup_fit(who, sizeof(who), who, NOTES_META_PX, likes ? NOTES_LIST_W - 72 : NOTES_LIST_W);
    notes_draw_text(fb, x, y, NOTES_META_PX, who, EPD_DRAW_ALIGN_LEFT, false);
    if (!likes) return;
    const int eff = ui_text_effective_px(NOTES_META_PX);
    char num[12];
    snprintf(num, sizeof(num), "%u", likes);
    const int num_w = notes_text_width(eff, num);
    const int hx = x + NOTES_LIST_W - (num_w + 13 + 9);  // 心形+数字贴行尾 / flush right
    popup_heart(fb, hx, y + (eff - 8) / 2);
    notes_draw_text(fb, hx + 13, y, NOTES_META_PX, num, EPD_DRAW_ALIGN_LEFT, false);
}

// 按像素宽测量折行行数（与 popup_paragraph 同源折行算法，不绘制不改写文本）。
// / Count wrapped lines with the same algorithm as popup_paragraph; no drawing.
static int popup_measure(int width, int px, char* text, int max_lines) {
    const int eff = ui_text_effective_px(px);
    int drawn = 0;
    char* cursor = text;
    for (int row = 0; row < max_lines && cursor[0]; ++row) {
        size_t keep = strlen(cursor);
        while (keep) {
            const char saved = cursor[keep];
            cursor[keep] = 0;
            const int w = notes_text_width(eff, cursor);
            cursor[keep] = saved;
            if (w <= width) break;
            --keep;
            while (keep && ((unsigned char)cursor[keep] & 0xc0) == 0x80) --keep;
        }
        if (!keep) break;
        ++drawn;
        cursor += keep;
        while (*cursor == ' ') ++cursor;
    }
    return drawn;
}

void book_notes_render(uint8_t* fb) {
    if (!s_notes.open) return;
    // 居中浮层：圆角白底 + 细边，不再贴屏底（下方正文带可见可点）。
    // / Centered floating sheet with rounded corners; the body strip below stays live.
    EpdRect sheet = {NOTES_SHEET_X, NOTES_SHEET_TOP, NOTES_SHEET_W,
                     NOTES_SHEET_BOTTOM - NOTES_SHEET_TOP};
    ui_fill_round_rect(fb, sheet, 28, UI_GRAY_WHITE);
    ui_draw_round_rect(fb, sheet, 28, 0x58);
    ui_draw_round_rect(fb, (EpdRect){sheet.x + 1, sheet.y + 1, sheet.width - 2, sheet.height - 2}, 27, 0xa8);
    // 右上角关闭 X。/ Close cross in the top-right corner.
    notes_draw_text_vc(fb, popup_close_rect().x + 26, popup_close_rect().y + 26, 36,
               "×", EPD_DRAW_ALIGN_CENTER, false);
    char title[48];
    snprintf(title, sizeof(title), "想法 · %u 条", s_notes.count);
    notes_draw_text_vc(fb, NOTES_SHEET_X + NOTES_SHEET_W / 2, NOTES_SHEET_TOP + 48, 28,
               title, EPD_DRAW_ALIGN_CENTER, false);

    // 划线原文（引用句）最多两行，下方横线与想法列表分隔。
    // / The highlighted sentence (quote), up to two lines, then a divider.
    char hl[WEREAD_NOTE_TEXT_CAP + 8];
    snprintf(hl, sizeof(hl), "%s", s_notes.hit.text);
    popup_paragraph(fb, NOTES_LIST_X, NOTES_SHEET_TOP + 88, NOTES_LIST_W, NOTES_HL_PX, hl, 2);
    popup_hline(fb, NOTES_LIST_X, NOTES_LIST_X + NOTES_LIST_W, 284);
    // 想法列表（全文直出，无 2 行截断、无详情跳转）：每条 = 作者小字行（♥右对齐）
    // + 正文全文（几行排几行）。本页剩余高度装不下整条时整条移到下一页；首条就
    // 超过一页高的极端长文独占本页、末行省略号兜底。排到分页栏上沿为止，内容多
    // 则页满，末页自然留白，不固定条数；不足时现场续读游标。
    // / Thoughts are shown in full — no 2-line cap, no detail hop: author line
    // / (heart right) then the whole body. A thought that cannot fit the page
    // / remainder moves whole to the next page; one taller than an entire page
    // / takes the page alone with an ellipsis tail. Rows flow until the pager.
    const int body_step = ui_text_effective_px(NOTES_BODY_PX) + 10;
    const int meta_step = ui_text_effective_px(NOTES_META_PX);
    const int list_bottom = NOTES_PAGER_Y - 8;
    int y = NOTES_LIST_TOP;
    unsigned i = s_notes.page_start[s_notes.page];
    unsigned k = 0;
    while (i < s_notes.count && k < NOTES_MAX_ROWS) {
        if (i >= s_notes.loaded) {
            notes_load(i + 1);  // 渲染中续读。/ Top up mid-layout.
            if (i >= s_notes.loaded) break;  // 数据到底。/ End of data.
        }
        char body[WEREAD_NOTE_CONTENT_CAP + 8];
        snprintf(body, sizeof(body), "%s", s_notes.notes[i].content);
        int lines = popup_measure(NOTES_LIST_W, NOTES_BODY_PX, body, 64);  // 全文测量。/ Full measure.
        int divider = y + meta_step + 6 + lines * body_step + 8;
        if (divider > list_bottom) {
            if (k) break;  // 装不下整条 → 移下一页。/ Move whole to the next page.
            // 首条超页：独占本页，末行省略号。/ Taller than a page: page alone.
            lines = (list_bottom - y - meta_step - 14) / body_step;
            if (lines < 1) lines = 1;
            divider = y + meta_step + 6 + lines * body_step + 8;
        }
        popup_meta(fb, NOTES_LIST_X, y, s_notes.notes[i].author, s_notes.notes[i].likes);
        popup_paragraph(fb, NOTES_LIST_X, y + meta_step + 6, NOTES_LIST_W, NOTES_BODY_PX,
                        body, lines);
        popup_hline(fb, NOTES_LIST_X, NOTES_LIST_X + NOTES_LIST_W, divider);
        y = divider + 10;
        ++k;
        ++i;
    }
    s_notes.rows_used[s_notes.page] = k;
    if (k) {
        // 页数随排版逐步确定：排到数据尾即精确；否则按本页行数估算并只增不减。
        // / Page total firms up as layout proceeds: exact at the tail, else grown
        // / from this page's row count (never shrinking).
        const unsigned next_start = s_notes.page_start[s_notes.page] + k;
        if (next_start >= s_notes.count) {
            s_notes.pages = s_notes.page + 1;
        } else {
            const unsigned est = s_notes.page + 1 + (s_notes.count - next_start + k - 1) / k;
            if (est > s_notes.pages) s_notes.pages = est;
        }
    }
    // 底部分页栏：顶线 + 两竖线分成 上一页 / 页码 / 下一页 三格（撷思样式），
    // 不可用方向浅色显示。/ Pager bar split into three cells; disabled side is dimmed.
    popup_hline(fb, NOTES_LIST_X, NOTES_LIST_X + NOTES_LIST_W, NOTES_PAGER_Y);
    const int pager_mid_y = NOTES_PAGER_Y + NOTES_PAGER_H / 2;
    epd_fill_rect((EpdRect){NOTES_LIST_X + 196, NOTES_PAGER_Y + 12, 1, NOTES_PAGER_H - 24}, 0x9c, fb);
    epd_fill_rect((EpdRect){NOTES_LIST_X + NOTES_LIST_W - 196, NOTES_PAGER_Y + 12, 1, NOTES_PAGER_H - 24}, 0x9c, fb);
    notes_draw_text_vc(fb, NOTES_LIST_X + 98, pager_mid_y, 24,
               "‹ 上一页", EPD_DRAW_ALIGN_CENTER, false);
    char pager[24];
    snprintf(pager, sizeof(pager), "%u/%u", s_notes.page + 1, s_notes.pages);
    notes_draw_text_vc(fb, NOTES_SHEET_X + NOTES_SHEET_W / 2, pager_mid_y, 26,
               pager, EPD_DRAW_ALIGN_CENTER, false);
    notes_draw_text_vc(fb, NOTES_LIST_X + NOTES_LIST_W - 98, pager_mid_y, 24,
               "下一页 ›", EPD_DRAW_ALIGN_CENTER, false);
}
