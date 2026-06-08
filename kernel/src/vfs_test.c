#include "vfs.h"
#include "str.h"
#include "uart.h"
void test_vfs_basic1_more(void) {
    struct file* f = 0;
    struct file* f1 = 0;
    struct file* f2 = 0;
    char buf[32];

    // 1. Missing file without O_CREAT should fail.
    if (vfs_open("/no_such_file", 0, &f) == 0) {
        uart_puts("[FAIL] opened missing file without O_CREAT\n");
        vfs_close(f);
        return;
    }

    // 2. Create file and write data.
    if (vfs_open("/alpha", O_CREAT, &f) != 0) {
        uart_puts("[FAIL] create /alpha\n");
        return;
    }

    if (vfs_write(f, "abcdef", 6) != 6) {
        uart_puts("[FAIL] write /alpha\n");
        vfs_close(f);
        return;
    }

    vfs_close(f);

    // 3. Reopen and read first 3 bytes.
    memset(buf, 0, sizeof(buf));

    if (vfs_open("/alpha", 0, &f) != 0) {
        uart_puts("[FAIL] reopen /alpha\n");
        return;
    }

    if (vfs_read(f, buf, 3) != 3) {
        uart_puts("[FAIL] read first 3 bytes\n");
        vfs_close(f);
        return;
    }

    if (strcmp(buf, "abc") != 0) {
        uart_puts("[FAIL] first read data mismatch\n");
        vfs_close(f);
        return;
    }

    // 4. Read next 3 bytes; this checks f_pos advanced.
    memset(buf, 0, sizeof(buf));

    if (vfs_read(f, buf, 3) != 3) {
        uart_puts("[FAIL] read next 3 bytes\n");
        vfs_close(f);
        return;
    }

    if (strcmp(buf, "def") != 0) {
        uart_puts("[FAIL] second read data mismatch\n");
        vfs_close(f);
        return;
    }

    // 5. EOF should return 0.
    memset(buf, 0, sizeof(buf));

    if (vfs_read(f, buf, 3) != 0) {
        uart_puts("[FAIL] EOF read should return 0\n");
        vfs_close(f);
        return;
    }

    vfs_close(f);

    // 6. Reopening should reset f_pos to 0.
    memset(buf, 0, sizeof(buf));

    if (vfs_open("/alpha", 0, &f) != 0) {
        uart_puts("[FAIL] reopen /alpha second time\n");
        return;
    }

    if (vfs_read(f, buf, 3) != 3) {
        uart_puts("[FAIL] read after reopen\n");
        vfs_close(f);
        return;
    }

    if (strcmp(buf, "abc") != 0) {
        uart_puts("[FAIL] reopen did not reset f_pos\n");
        vfs_close(f);
        return;
    }

    vfs_close(f);

    // 7. Two file handles should have independent f_pos.
    if (vfs_open("/alpha", 0, &f1) != 0) {
        uart_puts("[FAIL] open f1\n");
        return;
    }

    if (vfs_open("/alpha", 0, &f2) != 0) {
        uart_puts("[FAIL] open f2\n");
        vfs_close(f1);
        return;
    }

    memset(buf, 0, sizeof(buf));

    if (vfs_read(f1, buf, 3) != 3 || strcmp(buf, "abc") != 0) {
        uart_puts("[FAIL] f1 first read\n");
        vfs_close(f1);
        vfs_close(f2);
        return;
    }

    memset(buf, 0, sizeof(buf));

    if (vfs_read(f2, buf, 3) != 3 || strcmp(buf, "abc") != 0) {
        uart_puts("[FAIL] f2 independent f_pos\n");
        vfs_close(f1);
        vfs_close(f2);
        return;
    }

    vfs_close(f1);
    vfs_close(f2);

    // 8. Nested path should fail for now because mkdir is not implemented yet.
    if (vfs_open("/a/b/file", O_CREAT, &f) == 0) {
        uart_puts("[FAIL] nested create should fail before mkdir exists\n");
        vfs_close(f);
        return;
    }

    uart_puts("[PASS] basic1 extended VFS test\n");
}

void test_vfs_basic2(void) {
    struct file* f = 0;
    char buf[32];

    // 1. Create nested directories.
    if (vfs_mkdir("/a") != 0) {
        uart_puts("[FAIL] mkdir /a\n");
        return;
    }

    if (vfs_mkdir("/a/b") != 0) {
        uart_puts("[FAIL] mkdir /a/b\n");
        return;
    }

    // 2. Duplicate mkdir should fail.
    if (vfs_mkdir("/a") == 0) {
        uart_puts("[FAIL] duplicate mkdir /a should fail\n");
        return;
    }

    // 3. Create file inside nested directory.
    if (vfs_open("/a/b/file", O_CREAT, &f) != 0) {
        uart_puts("[FAIL] open /a/b/file\n");
        return;
    }

    if (vfs_write(f, "abc", 3) != 3) {
        uart_puts("[FAIL] write /a/b/file\n");
        vfs_close(f);
        return;
    }

    vfs_close(f);

    // 4. Read nested file back.
    memset(buf, 0, sizeof(buf));

    if (vfs_open("/a/b/file", 0, &f) != 0) {
        uart_puts("[FAIL] reopen /a/b/file\n");
        return;
    }

    if (vfs_read(f, buf, 3) != 3 || strcmp(buf, "abc") != 0) {
        uart_puts("[FAIL] read /a/b/file data mismatch\n");
        vfs_close(f);
        return;
    }

    vfs_close(f);

    // 5. Create mount point directory.
    if (vfs_mkdir("/mnt") != 0) {
        uart_puts("[FAIL] mkdir /mnt\n");
        return;
    }

    // 6. Create a file under /mnt before mounting.
    if (vfs_open("/mnt/oldfile", O_CREAT, &f) != 0) {
        uart_puts("[FAIL] create /mnt/oldfile before mount\n");
        return;
    }

    if (vfs_write(f, "old", 3) != 3) {
        uart_puts("[FAIL] write /mnt/oldfile\n");
        vfs_close(f);
        return;
    }

    vfs_close(f);

    // 7. Mount another tmpfs on /mnt.
    if (vfs_mount("/mnt", "tmpfs") != 0) {
        uart_puts("[FAIL] mount tmpfs on /mnt\n");
        return;
    }

    // 8. oldfile should be hidden by mounted filesystem.
    if (vfs_open("/mnt/oldfile", 0, &f) == 0) {
        uart_puts("[FAIL] /mnt/oldfile should be hidden after mount\n");
        vfs_close(f);
        return;
    }

    // 9. Create file inside mounted tmpfs.
    if (vfs_open("/mnt/newfile", O_CREAT, &f) != 0) {
        uart_puts("[FAIL] create /mnt/newfile after mount\n");
        return;
    }

    if (vfs_write(f, "new", 3) != 3) {
        uart_puts("[FAIL] write /mnt/newfile\n");
        vfs_close(f);
        return;
    }

    vfs_close(f);

    // 10. Read file inside mounted tmpfs.
    memset(buf, 0, sizeof(buf));

    if (vfs_open("/mnt/newfile", 0, &f) != 0) {
        uart_puts("[FAIL] reopen /mnt/newfile\n");
        return;
    }

    if (vfs_read(f, buf, 3) != 3 || strcmp(buf, "new") != 0) {
        uart_puts("[FAIL] read /mnt/newfile data mismatch\n");
        vfs_close(f);
        return;
    }

    vfs_close(f);

    uart_puts("[PASS] basic2 mkdir/path/mount\n");
}