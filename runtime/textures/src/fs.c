/* Minimal POSIX file helpers (mkdir -p, read all, atomic write). Portable to Linux/macOS/iOS. */
#define _POSIX_C_SOURCE 200809L
#include "rltex.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

int rltex_fs_exists(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0;
}

int rltex_fs_mkdir_p(const char *path)
{
	char buf[1024];
	size_t n = strlen(path);
	if (!n || n >= sizeof buf) return RLTEX_E_ARG;
	memcpy(buf, path, n + 1);
	for (size_t i = 1; i <= n; i++) {
		if (buf[i] == '/' || buf[i] == 0) {
			char c = buf[i];
			buf[i] = 0;
			if (mkdir(buf, 0755) != 0 && errno != EEXIST) return RLTEX_E_IO;
			buf[i] = c;
		}
	}
	return RLTEX_OK;
}

int rltex_fs_read_all(const char *path, uint8_t **out, size_t *out_size)
{
	FILE *f = fopen(path, "rb");
	uint8_t *buf;
	long n;
	if (!f) return RLTEX_E_IO;
	if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		return RLTEX_E_IO;
	}
	buf = (uint8_t *)malloc(n ? (size_t)n : 1);
	if (!buf) { fclose(f); return RLTEX_E_NOMEM; }
	if (n && fread(buf, 1, (size_t)n, f) != (size_t)n) {
		free(buf);
		fclose(f);
		return RLTEX_E_IO;
	}
	fclose(f);
	*out = buf;
	*out_size = (size_t)n;
	return RLTEX_OK;
}

int rltex_fs_write_atomic(const char *path, const void *data, size_t size)
{
	static atomic_uint counter;
	char tmp[1100];
	FILE *f;
	int ok;
	unsigned id = atomic_fetch_add(&counter, 1u);
	if (snprintf(tmp, sizeof tmp, "%s.tmp.%ld.%u", path, (long)getpid(), id) >= (int)sizeof tmp)
		return RLTEX_E_ARG;
	f = fopen(tmp, "wb");
	if (!f) return RLTEX_E_IO;
	ok = fwrite(data, 1, size, f) == size;
	ok = (fclose(f) == 0) && ok;
	if (!ok || rename(tmp, path) != 0) {
		remove(tmp);
		return RLTEX_E_IO;
	}
	return RLTEX_OK;
}
