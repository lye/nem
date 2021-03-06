#include <sys/types.h>
#include <sqlite3.h>
#include <time.h>
#include "nem.h"

extern bool NEM_rootd_testing;

NEM_err_t NEM_err_sqlite(sqlite3 *db);
NEM_err_t NEM_path_abs(char **path);
NEM_err_t NEM_path_join(char **out, const char *base, const char *rest);
NEM_err_t NEM_ensure_dir(const char *path);
NEM_err_t NEM_erase_dir(const char *path);

int NEM_tm_cmp(struct tm *lhs, struct tm *rhs);
