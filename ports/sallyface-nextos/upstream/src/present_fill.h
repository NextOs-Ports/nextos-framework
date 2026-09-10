/* Port-specific final-frame fill adapter for Sally Face. */
#ifndef SF_PRESENT_FILL_H
#define SF_PRESENT_FILL_H

void sf_present_fill_set_resolver(void *(*resolver)(const char *));
int sf_present_fill_apply(int drawable_width, int drawable_height,
                          const char *aspect_policy);

#endif
