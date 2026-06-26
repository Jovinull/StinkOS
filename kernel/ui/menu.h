/* Start menu: lists the on-disk apps and launches the chosen one. */
#ifndef MENU_H
#define MENU_H

void menu_run(void);   /* draw the menu, let the user pick an app, run it */
void menu_exit(void);  /* called by the SYS_EXIT syscall: return to the menu */

#endif
