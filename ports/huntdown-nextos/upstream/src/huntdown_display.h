#ifndef HUNTDOWN_DISPLAY_H
#define HUNTDOWN_DISPLAY_H

/* Resolve the visible display size shared by every Huntdown host bridge.
 * Returns one only when both dimensions are valid.  The optional source points
 * to a static diagnostic label. */
int hd_display_size_detect(int *width, int *height, const char **source);

#endif
