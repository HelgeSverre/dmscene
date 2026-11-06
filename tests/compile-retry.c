/* Count compiler and module allocations across failed compilations, including
 * longjmp cleanup of blocks, patterns and missing asset paths. */
#include "dm.h"
#include <stdlib.h>
#include <string.h>
static void *owned[256];
static void *count_malloc(size_t n) {
  void *p = malloc(n);
  for (int i = 0; i < 256; i++) if (!owned[i]) { owned[i] = p; return p; }
  abort();
}
static void *count_calloc(size_t n, size_t size) {
  void *p = count_malloc(n * size); if (p) memset(p, 0, n * size); return p;
}
static void count_free(void *p) {
  for (int i = 0; i < 256; i++) if (owned[i] == p) owned[i] = NULL;
  free(p);
}
static void *count_realloc(void *p, size_t n) {
  for (int i = 0; i < 256; i++) if (owned[i] == p) {
    void *q = realloc(p, n); if (q) owned[i] = q; return q;
  }
  abort();
}
#define calloc count_calloc
#define malloc count_malloc
#define realloc count_realloc
#define free count_free
#include "../src/dm_compile.c"
#include "../src/dm_module.c"
#undef calloc
#undef malloc
#undef realloc
#undef free

int main(void) {
  const char *src[] = {
    "fn f(a) {\n a\n}\ninst bad {\n out(nosuch(1))\n}\n",
    "fn f(a) {\n a\n",
    "pixel {\n rgb(nosuch(1),0,0)\n}\n",
    "pattern 0 rows=1 {\n nosuch\n}\n",
    "sample s \"/nonexistent-dmscene.wav\"\n",
    "texture t \"/nonexistent-dmscene.ppm\"\n"
  };
  int bad = 0;
  for (int retry = 0; retry < 10; retry++) for (unsigned j = 0; j < sizeof src / sizeof *src; j++) {
    dm_module m = {0};
    if (!dm_compile(src[j], "retry", &m)) return 2;
    dm_module_free(&m);
    for (int i = 0; i < 256; i++) if (owned[i]) {
      if (!bad) fprintf(stderr, "FAIL: failed compilation retains compiler allocations\n");
      bad = 1; count_free(owned[i]);
    }
  }
  if (!bad) puts("ok: failed compile retries release all compiler allocations");
  return bad;
}
