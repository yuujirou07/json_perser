

#include<myjp.h>
#include<stddef.h>
#include<stdint.h>
#include<stdio.h>
#include<stdlib.h>
#include<string.h>


//JSONの値の種類
typedef enum{
    JP_NULL,
    JP_BOOL,
    JP_NUM,
    JP_STR,
    JP_ARRY,
    JP_OBJ,
}JP_TYPE;

typedef struct JP_member JP_member;

//JSONの値1つ。typeで共用体uのどのメンバが有効かを決める
struct JP_value{
    JP_TYPE type;
    union{
        bool boolean;                                         //JP_BOOL
        double number;                                        //JP_NUM 整数と小数を区別しない
        struct{ char *data; size_t len; } string;             //JP_STR '\0'終端済み。lenは'\0'を含まない
        struct{ JP_value *items; size_t count; } array;       //JP_ARRY 要素を出現順に持つ
        struct{ JP_member *members; size_t count; } object;   //JP_OBJ "key":valueの組を出現順に持つ
    }u;
};


struct JP_member{   // オブジェクトの "key": value の1組
    char *key;
    JP_value value;
};

//読み込んだファイルの中身。file_str[file_size]には'\0'を置く
typedef struct{
    char *file_str;
    size_t file_size;
}JP_file_data;

//今読んでいる位置
typedef struct{
    char *now_ptr;
}JP_pos;

struct JP_data{
    JP_value value;             //パース結果の最上位の値(JSONの最上位の値は必ず1つ)
    JP_pos read_pos;
    JP_file_data file_data;
    const char *err_msg;        //失敗時の理由。成功時はNULL
    size_t err_line,err_col;    //失敗した位置(1始まり)
    int depth;   
    size_t JP_MAX_DEPTH;               //今の入れ子の深さ
};



static char *f_read_all(FILE*f,size_t *size);
static bool is_end(JP_data *data);
static char now_chr(JP_data *data);
static void skip_s(JP_data *data);
static int set_error(JP_data *data,const char *msg);
static int parse_value(JP_value *value,JP_data *data);
static int parse_object(JP_value *value,JP_data *data);
static int parse_arry(JP_value *value,JP_data *data);
static int parse_string(char **out,size_t *out_len,JP_data *data);
static int parse_number(JP_value *value,JP_data *data);
static int parse_literal(JP_value *value,JP_data *data,const char *word,JP_TYPE type,bool boolean);
static void free_value(JP_value *value);
static JP_value *JP_search_value(JP_value *value,const char *key);

//失敗理由を返す。行・列は不要ならNULLでよい
const char *JP_error(JP_data *data,size_t *line,size_t *col){
    if(line != NULL)*line = data->err_line;
    if(col != NULL)*col = data->err_col;
    return data->err_msg;
}

//ファイルを丸ごと読み込み、パースの準備をする
//返り値: 成功時は JP_destroy() で解放する JP_data、失敗時 NULL
JP_data *JP_create_from_file(const char *path){
    if(path == NULL)return NULL;

    FILE *jp_f = fopen(path,"r");
    if(jp_f == NULL)return NULL;

    JP_data *tmp_jp_data = calloc(1,sizeof(JP_data));
    if(tmp_jp_data == NULL){
        fclose(jp_f);
        return NULL;
    }

    tmp_jp_data->file_data.file_str =
        f_read_all(jp_f,&tmp_jp_data->file_data.file_size);
    fclose(jp_f);
    if(tmp_jp_data->file_data.file_str == NULL){
        free(tmp_jp_data);
        return NULL;
    }
    tmp_jp_data->read_pos.now_ptr =
        &tmp_jp_data->file_data.file_str[0];
    tmp_jp_data->value.type = JP_NULL;
    tmp_jp_data->JP_MAX_DEPTH = 512;
    return tmp_jp_data;
}



JP_data *JP_create_from_string(const char *str){

    if(str == NULL)return NULL;


    FILE *jp_f = fopen(str,"r");
    if(jp_f == NULL)return NULL;

    JP_data *tmp_jp_data = calloc(1,sizeof(JP_data));
    if(tmp_jp_data == NULL){
        fclose(jp_f);
        return NULL;
    }

    tmp_jp_data->file_data.file_str = strdup(str);
    fclose(jp_f);
    if(tmp_jp_data->file_data.file_str == NULL){
        free(tmp_jp_data);
        return NULL;
    }
    tmp_jp_data->read_pos.now_ptr =
        &tmp_jp_data->file_data.file_str[0];
    tmp_jp_data->value.type = JP_NULL;
    tmp_jp_data->JP_MAX_DEPTH = 512;
    return tmp_jp_data;
}



//読み込んだ文字列全体を1つのJSONの値としてパースし、結果を data->value に入れる
//返り値: 成功時 0、失敗時 -1(理由と位置は data->err_msg / err_line / err_col)
int JP_parse(JP_data *data){
    if(data == NULL || data->file_data.file_str == NULL)return -1;

    //流れ: 値を1つ読む → 空白を飛ばす → ファイルの末尾まで来ているか
    int result = parse_value(&data->value,data);
    if(result == 0){
        skip_s(data);
        if(!is_end(data))result = set_error(data,"extra data after JSON value");
    }
    if(result != 0){
        //失敗したら途中まで作った木を捨てる(木は常に解放できる状態で組み立てている)
        free_value(&data->value);
        return -1;
    }
    return 0;
}


//JP_create() で確保したものとパース結果をすべて解放する
void JP_destroy(JP_data *data){
    if(data == NULL)return;
    free_value(&data->value);
    free(data->file_data.file_str);
    free(data);
}


static char *f_read_all(FILE*f,size_t *size){
    if(fseek(f,0,SEEK_SET) != 0){
        *size = 0;
        return NULL;
    }

    size_t buff_size = 1024;

    size_t read_count = 0;
    char *tmp_buff = malloc(buff_size);
    if(tmp_buff == NULL){
        *size = 0;
        return NULL;
    }


    while(1){
        //配列拡張処理(末尾の'\0'用に常に1バイト残す)
        if(buff_size <= read_count + 1){
            buff_size *= 2;
            char *tmp_tmp_buff = realloc(tmp_buff,buff_size);
            if(tmp_tmp_buff == NULL){
                *size = 0;
                free(tmp_buff);
                return NULL;
            }
            tmp_buff = tmp_tmp_buff;
        }
        size_t read_len = fread(tmp_buff + read_count,sizeof(char),buff_size - read_count - 1,f);
        if(read_len == 0)break;
        read_count+= read_len;
    }
    tmp_buff[read_count] = '\0';
    *size = read_count;
    return tmp_buff;
}



//---------------- 読み進めるための小さな部品 ----------------

//ファイルの末尾まで読み終えたか
static bool is_end(JP_data *data){
    return (size_t)(data->read_pos.now_ptr - data->file_data.file_str) >= data->file_data.file_size;
}

//今の位置の文字。末尾なら'\0'を返すので、呼び出し側は範囲外を読まずに済む
static char now_chr(JP_data *data){
    return is_end(data) ? '\0' : *data->read_pos.now_ptr;
}

//空白・タブ・改行を飛ばす
static void skip_s(JP_data *data){
    for(;;){
        char tmp_chr = now_chr(data);
        if(tmp_chr == ' ' || tmp_chr == '\t' || tmp_chr == '\n' || tmp_chr == '\r'){
            data->read_pos.now_ptr++;
        }
        else{
            return;
        }
    }
}

//エラーの理由と、今の位置の行・列を記録する
//返り値: 常に -1(return set_error(...); と書けるように)
static int set_error(JP_data *data,const char *msg){
    data->err_msg = msg;
    data->err_line = 1;
    data->err_col = 1;
    for(char *p = data->file_data.file_str;p < data->read_pos.now_ptr;p++){
        if(*p == '\n'){
            data->err_line++;
            data->err_col = 1;
        }
        else data->err_col++;
    }
    return -1;
}



//---------------- 値を読む関数 ----------------
//どの関数も「成功したら読んだ分だけ now_ptr を進めて 0、失敗したら set_error() して -1」で統一する

//次の1文字を見て、どの種類の値かを判断し、その種類を読む関数へ振り分ける
static int parse_value(JP_value *value,JP_data *data){
    value->type = JP_NULL; //失敗しても free_value() で安全に扱えるように先に決めておく
    skip_s(data);
    char chr = now_chr(data);
    switch(chr){
        case '{':
            return parse_object(value,data);
        case '[':
            return parse_arry(value,data);
        case '"':
            value->type = JP_STR;
            value->u.string.data = NULL;
            return parse_string(&value->u.string.data,&value->u.string.len,data);
        case 't':
            return parse_literal(value,data,"true",JP_BOOL,true);
        case 'f':
            return parse_literal(value,data,"false",JP_BOOL,false);
        case 'n':
            return parse_literal(value,data,"null",JP_NULL,false);
        default:
            if(chr == '-' || (chr >= '0' && chr <= '9'))return parse_number(value,data);
            if(chr == '\0')return set_error(data,"unexpected end of input");
            return set_error(data,"unexpected character");
    }
    
}


//{ "key": value , "key": value ... } を読む
static int parse_object(JP_value *value,JP_data *data){
    if(++data->depth > data->JP_MAX_DEPTH)return set_error(data,"nesting too deep");

    //'{' を飛ばし、空のオブジェクトとして始める
    data->read_pos.now_ptr++;
    value->type = JP_OBJ;
    value->u.object.members = NULL;
    value->u.object.count = 0;
    size_t capacity = 0;

    skip_s(data);
    if(now_chr(data) == '}'){ //{} 空のオブジェクト
        data->read_pos.now_ptr++;
        data->depth--;
        return 0;
    }

    for(;;){
        //1. キー(必ず "..." で囲まれた文字列)
        skip_s(data);
        if(now_chr(data) != '"')return set_error(data,"expected '\"' for object key");

        //メンバーを1つ分広げる。足りなければ2倍に広げる
        if(value->u.object.count == capacity){
            size_t new_capacity = capacity == 0 ? 4 : capacity * 2;
            JP_member *tmp = realloc(value->u.object.members,sizeof(JP_member) * new_capacity);
            if(tmp == NULL)return set_error(data,"out of memory");
            value->u.object.members = tmp;
            capacity = new_capacity;
        }

        //先に count を増やして空の状態で置き、途中で失敗しても木全体を解放できるようにする
        JP_member *member = &value->u.object.members[value->u.object.count++];
        member->key = NULL;
        member->value.type = JP_NULL;

        size_t key_len;
        if(parse_string(&member->key,&key_len,data) != 0)return -1;

        //2. キーと値の区切りの ':'
        skip_s(data);
        if(now_chr(data) != ':')return set_error(data,"expected ':' after object key");
        data->read_pos.now_ptr++;

        //3. 値(ここで再帰する。値が { なら parse_object がまた呼ばれる)
        if(parse_value(&member->value,data) != 0)return -1;

        //4. ',' なら次のメンバーへ、'}' なら終わり
        skip_s(data);
        char chr = now_chr(data);
        if(chr == ','){
            data->read_pos.now_ptr++;
            continue;
        }
        if(chr == '}'){
            data->read_pos.now_ptr++;
            break;
        }
        return set_error(data,"expected ',' or '}' in object");
    }
    data->depth--;
    return 0;
}


//[ value , value ... ] を読む。流れは parse_object からキーと ':' を除いたもの
static int parse_arry(JP_value *value,JP_data *data){
    if(++data->depth > data->JP_MAX_DEPTH)return set_error(data,"nesting too deep");

    data->read_pos.now_ptr++;
    value->type = JP_ARRY;
    value->u.array.items = NULL;
    value->u.array.count = 0;
    size_t capacity = 0;

    skip_s(data);
    if(now_chr(data) == ']'){ //[] 空の配列
        data->read_pos.now_ptr++;
        data->depth--;
        return 0;
    }

    for(;;){
        if(value->u.array.count == capacity){
            size_t new_capacity = capacity == 0 ? 4 : capacity * 2;
            JP_value *tmp = realloc(value->u.array.items,sizeof(JP_value) * new_capacity);
            if(tmp == NULL)return set_error(data,"out of memory");
            value->u.array.items = tmp;
            capacity = new_capacity;
        }
        JP_value *item = &value->u.array.items[value->u.array.count++];
        if(parse_value(item,data) != 0)return -1;

        skip_s(data);
        char chr = now_chr(data);
        if(chr == ','){
            data->read_pos.now_ptr++;
            continue;
        }
        if(chr == ']'){
            data->read_pos.now_ptr++;
            break;
        }
        return set_error(data,"expected ',' or ']' in array");
    }
    data->depth--;
    return 0;
}


//出力先の文字列の末尾に1バイト足す。足りなければ2倍に広げる
static int push_chr(char **buf,size_t *len,size_t *capacity,char chr){
    if(*len + 1 >= *capacity){
        size_t new_capacity = *capacity == 0 ? 16 : *capacity * 2;
        char *tmp = realloc(*buf,new_capacity);
        if(tmp == NULL)return -1;
        *buf = tmp;
        *capacity = new_capacity;
    }
    (*buf)[(*len)++] = chr;
    (*buf)[*len] = '\0';
    return 0;
}

//\uXXXX の16進4桁を読む。成功時 0
static int read_hex4(JP_data *data,uint32_t *out){
    *out = 0;
    for(int i = 0;i < 4;i++){
        char chr = now_chr(data);
        int digit;
        if(chr >= '0' && chr <= '9')digit = chr - '0';
        else if(chr >= 'a' && chr <= 'f')digit = chr - 'a' + 10;
        else if(chr >= 'A' && chr <= 'F')digit = chr - 'A' + 10;
        else return -1;
        *out = *out * 16 + digit;
        data->read_pos.now_ptr++;
    }
    return 0;
}

//"..." を読み、エスケープを展開した新しい文字列を *out に入れる(呼び出し側の木が持ち主になる)
//キーと文字列の値の両方で使う
static int parse_string(char **out,size_t *out_len,JP_data *data){
    char *buf = NULL;
    size_t len = 0,capacity = 0;

    data->read_pos.now_ptr++; //開始の '"' を飛ばす
    for(;;){
        if(is_end(data)){
            free(buf);
            return set_error(data,"unterminated string");
        }
        unsigned char chr = (unsigned char)*data->read_pos.now_ptr;

        if(chr == '"'){ //閉じる '"'
            data->read_pos.now_ptr++;
            break;
        }
        if(chr < 0x20){ //JSONでは改行やタブを文字列にそのまま書けない(\n \t と書く)
            free(buf);
            return set_error(data,"control character in string");
        }
        data->read_pos.now_ptr++;
        if(chr != '\\'){ //普通の文字(UTF-8の日本語もバイト単位でそのままコピー)
            if(push_chr(&buf,&len,&capacity,(char)chr) != 0)goto no_memory;
            continue;
        }

        //ここからエスケープ: \ の次の1文字で決まる
        char esc = now_chr(data);
        char plain = 0;
        switch(esc){
            case '"':  plain = '"';  break;
            case '\\': plain = '\\'; break;
            case '/':  plain = '/';  break;
            case 'b':  plain = '\b'; break;
            case 'f':  plain = '\f'; break;
            case 'n':  plain = '\n'; break;
            case 'r':  plain = '\r'; break;
            case 't':  plain = '\t'; break;
            case 'u':  break;
            default:
                free(buf);
                return set_error(data,"invalid escape in string");
        }
        data->read_pos.now_ptr++;
        if(plain != 0){
            if(push_chr(&buf,&len,&capacity,plain) != 0)goto no_memory;
            continue;
        }

        //\uXXXX: Unicodeの番号を UTF-8 のバイト列に直す
        uint32_t code;
        if(read_hex4(data,&code) != 0){
            free(buf);
            return set_error(data,"invalid \\u escape");
        }
        //U+10000以上(絵文字など)は 😀 のように2つ組(サロゲートペア)で書かれる
        if(code >= 0xD800 && code <= 0xDBFF){
            uint32_t low = 0;
            int ok = now_chr(data) == '\\';
            if(ok){
                data->read_pos.now_ptr++;
                ok = now_chr(data) == 'u';
            }
            if(ok){
                data->read_pos.now_ptr++;
                ok = read_hex4(data,&low) == 0 && low >= 0xDC00 && low <= 0xDFFF;
            }
            if(!ok){
                free(buf);
                return set_error(data,"invalid surrogate pair");
            }
            code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
        }
        else if(code >= 0xDC00 && code <= 0xDFFF){ //2つ組の後半だけが単独で来た
            free(buf);
            return set_error(data,"invalid surrogate pair");
        }

        //番号の大きさで UTF-8 のバイト数(1〜4)が決まる
        char utf8[4];
        int n;
        if(code < 0x80){
            utf8[0] = (char)code;
            n = 1;
        }
        else if(code < 0x800){
            utf8[0] = (char)(0xC0 | (code >> 6));
            utf8[1] = (char)(0x80 | (code & 0x3F));
            n = 2;
        }
        else if(code < 0x10000){
            utf8[0] = (char)(0xE0 | (code >> 12));
            utf8[1] = (char)(0x80 | ((code >> 6) & 0x3F));
            utf8[2] = (char)(0x80 | (code & 0x3F));
            n = 3;
        }
        else{
            utf8[0] = (char)(0xF0 | (code >> 18));
            utf8[1] = (char)(0x80 | ((code >> 12) & 0x3F));
            utf8[2] = (char)(0x80 | ((code >> 6) & 0x3F));
            utf8[3] = (char)(0x80 | (code & 0x3F));
            n = 4;
        }
        for(int i = 0;i < n;i++){
            if(push_chr(&buf,&len,&capacity,utf8[i]) != 0)goto no_memory;
        }
    }

    //"" (空文字列) のときは1度も確保していないので、ここで空の文字列を作る
    if(buf == NULL && (buf = calloc(1,1)) == NULL)return set_error(data,"out of memory");
    *out = buf;
    *out_len = len;
    return 0;

no_memory:
    free(buf);
    return set_error(data,"out of memory");
}


//数字が続く間進める
static void skip_digits(JP_data *data){
    while(now_chr(data) >= '0' && now_chr(data) <= '9')data->read_pos.now_ptr++;
}

static bool is_digit_now(JP_data *data){
    return now_chr(data) >= '0' && now_chr(data) <= '9';
}

//数値を読む。JSONの書式: -? (0 | 1-9の後に数字) (.数字+)? ((e|E)(+|-)?数字+)?
//書式を確かめてから、その範囲を strtod で double に直す
static int parse_number(JP_value *value,JP_data *data){
    char *start = data->read_pos.now_ptr;

    if(now_chr(data) == '-')data->read_pos.now_ptr++;

    //整数部: 0 単独か、1-9 で始まる数字の並び(01 のような先頭の0は不正)
    if(now_chr(data) == '0'){
        data->read_pos.now_ptr++;
    }
    else if(now_chr(data) >= '1' && now_chr(data) <= '9'){
        skip_digits(data);
    }
    else return set_error(data,"invalid number");

    //小数部: '.' の後に数字が1つ以上
    if(now_chr(data) == '.'){
        data->read_pos.now_ptr++;
        if(!is_digit_now(data))return set_error(data,"invalid number");
        skip_digits(data);
    }

    //指数部: e か E、符号は任意、その後に数字が1つ以上
    if(now_chr(data) == 'e' || now_chr(data) == 'E'){
        data->read_pos.now_ptr++;
        if(now_chr(data) == '+' || now_chr(data) == '-')data->read_pos.now_ptr++;
        if(!is_digit_now(data))return set_error(data,"invalid number");
        skip_digits(data);
    }

    //書式は確認済みなので、strtod には確認した範囲だけを渡す
    char num_str[128];
    size_t num_len = (size_t)(data->read_pos.now_ptr - start);
    if(num_len >= sizeof num_str)return set_error(data,"number too long");
    memcpy(num_str,start,num_len);
    num_str[num_len] = '\0';

    value->type = JP_NUM;
    value->u.number = strtod(num_str,NULL);
    return 0;
}


//true / false / null を読む。word と完全に一致すれば type と boolean を設定する
static int parse_literal(JP_value *value,JP_data *data,const char *word,JP_TYPE type,bool boolean){
    size_t len = strlen(word);
    size_t rest = data->file_data.file_size - (size_t)(data->read_pos.now_ptr - data->file_data.file_str);
    if(rest < len || strncmp(data->read_pos.now_ptr,word,len) != 0)return set_error(data,"invalid literal");

    data->read_pos.now_ptr += len;
    value->type = type;
    if(type == JP_BOOL)value->u.boolean = boolean;
    return 0;
}



//---------------- 解放と表示 ----------------

//値とその子孫をすべて解放する(値そのものの領域は親が持つので解放しない)
static void free_value(JP_value *value){
    switch(value->type){
        case JP_STR:
            free(value->u.string.data);
            break;
        case JP_ARRY:
            for(size_t i = 0;i < value->u.array.count;i++)free_value(&value->u.array.items[i]);
            free(value->u.array.items);
            break;
        case JP_OBJ:
            for(size_t i = 0;i < value->u.object.count;i++){
                free(value->u.object.members[i].key);
                free_value(&value->u.object.members[i].value);
            }
            free(value->u.object.members);
            break;
        default:
            break;
    }
    value->type = JP_NULL;
}


//値を字下げして表示する(動作確認用)。文字列中の " や改行はエスケープし直さない
void JP_print(JP_value *value,int indent){
    if(value == NULL)return;
    switch(value->type){
        case JP_NULL: printf("null"); break;
        case JP_BOOL: printf(value->u.boolean ? "true" : "false"); break;
        case JP_NUM:  printf("%.17g",value->u.number); break;
        case JP_STR:  printf("\"%s\"",value->u.string.data); break;
        case JP_ARRY:
            printf("[\n");
            for(size_t i = 0;i < value->u.array.count;i++){
                printf("%*s",(indent + 1) * 2,"");
                JP_print(&value->u.array.items[i],indent + 1);
                printf(i + 1 < value->u.array.count ? ",\n" : "\n");
            }
            printf("%*s]",indent * 2,"");
            break;
        case JP_OBJ:
            printf("{\n");
            for(size_t i = 0;i < value->u.object.count;i++){
                printf("%*s\"%s\": ",(indent + 1) * 2,"",value->u.object.members[i].key);
                JP_print(&value->u.object.members[i].value,indent + 1);
                printf(i + 1 < value->u.object.count ? ",\n" : "\n");
            }
            printf("%*s}",indent * 2,"");
            break;
    }
}



JP_value *JP_get_value(JP_data *data,const char *key){
    return JP_search_value(&data->value,key);
}



static JP_value *JP_search_value(JP_value *value,const char *key){
    if(value->type == JP_OBJ){
        for(size_t i = 0;i < value->u.object.count;i++){
            if(strcmp(value->u.object.members[i].key,key) == 0){
                return &value->u.object.members[i].value;
            }
            JP_value *fd = JP_search_value(&value->u.object.members[i].value,key);
            if(fd != NULL)return fd;
        }
    }
    else if(value->type == JP_ARRY){
        for(size_t i = 0;i < value->u.array.count;i++){
            JP_value *fd = JP_search_value(&value->u.array.items[i],key);
            if(fd != NULL)return fd;
        }
    }
    
    return NULL;
}


int JP_option(JP_data *data,size_t s,JP_OPTION option){
    if(data == NULL)return -1;
    
    switch(option){
        case JP_OP_DEPHS:
            data->JP_MAX_DEPTH = s;
            break;
        
        default:
            break;
    }
    return 0;
}