#ifndef DEV__CHAR__CONSOLE_K_H_
#define DEV__CHAR__CONSOLE_K_H_

#include <stddef.h>

void console_init(void);
void console_write(const char *buf, size_t length);

#endif
