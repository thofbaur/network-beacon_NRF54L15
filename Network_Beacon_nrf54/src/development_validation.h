#ifndef DEVELOPMENT_VALIDATION_H
#define DEVELOPMENT_VALIDATION_H

#include <stdint.h>

#if defined(CONFIG_DSA_DEV_SYNTHETIC_CONTACTS)
void development_validation_fill_random_contacts(uint16_t count);
#endif

#if defined(CONFIG_DSA_DEV_SYNTHETIC_SELF_REPORTS)
void development_validation_fill_self_reports(uint16_t count);
#endif

#endif /* DEVELOPMENT_VALIDATION_H */
