/*
    dali_query.h - DALI query command handler (IEC 62386-102, cmd 144-199)

    Read-only lookups on shared state + backward frame responses.
*/
#ifndef _DALI_QUERY_H
#define _DALI_QUERY_H

#include <stdint.h>

/* Process a query command (144-199). Sends backward frame if applicable. */
void dali_query_process(uint8_t cmd);

#endif /* _DALI_QUERY_H */
