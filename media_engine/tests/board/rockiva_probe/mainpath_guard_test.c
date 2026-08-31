#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mainpath_guard.h"

static int make_path(char *path, size_t path_size, const char *directory,
			     const char *suffix)
{
	int written = snprintf(path, path_size, "%s/%s", directory, suffix);

	return written >= 0 && (size_t)written < path_size ? 0 : -1;
}

static int expect_match(const char *name, const char *device,
			const char *mainpath, int expected)
{
	int actual = rockiva_probe_is_mainpath(device, mainpath);

	if (actual != expected) {
		fprintf(stderr, "%s: expected %d, got %d\n", name, expected, actual);
		return -1;
	}
	printf("[PASS] %s\n", name);
	return 0;
}

static int expect_fd_match(const char *name, int device_fd,
				 const char *mainpath, int expected)
{
	int actual = rockiva_probe_fd_is_mainpath(device_fd, mainpath);

	if (actual != expected) {
		fprintf(stderr, "%s: expected %d, got %d\n", name, expected, actual);
		return -1;
	}
	printf("[PASS] %s\n", name);
	return 0;
}

int main(void)
{
	char directory_template[] = "/tmp/rockiva-mainpath-guard.XXXXXX";
	char *directory;
	char subdirectory[PATH_MAX];
	char mainpath[PATH_MAX];
	char normalized_alias[PATH_MAX];
	char symlink_alias[PATH_MAX];
	char other_path[PATH_MAX];
	int fd = -1;
	int char_device_fd = -1;
	int result = 1;

	subdirectory[0] = '\0';
	mainpath[0] = '\0';
	normalized_alias[0] = '\0';
	symlink_alias[0] = '\0';
	other_path[0] = '\0';
	directory = mkdtemp(directory_template);
	if (!directory) {
		fprintf(stderr, "mkdtemp failed: %s\n", strerror(errno));
		return 1;
	}
	if (make_path(subdirectory, sizeof(subdirectory), directory, "sub") != 0 ||
	    mkdir(subdirectory, 0700) != 0 ||
	    make_path(mainpath, sizeof(mainpath), directory, "video24") != 0 ||
	    make_path(normalized_alias, sizeof(normalized_alias), directory,
		      "sub/../video24") != 0 ||
	    make_path(symlink_alias, sizeof(symlink_alias), directory,
		      "video24-alias") != 0 ||
	    make_path(other_path, sizeof(other_path), directory, "video25") != 0) {
		fprintf(stderr, "cannot prepare mainpath guard test paths\n");
		goto cleanup;
	}
	fd = open(mainpath, O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (fd < 0 || close(fd) != 0)
		goto cleanup;
	fd = -1;
	if (symlink("video24", symlink_alias) != 0)
		goto cleanup;
	fd = open(other_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (fd < 0 || close(fd) != 0)
		goto cleanup;
	fd = -1;

	if (expect_match("canonical mainpath", mainpath, mainpath, 1) != 0 ||
	    expect_match("normalized alias", normalized_alias, mainpath, 1) != 0 ||
	    expect_match("symlink alias", symlink_alias, mainpath, 1) != 0 ||
	    expect_match("different device path", other_path, mainpath, 0) != 0)
		goto cleanup;
	char_device_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
	if (char_device_fd < 0) {
		fprintf(stderr, "cannot open /dev/null for fd identity test: %s\n",
			strerror(errno));
		goto cleanup;
	}
	if (expect_fd_match("opened mainpath character device", char_device_fd,
				   "/dev/null", 1) != 0 ||
	    expect_fd_match("opened different character device", char_device_fd,
				   "/dev/zero", 0) != 0 ||
	    expect_fd_match("invalid opened fd", -1, "/dev/null", -1) != 0)
		goto cleanup;
	result = 0;

cleanup:
	if (char_device_fd >= 0)
		(void)close(char_device_fd);
	if (fd >= 0)
		(void)close(fd);
	(void)unlink(symlink_alias);
	(void)unlink(other_path);
	(void)unlink(mainpath);
	(void)rmdir(subdirectory);
	(void)rmdir(directory);
	return result;
}
