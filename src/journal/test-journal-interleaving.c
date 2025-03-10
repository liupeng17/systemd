/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <fcntl.h>
#include <unistd.h>

#include "sd-id128.h"
#include "sd-journal.h"

#include "alloc-util.h"
#include "chattr-util.h"
#include "io-util.h"
#include "journal-vacuum.h"
#include "log.h"
#include "managed-journal-file.h"
#include "parse-util.h"
#include "rm-rf.h"
#include "tests.h"

/* This program tests skipping around in a multi-file journal. */

static bool arg_keep = false;

_noreturn_ static void log_assert_errno(const char *text, int error, const char *file, unsigned line, const char *func) {
        log_internal(LOG_CRIT, error, file, line, func,
                     "'%s' failed at %s:%u (%s): %m", text, file, line, func);
        abort();
}

#define assert_ret(expr)                                                \
        do {                                                            \
                int _r_ = (expr);                                       \
                if (_unlikely_(_r_ < 0))                                \
                        log_assert_errno(#expr, -_r_, PROJECT_FILE, __LINE__, __func__); \
        } while (false)

static ManagedJournalFile *test_open(const char *name) {
        _cleanup_(mmap_cache_unrefp) MMapCache *m = NULL;
        ManagedJournalFile *f;

        m = mmap_cache_new();
        assert_se(m != NULL);

        assert_ret(managed_journal_file_open(-1, name, O_RDWR|O_CREAT, JOURNAL_COMPRESS, 0644, UINT64_MAX, NULL, m, NULL, NULL, &f));
        return f;
}

static void test_close(ManagedJournalFile *f) {
        (void) managed_journal_file_close(f);
}

static void append_number(ManagedJournalFile *f, int n, uint64_t *seqnum) {
        char *p;
        dual_timestamp ts;
        static dual_timestamp previous_ts = {};
        struct iovec iovec[1];

        dual_timestamp_get(&ts);

        if (ts.monotonic <= previous_ts.monotonic)
                ts.monotonic = previous_ts.monotonic + 1;

        if (ts.realtime <= previous_ts.realtime)
                ts.realtime = previous_ts.realtime + 1;

        previous_ts = ts;

        assert_se(asprintf(&p, "NUMBER=%d", n) >= 0);
        iovec[0] = IOVEC_MAKE_STRING(p);
        assert_ret(journal_file_append_entry(f->file, &ts, NULL, iovec, 1, seqnum, NULL, NULL, NULL));
        free(p);
}

static void test_check_number(sd_journal *j, int n) {
        const void *d;
        _cleanup_free_ char *k = NULL;
        size_t l;
        int x;

        assert_ret(sd_journal_get_data(j, "NUMBER", &d, &l));
        assert_se(k = strndup(d, l));
        printf("%s (expected=%i)\n", k, n);

        assert_se(safe_atoi(k + STRLEN("NUMBER="), &x) >= 0);
        assert_se(n == x);
}

static void test_check_numbers_down(sd_journal *j, int count) {
        int i;

        for (i = 1; i <= count; i++) {
                int r;
                test_check_number(j, i);
                assert_ret(r = sd_journal_next(j));
                if (i == count)
                        assert_se(r == 0);
                else
                        assert_se(r == 1);
        }

}

static void test_check_numbers_up(sd_journal *j, int count) {
        for (int i = count; i >= 1; i--) {
                int r;
                test_check_number(j, i);
                assert_ret(r = sd_journal_previous(j));
                if (i == 1)
                        assert_se(r == 0);
                else
                        assert_se(r == 1);
        }

}

static void setup_sequential(void) {
        ManagedJournalFile *f1, *f2, *f3;

        f1 = test_open("one.journal");
        f2 = test_open("two.journal");
        f3 = test_open("three.journal");
        append_number(f1, 1, NULL);
        append_number(f1, 2, NULL);
        append_number(f1, 3, NULL);
        append_number(f2, 4, NULL);
        append_number(f2, 5, NULL);
        append_number(f2, 6, NULL);
        append_number(f3, 7, NULL);
        append_number(f3, 8, NULL);
        append_number(f3, 9, NULL);
        test_close(f1);
        test_close(f2);
        test_close(f3);
}

static void setup_interleaved(void) {
        ManagedJournalFile *f1, *f2, *f3;

        f1 = test_open("one.journal");
        f2 = test_open("two.journal");
        f3 = test_open("three.journal");
        append_number(f1, 1, NULL);
        append_number(f2, 2, NULL);
        append_number(f3, 3, NULL);
        append_number(f1, 4, NULL);
        append_number(f2, 5, NULL);
        append_number(f3, 6, NULL);
        append_number(f1, 7, NULL);
        append_number(f2, 8, NULL);
        append_number(f3, 9, NULL);
        test_close(f1);
        test_close(f2);
        test_close(f3);
}

static void mkdtemp_chdir_chattr(char *path) {
        assert_se(mkdtemp(path));
        assert_se(chdir(path) >= 0);

        /* Speed up things a bit on btrfs, ensuring that CoW is turned off for all files created in our
         * directory during the test run */
        (void) chattr_path(path, FS_NOCOW_FL, FS_NOCOW_FL, NULL);
}

static void test_skip_one(void (*setup)(void)) {
        char t[] = "/var/tmp/journal-skip-XXXXXX";
        sd_journal *j;
        int r;

        mkdtemp_chdir_chattr(t);

        setup();

        /* Seek to head, iterate down. */
        assert_ret(sd_journal_open_directory(&j, t, 0));
        assert_ret(sd_journal_seek_head(j));
        assert_se(sd_journal_next(j) == 1);     /* pointing to the first entry */
        test_check_numbers_down(j, 9);
        sd_journal_close(j);

        /* Seek to head, iterate down. */
        assert_ret(sd_journal_open_directory(&j, t, 0));
        assert_ret(sd_journal_seek_head(j));
        assert_se(sd_journal_next(j) == 1);     /* pointing to the first entry */
        assert_se(sd_journal_previous(j) == 0); /* no-op */
        test_check_numbers_down(j, 9);
        sd_journal_close(j);

        /* Seek to head twice, iterate down. */
        assert_ret(sd_journal_open_directory(&j, t, 0));
        assert_ret(sd_journal_seek_head(j));
        assert_se(sd_journal_next(j) == 1);     /* pointing to the first entry */
        assert_ret(sd_journal_seek_head(j));
        assert_se(sd_journal_next(j) == 1);     /* pointing to the first entry */
        test_check_numbers_down(j, 9);
        sd_journal_close(j);

        /* Seek to head, move to previous, then iterate down. */
        assert_ret(sd_journal_open_directory(&j, t, 0));
        assert_ret(sd_journal_seek_head(j));
        assert_se(sd_journal_previous(j) == 0); /* no-op */
        assert_se(sd_journal_next(j) == 1);     /* pointing to the first entry */
        test_check_numbers_down(j, 9);
        sd_journal_close(j);

        /* Seek to head, walk several steps, then iterate down. */
        assert_ret(sd_journal_open_directory(&j, t, 0));
        assert_ret(sd_journal_seek_head(j));
        assert_se(sd_journal_previous(j) == 0); /* no-op */
        assert_se(sd_journal_previous(j) == 0); /* no-op */
        assert_se(sd_journal_previous(j) == 0); /* no-op */
        assert_se(sd_journal_next(j) == 1);     /* pointing to the first entry */
        assert_se(sd_journal_previous(j) == 0); /* no-op */
        assert_se(sd_journal_previous(j) == 0); /* no-op */
        test_check_numbers_down(j, 9);
        sd_journal_close(j);

        /* Seek to tail, iterate up. */
        assert_ret(sd_journal_open_directory(&j, t, 0));
        assert_ret(sd_journal_seek_tail(j));
        assert_se(sd_journal_previous(j) == 1); /* pointing to the last entry */
        test_check_numbers_up(j, 9);
        sd_journal_close(j);

        /* Seek to tail twice, iterate up. */
        assert_ret(sd_journal_open_directory(&j, t, 0));
        assert_ret(sd_journal_seek_tail(j));
        assert_se(sd_journal_previous(j) == 1); /* pointing to the last entry */
        assert_ret(sd_journal_seek_tail(j));
        assert_se(sd_journal_previous(j) == 1); /* pointing to the last entry */
        test_check_numbers_up(j, 9);
        sd_journal_close(j);

        /* Seek to tail, move to next, then iterate up. */
        assert_ret(sd_journal_open_directory(&j, t, 0));
        assert_ret(sd_journal_seek_tail(j));
        assert_se(sd_journal_next(j) == 0);     /* no-op */
        assert_se(sd_journal_previous(j) == 1); /* pointing to the last entry */
        test_check_numbers_up(j, 9);
        sd_journal_close(j);

        /* Seek to tail, walk several steps, then iterate up. */
        assert_ret(sd_journal_open_directory(&j, t, 0));
        assert_ret(sd_journal_seek_tail(j));
        assert_se(sd_journal_next(j) == 0);     /* no-op */
        assert_se(sd_journal_next(j) == 0);     /* no-op */
        assert_se(sd_journal_next(j) == 0);     /* no-op */
        assert_se(sd_journal_previous(j) == 1); /* pointing to the last entry. */
        assert_se(sd_journal_next(j) == 0);     /* no-op */
        assert_se(sd_journal_next(j) == 0);     /* no-op */
        test_check_numbers_up(j, 9);
        sd_journal_close(j);

        /* Seek to tail, skip to head, iterate down. */
        assert_ret(sd_journal_open_directory(&j, t, 0));
        assert_ret(sd_journal_seek_tail(j));
        assert_se(sd_journal_previous_skip(j, 9) == 9); /* pointing to the first entry. */
        test_check_numbers_down(j, 9);
        sd_journal_close(j);

        /* Seek to tail, skip to head in a more complex way, then iterate down. */
        assert_ret(sd_journal_open_directory(&j, t, 0));
        assert_ret(sd_journal_seek_tail(j));
        assert_se(sd_journal_next(j) == 0);
        assert_se(sd_journal_previous_skip(j, 4) == 4);
        assert_se(sd_journal_previous_skip(j, 5) == 5);
        assert_se(sd_journal_previous(j) == 0);
        assert_se(sd_journal_previous_skip(j, 5) == 0);
        assert_se(sd_journal_next(j) == 1);
        assert_se(sd_journal_previous_skip(j, 5) == 1);
        assert_se(sd_journal_next(j) == 1);
        assert_se(sd_journal_next(j) == 1);
        assert_se(sd_journal_previous(j) == 1);
        assert_se(sd_journal_next(j) == 1);
        assert_se(sd_journal_next(j) == 1);
        assert_se(sd_journal_previous_skip(j, 5) == 3);
        test_check_numbers_down(j, 9);
        sd_journal_close(j);

        /* Seek to head, skip to tail, iterate up. */
        assert_ret(sd_journal_open_directory(&j, t, 0));
        assert_ret(sd_journal_seek_head(j));
        assert_se(sd_journal_next_skip(j, 9) == 9);
        test_check_numbers_up(j, 9);
        sd_journal_close(j);

        /* Seek to head, skip to tail in a more complex way, then iterate up. */
        assert_ret(sd_journal_open_directory(&j, t, 0));
        assert_ret(sd_journal_seek_head(j));
        assert_se(sd_journal_previous(j) == 0);
        assert_se(sd_journal_next_skip(j, 4) == 4);
        assert_se(sd_journal_next_skip(j, 5) == 5);
        assert_se(sd_journal_next(j) == 0);
        assert_se(sd_journal_next_skip(j, 5) == 0);
        assert_se(sd_journal_previous(j) == 1);
        assert_se(sd_journal_next_skip(j, 5) == 1);
        assert_se(sd_journal_previous(j) == 1);
        assert_se(sd_journal_previous(j) == 1);
        assert_se(sd_journal_next(j) == 1);
        assert_se(sd_journal_previous(j) == 1);
        assert_se(sd_journal_previous(j) == 1);
        assert_se(r = sd_journal_next_skip(j, 5) == 3);
        test_check_numbers_up(j, 9);
        sd_journal_close(j);

        log_info("Done...");

        if (arg_keep)
                log_info("Not removing %s", t);
        else {
                journal_directory_vacuum(".", 3000000, 0, 0, NULL, true);

                assert_se(rm_rf(t, REMOVE_ROOT|REMOVE_PHYSICAL) >= 0);
        }

        puts("------------------------------------------------------------");
}

TEST(skip) {
        test_skip_one(setup_sequential);
        test_skip_one(setup_interleaved);
}

static void test_sequence_numbers_one(void) {
        _cleanup_(mmap_cache_unrefp) MMapCache *m = NULL;
        char t[] = "/var/tmp/journal-seq-XXXXXX";
        ManagedJournalFile *one, *two;
        uint64_t seqnum = 0;
        sd_id128_t seqnum_id;

        m = mmap_cache_new();
        assert_se(m != NULL);

        mkdtemp_chdir_chattr(t);

        assert_se(managed_journal_file_open(-1, "one.journal", O_RDWR|O_CREAT, JOURNAL_COMPRESS, 0644,
                                            UINT64_MAX, NULL, m, NULL, NULL, &one) == 0);

        append_number(one, 1, &seqnum);
        printf("seqnum=%"PRIu64"\n", seqnum);
        assert_se(seqnum == 1);
        append_number(one, 2, &seqnum);
        printf("seqnum=%"PRIu64"\n", seqnum);
        assert_se(seqnum == 2);

        assert_se(one->file->header->state == STATE_ONLINE);
        assert_se(!sd_id128_equal(one->file->header->file_id, one->file->header->machine_id));
        assert_se(!sd_id128_equal(one->file->header->file_id, one->file->header->tail_entry_boot_id));
        assert_se(sd_id128_equal(one->file->header->file_id, one->file->header->seqnum_id));

        memcpy(&seqnum_id, &one->file->header->seqnum_id, sizeof(sd_id128_t));

        assert_se(managed_journal_file_open(-1, "two.journal", O_RDWR|O_CREAT, JOURNAL_COMPRESS, 0644,
                                            UINT64_MAX, NULL, m, NULL, one, &two) == 0);

        assert_se(two->file->header->state == STATE_ONLINE);
        assert_se(!sd_id128_equal(two->file->header->file_id, one->file->header->file_id));
        assert_se(sd_id128_equal(one->file->header->machine_id, one->file->header->machine_id));
        assert_se(sd_id128_equal(one->file->header->tail_entry_boot_id, one->file->header->tail_entry_boot_id));
        assert_se(sd_id128_equal(one->file->header->seqnum_id, one->file->header->seqnum_id));

        append_number(two, 3, &seqnum);
        printf("seqnum=%"PRIu64"\n", seqnum);
        assert_se(seqnum == 3);
        append_number(two, 4, &seqnum);
        printf("seqnum=%"PRIu64"\n", seqnum);
        assert_se(seqnum == 4);

        test_close(two);

        append_number(one, 5, &seqnum);
        printf("seqnum=%"PRIu64"\n", seqnum);
        assert_se(seqnum == 5);

        append_number(one, 6, &seqnum);
        printf("seqnum=%"PRIu64"\n", seqnum);
        assert_se(seqnum == 6);

        test_close(one);

        /* If the machine-id is not initialized, the header file verification
         * (which happens when re-opening a journal file) will fail. */
        if (sd_id128_get_machine(NULL) >= 0) {
                /* restart server */
                seqnum = 0;

                assert_se(managed_journal_file_open(-1, "two.journal", O_RDWR, JOURNAL_COMPRESS, 0,
                                                    UINT64_MAX, NULL, m, NULL, NULL, &two) == 0);

                assert_se(sd_id128_equal(two->file->header->seqnum_id, seqnum_id));

                append_number(two, 7, &seqnum);
                printf("seqnum=%"PRIu64"\n", seqnum);
                assert_se(seqnum == 5);

                /* So..., here we have the same seqnum in two files with the
                 * same seqnum_id. */

                test_close(two);
        }

        log_info("Done...");

        if (arg_keep)
                log_info("Not removing %s", t);
        else {
                journal_directory_vacuum(".", 3000000, 0, 0, NULL, true);

                assert_se(rm_rf(t, REMOVE_ROOT|REMOVE_PHYSICAL) >= 0);
        }
}

TEST(sequence_numbers) {
        assert_se(setenv("SYSTEMD_JOURNAL_COMPACT", "0", 1) >= 0);
        test_sequence_numbers_one();

        assert_se(setenv("SYSTEMD_JOURNAL_COMPACT", "1", 1) >= 0);
        test_sequence_numbers_one();
}

static int intro(void) {
        /* managed_journal_file_open requires a valid machine id */
        if (access("/etc/machine-id", F_OK) != 0)
                return log_tests_skipped("/etc/machine-id not found");

        arg_keep = saved_argc > 1;

        return EXIT_SUCCESS;
}

DEFINE_TEST_MAIN_WITH_INTRO(LOG_DEBUG, intro);
