#ifndef SIST2_CTX_H
#define SIST2_CTX_H

#include "sist.h"
#include "tpool.h"
#include "libscan/scan.h"
#include "libscan/arc/arc.h"
#include "libscan/comic/comic.h"
#include "libscan/ebook/ebook.h"
#include "libscan/font/font.h"
#include "libscan/media/media.h"
#include "libscan/ooxml/ooxml.h"
#include "libscan/text/text.h"
#include "libscan/mobi/scan_mobi.h"
#include "libscan/raw/raw.h"
#include "libscan/msdoc/msdoc.h"
#include "libscan/wpd/wpd.h"
#include "libscan/json/json.h"
#include "src/io/store.h"
#include "src/index/elastic.h"

#include <glib.h>
#include <pcre.h>

typedef struct {
    struct index_t index;

    GHashTable *mime_table;
    GHashTable *ext_table;

    tpool_t *pool;

    tpool_t *writer_pool;

    int threads;
    int depth;
    int calculate_checksums;
    size_t mem_limit;

    size_t stat_tn_size;
    size_t stat_index_size;

    GHashTable *original_table;
    GHashTable *copy_table;
    GHashTable *new_table;
    pthread_mutex_t copy_table_mu;

    pcre *exclude;
    pcre_extra *exclude_extra;
    int fast;

    GHashTable *dbg_current_files;
    pthread_mutex_t dbg_current_files_mu;

    int dbg_failed_files_count;
    int dbg_skipped_files_count;
    int dbg_excluded_files_count;
    pthread_mutex_t dbg_file_counts_mu;

    scan_arc_ctx_t arc_ctx;
    scan_comic_ctx_t comic_ctx;
    scan_ebook_ctx_t ebook_ctx;
    scan_font_ctx_t font_ctx;
    scan_media_ctx_t media_ctx;
    scan_ooxml_ctx_t ooxml_ctx;
    scan_text_ctx_t text_ctx;
    scan_mobi_ctx_t mobi_ctx;
    scan_raw_ctx_t raw_ctx;
    scan_msdoc_ctx_t msdoc_ctx;
    scan_wpd_ctx_t wpd_ctx;
    scan_json_ctx_t json_ctx;
} ScanCtx_t;

typedef struct {
    int verbose;
    int very_verbose;
    int no_color;
} LogCtx_t;

typedef struct {
    char *es_url;
    es_version_t *es_version;
    char *es_index;
    int batch_size;
    tpool_t *pool;
    store_t *tag_store;
    GHashTable *tags;
    store_t *meta_store;
    GHashTable *meta;
} IndexCtx_t;

typedef struct {
    char *es_url;
    es_version_t *es_version;
    char *es_index;
    int index_count;
    char *auth_user;
    char *auth_pass;
    int auth_enabled;
    int tag_auth_enabled;
    char *tagline;
    struct index_t indices[256];
    char lang[10];
    int dev;
} WebCtx_t;

extern ScanCtx_t ScanCtx;
extern WebCtx_t WebCtx;
extern IndexCtx_t IndexCtx;
extern LogCtx_t LogCtx;


#endif
