#include<myjp.h>
#include<stdio.h>

int main(int argc,char **argv){
    char *path = argc > 1 ? argv[1] : "/home/yuujirou07/vscode_proj/json_perser/jp_test.json";
    JP_data *jp_data = JP_create_from_file(path);



    if(jp_data == NULL){
        fprintf(stderr,"jp create error\n");
        return 1;
    }

    if(JP_parse(jp_data) != 0){
        size_t line,col;
        const char *msg = JP_error(jp_data,&line,&col);
        fprintf(stderr,"parse error: %s (line %zu, col %zu)\n",msg,line,col);
        JP_destroy(jp_data);
        return 1;
    }


    JP_value *search_value = JP_get_value(jp_data,"code");
    JP_print(search_value,0);
    putchar('\n');
    JP_destroy(jp_data);
    return 0;
}
