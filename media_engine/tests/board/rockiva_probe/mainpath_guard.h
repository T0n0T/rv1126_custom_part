#ifndef ROCKIVA_PROBE_MAINPATH_GUARD_H
#define ROCKIVA_PROBE_MAINPATH_GUARD_H

#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>

#define ROCKIVA_PROBE_MAINPATH "/dev/video24"

/* Resolve aliases before allowing an independent probe to touch the mainpath. */
static int rockiva_probe_is_mainpath(const char *device,
					     const char *mainpath)
{
	struct stat device_stat;
	struct stat mainpath_stat;
	char *resolved_device = NULL;
	char *resolved_mainpath = NULL;
	int result = 0;

	if (!device || !device[0] || !mainpath || !mainpath[0])
		return 0;
	if (stat(device, &device_stat) == 0 &&
	    stat(mainpath, &mainpath_stat) == 0 &&
	    S_ISCHR(device_stat.st_mode) && S_ISCHR(mainpath_stat.st_mode) &&
	    device_stat.st_rdev == mainpath_stat.st_rdev)
		return 1;
	resolved_device = realpath(device, NULL);
	resolved_mainpath = realpath(mainpath, NULL);
	if (resolved_device && resolved_mainpath)
		result = strcmp(resolved_device, resolved_mainpath) == 0;
	else if (strcmp(device, mainpath) == 0)
		result = 1;
	free(resolved_device);
	free(resolved_mainpath);
	return result;
}

/* Re-check the object actually opened, closing the path-check/open race. */
static int rockiva_probe_fd_is_mainpath(int device_fd, const char *mainpath)
{
	struct stat device_stat;
	struct stat mainpath_stat;

	if (device_fd < 0 || !mainpath || !mainpath[0])
		return -1;
	if (fstat(device_fd, &device_stat) != 0)
		return -1;
	if (stat(mainpath, &mainpath_stat) != 0)
		return -1;
	if (!S_ISCHR(device_stat.st_mode) || !S_ISCHR(mainpath_stat.st_mode))
		return 0;
	return device_stat.st_rdev == mainpath_stat.st_rdev ? 1 : 0;
}

#endif
