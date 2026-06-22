/* Paging: identity-map the address space and turn paging on. */
#ifndef PAGING_H
#define PAGING_H

void paging_init(void);   /* identity-map 4 GiB with 4 MiB pages, enable paging */

#endif
