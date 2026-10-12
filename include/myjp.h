
#ifndef MYJP_H
#define MYJP_H

#include<stdbool.h>
#include <stddef.h>
#include<stdlib.h>
#include <threads.h>

typedef enum{
    JP_OP_DEPHS,
}JP_OPTION;

//中身はcore.cで定義(隠ぺい)
typedef struct JP_value JP_value;
typedef struct JP_data JP_data;

JP_data *JP_create_from_file(const char *path);
JP_data *JP_create_from_string(const char *str);
int JP_parse(JP_data *data);
void JP_print(JP_value *value,int indent);
void JP_destroy(JP_data *data);
const char *JP_error(JP_data *data,size_t *line,size_t *col);
int JP_option(JP_data *data,size_t s,JP_OPTION option);
JP_value *JP_get_value(JP_data *data,const char *key);


#endif
