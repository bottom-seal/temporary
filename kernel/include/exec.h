#ifndef EXEC_H
#define EXEC_H

int initrd_load_program(const void *rd,
                        const char *filename,
                        unsigned long *program_base,
                        unsigned long *program_size);
int initrd_exec(const void *rd, const char *filename);

#endif
