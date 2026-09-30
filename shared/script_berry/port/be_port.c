/* Berry port: no stdin, no file system; output goes to the script console. */
#include <stddef.h>
#include <string.h>
#include "berry.h"

void script_console_write(const char *buf, size_t len);

BERRY_API void be_writebuffer(const char *buffer, size_t length)
{
    script_console_write(buffer, length);
}

BERRY_API char *be_readstring(char *buffer, size_t size)
{
    (void)buffer;
    (void)size;
    return NULL;
}

/* File API: there is no file system, so every call fails (the "file"
 * module and loading scripts by name report an error to the script). */
#include "be_sys.h"

void *be_fopen(const char *filename, const char *modes) { (void)filename; (void)modes; return NULL; }
int be_fclose(void *hfile) { (void)hfile; return -1; }
size_t be_fwrite(void *hfile, const void *buffer, size_t length) { (void)hfile; (void)buffer; (void)length; return 0; }
size_t be_fread(void *hfile, void *buffer, size_t length) { (void)hfile; (void)buffer; (void)length; return 0; }
char *be_fgets(void *hfile, void *buffer, int size) { (void)hfile; (void)buffer; (void)size; return NULL; }
int be_fseek(void *hfile, long offset) { (void)hfile; (void)offset; return -1; }
long int be_ftell(void *hfile) { (void)hfile; return -1; }
long int be_fflush(void *hfile) { (void)hfile; return -1; }
size_t be_fsize(void *hfile) { (void)hfile; return 0; }
