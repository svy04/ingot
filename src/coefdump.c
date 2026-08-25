#include <stdio.h>
double ingot_coef_e[6][4][64 * 64];
void ingot_coef_dump(const char *path)
{
    FILE *f = fopen(path, "w");
    int sz, tx, k;
    if (!f) return;
    for (sz = 0; sz < 6; sz++)
        for (tx = 0; tx < 4; tx++) {
            int n = (sz == 0) ? 4 : (sz == 1) ? 8 : (sz == 2) ? 16
                  : (sz == 3) ? 32 : 64;
            double s = 0;
            for (k = 0; k < n * n; k++) s += ingot_coef_e[sz][tx][k];
            if (s <= 0) continue;
            fprintf(f, "SZ %d TX %d N %d\n", sz, tx, n);
            for (k = 0; k < n * n; k++)
                fprintf(f, "%d %.1f\n", k, ingot_coef_e[sz][tx][k]);
        }
    fclose(f);
}
