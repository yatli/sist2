#include "sist.h"
#include "ctx.h"

#include <third-party/argparse/argparse.h>
#include <locale.h>

#include "cli.h"
#include "io/serialize.h"
#include "io/store.h"
#include "tpool.h"
#include "io/walk.h"
#include "index/elastic.h"
#include "web/serve.h"
#include "parsing/mime.h"
#include "parsing/parse.h"

#include <signal.h>
#include <unistd.h>

#include "stats.h"

#define DESCRIPTION "Lightning-fast file system indexer and search tool."

#define EPILOG "Made by simon987 <me@simon987.net>. Released under GPL-3.0"


static const char *const usage[] = {
        "sist2 scan [OPTION]... PATH",
        "sist2 index [OPTION]... INDEX",
        "sist2 web [OPTION]... INDEX...",
        "sist2 exec-script [OPTION]... INDEX",
        NULL,
};


static __sighandler_t sigsegv_handler = NULL;
static __sighandler_t sigabrt_handler = NULL;

void sig_handler(int signum) {

    LogCtx.verbose = 1;
    LogCtx.very_verbose = 1;

    LOG_ERROR("*SIGNAL HANDLER*", "=============================================\n\n");
    LOG_ERRORF("*SIGNAL HANDLER*", "Uh oh! Caught fatal signal: %s", strsignal(signum));

    if (ScanCtx.dbg_current_files != NULL) {
        GHashTableIter iter;
        g_hash_table_iter_init(&iter, ScanCtx.dbg_current_files);

        void *key;
        void *value;
        while (g_hash_table_iter_next(&iter, &key, &value)) {
            parse_job_t *job = value;

            if (isatty(STDERR_FILENO)) {
                LOG_DEBUGF(
                        "*SIGNAL HANDLER*",
                        "Thread \033[%dm[%04llX]\033[0m was working on job '%s'",
                        31 + ((unsigned int) key) % 7, key, job->filepath
                );
            } else {
                LOG_DEBUGF(
                        "*SIGNAL HANDLER*",
                        "THREAD [%04llX] was working on job %s",
                        key, job->filepath
                );
            }
        }
    }

    if (ScanCtx.pool != NULL) {
        tpool_dump_debug_info(ScanCtx.pool);
    }

    if (IndexCtx.pool != NULL) {
        tpool_dump_debug_info(IndexCtx.pool);
    }

    LOG_INFO(
            "*SIGNAL HANDLER*",
            "Please consider creating a bug report at https://github.com/simon987/sist2/issues !"
    )
    LOG_INFO(
            "*SIGNAL HANDLER*",
            "sist2 is an open source project and relies on the collaboration of its users to diagnose and fix bugs"
    )

#ifndef SIST_DEBUG
    LOG_WARNING(
            "*SIGNAL HANDLER*",
            "You are running sist2 in release mode! Please consider downloading the debug binary from the Github "
            "releases page to provide additionnal information when submitting a bug report."
    )
#endif

    if (signum == SIGSEGV && sigsegv_handler != NULL) {
        sigsegv_handler(signum);
    } else if (signum == SIGABRT && sigabrt_handler != NULL) {
        sigabrt_handler(signum);
    }

    exit(-1);
}

void init_dir(const char *dirpath) {
    char path[PATH_MAX];
    snprintf(path, PATH_MAX, "%sdescriptor.json", dirpath);

    time(&ScanCtx.index.desc.timestamp);
    strcpy(ScanCtx.index.desc.version, Version);
    strcpy(ScanCtx.index.desc.type, INDEX_TYPE_NDJSON);

    unsigned char index_md5[MD5_DIGEST_LENGTH];
    MD5((unsigned char *) &ScanCtx.index.desc.timestamp, sizeof(ScanCtx.index.desc.timestamp), index_md5);
    buf2hex(index_md5, MD5_DIGEST_LENGTH, ScanCtx.index.desc.id);

    write_index_descriptor(path, &ScanCtx.index.desc);
}

void scan_print_header() {
    LOG_INFOF("main.c", "sist2 v%s", Version)
}

void _store(char *key, size_t key_len, char *buf, size_t buf_len) {
    store_write(ScanCtx.index.store, key, key_len, buf, buf_len);
}

void _log(const char *filepath, int level, char *str) {
    if (level == LEVEL_FATAL) {
        sist_log(filepath, level, str);
        exit(-1);
    }

    if (LogCtx.verbose) {
        if (level == LEVEL_DEBUG) {
            if (LogCtx.very_verbose) {
                sist_log(filepath, level, str);
            }
        } else {
            sist_log(filepath, level, str);
        }
    }
}

void _logf(const char *filepath, int level, char *format, ...) {

    va_list args;

    va_start(args, format);
    if (level == LEVEL_FATAL) {
        vsist_logf(filepath, level, format, args);
        exit(-1);
    }

    if (LogCtx.verbose) {
        if (level == LEVEL_DEBUG) {
            if (LogCtx.very_verbose) {
                vsist_logf(filepath, level, format, args);
            }
        } else {
            vsist_logf(filepath, level, format, args);
        }
    }
    va_end(args);
}

void initialize_scan_context(scan_args_t *args) {

    ScanCtx.dbg_current_files = g_hash_table_new_full(g_int64_hash, g_int64_equal, NULL, NULL);
    pthread_mutex_init(&ScanCtx.dbg_current_files_mu, NULL);
    pthread_mutex_init(&ScanCtx.dbg_file_counts_mu, NULL);
    pthread_mutex_init(&ScanCtx.copy_table_mu, NULL);

    ScanCtx.calculate_checksums = args->calculate_checksums;

    // Archive
    ScanCtx.arc_ctx.mode = args->archive_mode;
    ScanCtx.arc_ctx.log = _log;
    ScanCtx.arc_ctx.logf = _logf;
    ScanCtx.arc_ctx.parse = (parse_callback_t) parse;
    if (args->archive_passphrase != NULL) {
        strcpy(ScanCtx.arc_ctx.passphrase, args->archive_passphrase);
    } else {
        ScanCtx.arc_ctx.passphrase[0] = 0;
    }

    // Comic
    ScanCtx.comic_ctx.log = _log;
    ScanCtx.comic_ctx.logf = _logf;
    ScanCtx.comic_ctx.store = _store;
    ScanCtx.comic_ctx.tn_size = args->size;
    ScanCtx.comic_ctx.tn_qscale = args->quality;
    ScanCtx.comic_ctx.cbr_mime = mime_get_mime_by_string(ScanCtx.mime_table, "application/x-cbr");
    ScanCtx.comic_ctx.cbz_mime = mime_get_mime_by_string(ScanCtx.mime_table, "application/x-cbz");

    // Ebook
    pthread_mutex_init(&ScanCtx.ebook_ctx.mupdf_mutex, NULL);
    ScanCtx.ebook_ctx.content_size = args->content_size;
    ScanCtx.ebook_ctx.tn_size = args->size;
    ScanCtx.ebook_ctx.tesseract_lang = args->tesseract_lang;
    ScanCtx.ebook_ctx.tesseract_path = args->tesseract_path;
    ScanCtx.ebook_ctx.log = _log;
    ScanCtx.ebook_ctx.logf = _logf;
    ScanCtx.ebook_ctx.store = _store;
    ScanCtx.ebook_ctx.fast_epub_parse = args->fast_epub;
    ScanCtx.ebook_ctx.tn_qscale = args->quality;

    // Font
    ScanCtx.font_ctx.enable_tn = args->size > 0;
    ScanCtx.font_ctx.log = _log;
    ScanCtx.font_ctx.logf = _logf;
    ScanCtx.font_ctx.store = _store;

    // Media
    ScanCtx.media_ctx.tn_qscale = args->quality;
    ScanCtx.media_ctx.tn_size = args->size;
    ScanCtx.media_ctx.log = _log;
    ScanCtx.media_ctx.logf = _logf;
    ScanCtx.media_ctx.store = _store;
    ScanCtx.media_ctx.max_media_buffer = (long) args->max_memory_buffer * 1024 * 1024;
    ScanCtx.media_ctx.read_subtitles = args->read_subtitles;

    if (args->ocr_images) {
        ScanCtx.media_ctx.tesseract_lang = args->tesseract_lang;
        ScanCtx.media_ctx.tesseract_path = args->tesseract_path;
    }
    init_media();

    // OOXML
    ScanCtx.ooxml_ctx.content_size = args->content_size;
    ScanCtx.ooxml_ctx.log = _log;
    ScanCtx.ooxml_ctx.logf = _logf;
    ScanCtx.ooxml_ctx.store = _store;

    // MOBI
    ScanCtx.mobi_ctx.content_size = args->content_size;
    ScanCtx.mobi_ctx.log = _log;
    ScanCtx.mobi_ctx.logf = _logf;

    // TEXT
    ScanCtx.text_ctx.content_size = args->content_size;
    ScanCtx.text_ctx.log = _log;
    ScanCtx.text_ctx.logf = _logf;

    // MSDOC
    ScanCtx.msdoc_ctx.tn_size = args->size;
    ScanCtx.msdoc_ctx.content_size = args->content_size;
    ScanCtx.msdoc_ctx.log = _log;
    ScanCtx.msdoc_ctx.logf = _logf;
    ScanCtx.msdoc_ctx.store = _store;
    ScanCtx.msdoc_ctx.msdoc_mime = mime_get_mime_by_string(ScanCtx.mime_table, "application/msword");

    ScanCtx.threads = args->threads;
    ScanCtx.depth = args->depth;

    strncpy(ScanCtx.index.path, args->output, sizeof(ScanCtx.index.path));
    strncpy(ScanCtx.index.desc.name, args->name, sizeof(ScanCtx.index.desc.name));
    strncpy(ScanCtx.index.desc.root, args->path, sizeof(ScanCtx.index.desc.root));
    strncpy(ScanCtx.index.desc.rewrite_url, args->rewrite_url, sizeof(ScanCtx.index.desc.rewrite_url));
    ScanCtx.index.desc.root_len = (short) strlen(ScanCtx.index.desc.root);
    ScanCtx.fast = args->fast;

    // Raw
    ScanCtx.raw_ctx.tn_qscale = args->quality;
    ScanCtx.raw_ctx.tn_size = args->size;
    ScanCtx.raw_ctx.log = _log;
    ScanCtx.raw_ctx.logf = _logf;
    ScanCtx.raw_ctx.store = _store;

    // Wpd
    ScanCtx.wpd_ctx.content_size = args->content_size;
    ScanCtx.wpd_ctx.log = _log;
    ScanCtx.wpd_ctx.logf = _logf;
    ScanCtx.wpd_ctx.wpd_mime = mime_get_mime_by_string(ScanCtx.mime_table, "application/wordperfect");

    // Json
    ScanCtx.json_ctx.content_size = args->content_size;
    ScanCtx.json_ctx.log = _log;
    ScanCtx.json_ctx.logf = _logf;
    ScanCtx.json_ctx.json_mime = mime_get_mime_by_string(ScanCtx.mime_table, "application/json");
    ScanCtx.json_ctx.ndjson_mime = mime_get_mime_by_string(ScanCtx.mime_table, "application/ndjson");
}


void load_incremental_index(const scan_args_t *args) {
    ScanCtx.original_table = incremental_get_table();
    ScanCtx.copy_table = incremental_get_table();

    DIR *dir = opendir(args->incremental);
    if (dir == NULL) {
        LOG_FATALF("main.c", "Could not open original index for incremental scan: %s", strerror(errno))
    }

    char descriptor_path[PATH_MAX];
    snprintf(descriptor_path, PATH_MAX, "%s/descriptor.json", args->incremental);
    index_descriptor_t original_desc = read_index_descriptor(descriptor_path);

    if (strcmp(original_desc.version, Version) != 0) {
        LOG_FATALF("main.c", "Version mismatch! Index is %s but executable is %s", original_desc.version, Version)
    }

    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (strncmp(de->d_name, "_index", sizeof("_index") - 1) == 0) {
            char file_path[PATH_MAX];
            snprintf(file_path, PATH_MAX, "%s%s", args->incremental, de->d_name);
            incremental_read(ScanCtx.original_table, file_path, &original_desc);
        }
    }
    closedir(dir);

    LOG_INFOF("main.c", "Loaded %d items in to mtime table.", g_hash_table_size(ScanCtx.original_table))
}

void sist2_scan(scan_args_t *args) {

    ScanCtx.mime_table = mime_get_mime_table();
    ScanCtx.ext_table = mime_get_ext_table();

    initialize_scan_context(args);

    init_dir(ScanCtx.index.path);

    char store_path[PATH_MAX];
    snprintf(store_path, PATH_MAX, "%sthumbs", ScanCtx.index.path);
    ScanCtx.index.store = store_create(store_path, STORE_SIZE_TN);

    snprintf(store_path, PATH_MAX, "%smeta", ScanCtx.index.path);
    ScanCtx.index.meta_store = store_create(store_path, STORE_SIZE_META);

    scan_print_header();

    if (args->incremental != NULL) {
        load_incremental_index(args);
    }

    ScanCtx.pool = tpool_create(args->threads, thread_cleanup, TRUE, TRUE);
    tpool_start(ScanCtx.pool);

    ScanCtx.writer_pool = tpool_create(1, writer_cleanup, TRUE, FALSE);
    tpool_start(ScanCtx.writer_pool);

    if (args->list_path) {
        // Scan using file list
        int list_ret = iterate_file_list(args->list_file);
        if (list_ret != 0) {
            LOG_FATALF("main.c", "iterate_file_list() failed! (%d)", list_ret)
        }
    } else {
        // Scan directory recursively
        int walk_ret = walk_directory_tree(ScanCtx.index.desc.root);
        if (walk_ret == -1) {
            LOG_FATALF("main.c", "walk_directory_tree() failed! %s (%d)", strerror(errno), errno)
        }
    }

    tpool_wait(ScanCtx.pool);
    tpool_destroy(ScanCtx.pool);

    tpool_wait(ScanCtx.writer_pool);
    tpool_destroy(ScanCtx.writer_pool);

    LOG_DEBUGF("main.c", "Skipped files: %d", ScanCtx.dbg_skipped_files_count)
    LOG_DEBUGF("main.c", "Excluded files: %d", ScanCtx.dbg_excluded_files_count)
    LOG_DEBUGF("main.c", "Failed files: %d", ScanCtx.dbg_failed_files_count)

    if (args->incremental != NULL) {
        char dst_path[PATH_MAX];
        snprintf(store_path, PATH_MAX, "%sthumbs", args->incremental);
        snprintf(dst_path, PATH_MAX, "%s_index_original.ndjson.zst", ScanCtx.index.path);
        store_t *source = store_create(store_path, STORE_SIZE_TN);

        DIR *dir = opendir(args->incremental);
        if (dir == NULL) {
            perror("opendir");
            return;
        }
        struct dirent *de;
        while ((de = readdir(dir)) != NULL) {
            if (strncmp(de->d_name, "_index_", sizeof("_index_") - 1) == 0) {
                char file_path[PATH_MAX];
                snprintf(file_path, PATH_MAX, "%s%s", args->incremental, de->d_name);
                incremental_copy(source, ScanCtx.index.store, file_path, dst_path, ScanCtx.copy_table);
            }
        }
        closedir(dir);
        store_destroy(source);
        writer_cleanup();

        snprintf(store_path, PATH_MAX, "%stags", args->incremental);
        snprintf(dst_path, PATH_MAX, "%stags", ScanCtx.index.path);
        store_t *source_tags = store_create(store_path, STORE_SIZE_TAG);
        store_copy(source_tags, dst_path);
        store_destroy(source_tags);
    }

    generate_stats(&ScanCtx.index, args->treemap_threshold, ScanCtx.index.path);

    store_destroy(ScanCtx.index.store);
    store_destroy(ScanCtx.index.meta_store);
}

void sist2_index(index_args_t *args) {

    IndexCtx.es_url = args->es_url;
    IndexCtx.es_index = args->es_index;
    IndexCtx.batch_size = args->batch_size;

    if (!args->print) {
        elastic_init(args->force_reset, args->es_mappings, args->es_settings);
    }

    char descriptor_path[PATH_MAX];
    snprintf(descriptor_path, PATH_MAX, "%s/descriptor.json", args->index_path);

    index_descriptor_t desc = read_index_descriptor(descriptor_path);

    LOG_DEBUGF("main.c", "descriptor version %s (%s)", desc.version, desc.type)

    if (strcmp(desc.version, Version) != 0) {
        LOG_FATALF("main.c", "Version mismatch! Index is %s but executable is %s", desc.version, Version)
    }

    DIR *dir = opendir(args->index_path);
    if (dir == NULL) {
        LOG_FATALF("main.c", "Could not open index %s: %s", args->index_path, strerror(errno))
    }

    char path_tmp[PATH_MAX];
    snprintf(path_tmp, sizeof(path_tmp), "%s/tags", args->index_path);
    IndexCtx.tag_store = store_create(path_tmp, STORE_SIZE_TAG);
    IndexCtx.tags = store_read_all(IndexCtx.tag_store);

    snprintf(path_tmp, sizeof(path_tmp), "%s/meta", args->index_path);
    IndexCtx.meta_store = store_create(path_tmp, STORE_SIZE_META);
    IndexCtx.meta = store_read_all(IndexCtx.meta_store);

    index_func f;
    if (args->print) {
        f = print_json;
    } else {
        f = index_json;
    }

    void (*cleanup)();
    if (args->print) {
        cleanup = NULL;
    } else {
        cleanup = elastic_cleanup;
    }

    IndexCtx.pool = tpool_create(args->threads, cleanup, FALSE, args->print == 0);
    tpool_start(IndexCtx.pool);

    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (strncmp(de->d_name, "_index_", sizeof("_index_") - 1) == 0) {
            char file_path[PATH_MAX];
            snprintf(file_path, PATH_MAX, "%s/%s", args->index_path, de->d_name);
            read_index(file_path, desc.id, desc.type, f);
            LOG_DEBUGF("main.c", "Read index file %s (%s)", file_path, desc.type)
        }
    }
    closedir(dir);

    tpool_wait(IndexCtx.pool);

    tpool_destroy(IndexCtx.pool);

    if (!args->print) {
        finish_indexer(args->script, args->async_script, desc.id);
    }

    store_destroy(IndexCtx.tag_store);
    store_destroy(IndexCtx.meta_store);
    g_hash_table_remove_all(IndexCtx.tags);
    g_hash_table_destroy(IndexCtx.tags);
}

void sist2_exec_script(exec_args_t *args) {

    LogCtx.verbose = TRUE;

    char descriptor_path[PATH_MAX];
    snprintf(descriptor_path, PATH_MAX, "%s/descriptor.json", args->index_path);
    index_descriptor_t desc = read_index_descriptor(descriptor_path);

    IndexCtx.es_url = args->es_url;
    IndexCtx.es_index = args->es_index;

    LOG_DEBUGF("main.c", "descriptor version %s (%s)", desc.version, desc.type)

    execute_update_script(args->script, args->async_script, desc.id);
    free(args->script);
}

void sist2_web(web_args_t *args) {

    WebCtx.es_url = args->es_url;
    WebCtx.es_index = args->es_index;
    WebCtx.index_count = args->index_count;
    WebCtx.auth_user = args->auth_user;
    WebCtx.auth_pass = args->auth_pass;
    WebCtx.auth_enabled = args->auth_enabled;
    WebCtx.tag_auth_enabled = args->tag_auth_enabled;
    WebCtx.tagline = args->tagline;
    WebCtx.dev = args->dev;
    strcpy(WebCtx.lang, args->lang);

    for (int i = 0; i < args->index_count; i++) {
        char *abs_path = abspath(args->indices[i]);
        if (abs_path == NULL) {
            return;
        }
        char path_tmp[PATH_MAX];

        snprintf(path_tmp, PATH_MAX, "%sthumbs", abs_path);
        WebCtx.indices[i].store = store_create(path_tmp, STORE_SIZE_TN);

        snprintf(path_tmp, PATH_MAX, "%stags", abs_path);
        mkdir(path_tmp, S_IWUSR | S_IRUSR | S_IXUSR);
        WebCtx.indices[i].tag_store = store_create(path_tmp, STORE_SIZE_TAG);

        snprintf(path_tmp, PATH_MAX, "%sdescriptor.json", abs_path);
        WebCtx.indices[i].desc = read_index_descriptor(path_tmp);

        strcpy(WebCtx.indices[i].path, abs_path);
        printf("Loaded index: %s\n", WebCtx.indices[i].desc.name);
        free(abs_path);
    }

    serve(args->listen_address);
}


int main(int argc, const char *argv[]) {
    sigsegv_handler = signal(SIGSEGV, sig_handler);
    sigabrt_handler = signal(SIGABRT, sig_handler);

    setlocale(LC_ALL, "");

    scan_args_t *scan_args = scan_args_create();
    index_args_t *index_args = index_args_create();
    web_args_t *web_args = web_args_create();
    exec_args_t *exec_args = exec_args_create();

    int arg_version = 0;

    char *common_es_url = NULL;
    char *common_es_index = NULL;
    char *common_script_path = NULL;
    int common_async_script = 0;
    int common_threads = 0;

    struct argparse_option options[] = {
            OPT_HELP(),

            OPT_BOOLEAN('v', "version", &arg_version, "Show version and exit"),
            OPT_BOOLEAN(0, "verbose", &LogCtx.verbose, "Turn on logging"),
            OPT_BOOLEAN(0, "very-verbose", &LogCtx.very_verbose, "Turn on debug messages"),

            OPT_GROUP("Scan options"),
            OPT_INTEGER('t', "threads", &common_threads, "Number of threads. DEFAULT=1"),
            OPT_FLOAT('q', "quality", &scan_args->quality,
                      "Thumbnail quality, on a scale of 1.0 to 31.0, 1.0 being the best. DEFAULT=3"),
            OPT_INTEGER(0, "size", &scan_args->size,
                        "Thumbnail size, in pixels. Use negative value to disable. DEFAULT=500"),
            OPT_INTEGER(0, "content-size", &scan_args->content_size,
                        "Number of bytes to be extracted from text documents. Use negative value to disable. DEFAULT=32768"),
            OPT_STRING(0, "incremental", &scan_args->incremental,
                       "Reuse an existing index and only scan modified files."),
            OPT_STRING('o', "output", &scan_args->output, "Output directory. DEFAULT=index.sist2/"),
            OPT_STRING(0, "rewrite-url", &scan_args->rewrite_url, "Serve files from this url instead of from disk."),
            OPT_STRING(0, "name", &scan_args->name, "Index display name. DEFAULT: (name of the directory)"),
            OPT_INTEGER(0, "depth", &scan_args->depth, "Scan up to DEPTH subdirectories deep. "
                                                       "Use 0 to only scan files in PATH. DEFAULT: -1"),
            OPT_STRING(0, "archive", &scan_args->archive, "Archive file mode (skip|list|shallow|recurse). "
                                                          "skip: Don't parse, list: only get file names as text, "
                                                          "shallow: Don't parse archives inside archives. DEFAULT: recurse"),
            OPT_STRING(0, "archive-passphrase", &scan_args->archive_passphrase,
                       "Passphrase for encrypted archive files"),

            OPT_STRING(0, "ocr-lang", &scan_args->tesseract_lang,
                       "Tesseract language (use 'tesseract --list-langs' to see "
                       "which are installed on your machine)"),
            OPT_BOOLEAN(0, "ocr-images", &scan_args->ocr_images, "Enable OCR'ing of image files."),
            OPT_BOOLEAN(0, "ocr-ebooks", &scan_args->ocr_ebooks, "Enable OCR'ing of ebook files."),
            OPT_STRING('e', "exclude", &scan_args->exclude_regex, "Files that match this regex will not be scanned"),
            OPT_BOOLEAN(0, "fast", &scan_args->fast, "Only index file names & mime type"),
            OPT_STRING(0, "treemap-threshold", &scan_args->treemap_threshold_str, "Relative size threshold for treemap "
                                                                                  "(see USAGE.md). DEFAULT: 0.0005"),
            OPT_INTEGER(0, "mem-buffer", &scan_args->max_memory_buffer,
                        "Maximum memory buffer size per thread in MB for files inside archives "
                        "(see USAGE.md). DEFAULT: 2000"),
            OPT_BOOLEAN(0, "read-subtitles", &scan_args->read_subtitles, "Read subtitles from media files."),
            OPT_BOOLEAN(0, "fast-epub", &scan_args->fast_epub,
                        "Faster but less accurate EPUB parsing (no thumbnails, metadata)"),
            OPT_BOOLEAN(0, "checksums", &scan_args->calculate_checksums, "Calculate file checksums when scanning."),
            OPT_STRING(0, "list-file", &scan_args->list_path, "Specify a list of newline-delimited paths to be scanned"
                                                              " instead of normal directory traversal. Use '-' to read"
                                                              " from stdin."),

            OPT_GROUP("Index options"),
            OPT_INTEGER('t', "threads", &common_threads, "Number of threads. DEFAULT=1"),
            OPT_STRING(0, "es-url", &common_es_url, "Elasticsearch url with port. DEFAULT=http://localhost:9200"),
            OPT_STRING(0, "es-index", &common_es_index, "Elasticsearch index name. DEFAULT=sist2"),
            OPT_BOOLEAN('p', "print", &index_args->print, "Just print JSON documents to stdout."),
            OPT_STRING(0, "script-file", &common_script_path, "Path to user script."),
            OPT_STRING(0, "mappings-file", &index_args->es_mappings_path, "Path to Elasticsearch mappings."),
            OPT_STRING(0, "settings-file", &index_args->es_settings_path, "Path to Elasticsearch settings."),
            OPT_BOOLEAN(0, "async-script", &common_async_script, "Execute user script asynchronously."),
            OPT_INTEGER(0, "batch-size", &index_args->batch_size, "Index batch size. DEFAULT: 100"),
            OPT_BOOLEAN('f', "force-reset", &index_args->force_reset, "Reset Elasticsearch mappings and settings. "
                                                                      "(You must use this option the first time you use the index command)"),

            OPT_GROUP("Web options"),
            OPT_STRING(0, "es-url", &common_es_url, "Elasticsearch url. DEFAULT=http://localhost:9200"),
            OPT_STRING(0, "es-index", &common_es_index, "Elasticsearch index name. DEFAULT=sist2"),
            OPT_STRING(0, "bind", &web_args->listen_address, "Listen on this address. DEFAULT=localhost:4090"),
            OPT_STRING(0, "auth", &web_args->credentials, "Basic auth in user:password format"),
            OPT_STRING(0, "tag-auth", &web_args->tag_credentials, "Basic auth in user:password format for tagging"),
            OPT_STRING(0, "tagline", &web_args->tagline, "Tagline in navbar"),
            OPT_BOOLEAN(0, "dev", &web_args->dev, "Serve html & js files from disk (for development)"),
            OPT_STRING(0, "lang", &web_args->lang, "Default UI language. Can be changed by the user"),

            OPT_GROUP("Exec-script options"),
            OPT_STRING(0, "es-url", &common_es_url, "Elasticsearch url. DEFAULT=http://localhost:9200"),
            OPT_STRING(0, "es-index", &common_es_index, "Elasticsearch index name. DEFAULT=sist2"),
            OPT_STRING(0, "script-file", &common_script_path, "Path to user script."),
            OPT_BOOLEAN(0, "async-script", &common_async_script, "Execute user script asynchronously."),

            OPT_END(),
    };

    struct argparse argparse;
    argparse_init(&argparse, options, usage, 0);
    argparse_describe(&argparse, DESCRIPTION, EPILOG);
    argc = argparse_parse(&argparse, argc, argv);

    if (arg_version) {
        printf(Version);
        goto end;
    }

    if (LogCtx.very_verbose != 0) {
        LogCtx.verbose = 1;
    }

    web_args->es_url = common_es_url;
    index_args->es_url = common_es_url;
    exec_args->es_url = common_es_url;

    web_args->es_index = common_es_index;
    index_args->es_index = common_es_index;
    exec_args->es_index = common_es_index;

    index_args->script_path = common_script_path;
    exec_args->script_path = common_script_path;
    index_args->threads = common_threads;
    scan_args->threads = common_threads;
    exec_args->async_script = common_async_script;
    index_args->async_script = common_async_script;

    if (argc == 0) {
        argparse_usage(&argparse);
        goto end;
    } else if (strcmp(argv[0], "scan") == 0) {

        int err = scan_args_validate(scan_args, argc, argv);
        if (err != 0) {
            goto end;
        }
        sist2_scan(scan_args);

    } else if (strcmp(argv[0], "index") == 0) {

        int err = index_args_validate(index_args, argc, argv);
        if (err != 0) {
            goto end;
        }
        sist2_index(index_args);

    } else if (strcmp(argv[0], "web") == 0) {

        int err = web_args_validate(web_args, argc, argv);
        if (err != 0) {
            goto end;
        }
        sist2_web(web_args);

    } else if (strcmp(argv[0], "exec-script") == 0) {

        int err = exec_args_validate(exec_args, argc, argv);
        if (err != 0) {
            goto end;
        }
        sist2_exec_script(exec_args);

    } else {
        fprintf(stderr, "Invalid command: '%s'\n", argv[0]);
        argparse_usage(&argparse);
        goto end;
    }
    printf("\n");

    end:
    scan_args_destroy(scan_args);
    index_args_destroy(index_args);
    web_args_destroy(web_args);
    exec_args_destroy(exec_args);

    return 0;
}
