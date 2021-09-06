#include <stdio.h>
#include "duktape.h"

#define NUMBER_OF_STRING 4
#define MAX_STRING_SIZE 40
#define MAX_CONTRACT_VAR 100

char* int_to_string(int num,char *str);

static duk_ret_t native_print(duk_context *ctx) {
  printf("%s\n", duk_to_string(ctx, 0));
  return 0;  /* no return value (= undefined) */
}

// encode all properties in global inside duktape vm with number string like '0', '1', '2'....
static void encode_duktape_vm_props(duk_context *ctx){
   duk_eval_string_noresult(ctx,"var global = new Function('return this;')();i=0;for (var k in this) { global[i+'']=k; i++; }; global['length']=i;");
}

char* int_to_string(int num,char *str)
{
    int i = 0;
    if(num<0) 
    {
        num = -num;
        str[i++] = '-';
    } 
    
    do
    {
        str[i++] = num%10+48;
        num /= 10;    
    }while(num);
    
    str[i] = '\0';
    
    int j = 0;
    if(str[0]=='-')
    {
        j = 1;
        ++i; 
    }
    
    for(;j<i/2;j++)
    {
        str[j] = str[j] + str[i-1-j];
        str[i-1-j] = str[j] - str[i-1-j];
        str[j] = str[j] - str[i-1-j];
    } 
    
    return str;
}

static void save_contract_status(duk_context *ctx){
  duk_get_prop_string(ctx, -1, "length");
  duk_int_t length = duk_to_int(ctx, -1);
  duk_pop(ctx);
  for (int i = 0; i < length; ++i)
  {
   char string[16] = {0};
   int_to_string(i,string);
   duk_get_prop_string(ctx, -1, string);
   const char *prop_name = duk_to_string(ctx, -1);
   duk_pop(ctx);
   duk_get_prop_string(ctx, -1, prop_name);
   if (duk_check_type(ctx, -1, DUK_TYPE_NUMBER)) {
    duk_int_t prop_value = duk_to_int(ctx, -1);
    printf("%s: [%s]:%d\n", string, prop_name, prop_value);
    duk_pop(ctx);
   }else{
     const char* prop_value = duk_to_string(ctx, -1);
     printf("%s: [%s]:%s\n", string, prop_name, prop_value);
     duk_pop(ctx);
   }
  }
}

static void load_contract_status(duk_context *ctx){
  duk_get_prop_string(ctx, -1, "length");
  duk_int_t length = duk_to_int(ctx, -1);
  duk_pop(ctx);
  for (int i = 0; i < length; ++i)
  {
   char string[16] = {0};
   int_to_string(i,string);
   duk_get_prop_string(ctx, -1, string);
   const char *prop_name = duk_to_string(ctx, -1);
   duk_pop(ctx);
   duk_get_prop_string(ctx, -1, prop_name);
   if (duk_check_type(ctx, -1, DUK_TYPE_NUMBER)) {
    duk_int_t prop_value = duk_to_int(ctx, -1);
    duk_pop(ctx);
    duk_push_int(ctx, 99);
    duk_put_global_string(ctx, prop_name);
   }else{
     duk_pop(ctx);
   }
  }
}

static void push_file_as_string(duk_context *ctx, const char *filename) {
    FILE *f;
    size_t len;
    char buf[16384];

    f = fopen(filename, "rb");
    if (f) {
        len = fread((void *) buf, 1, sizeof(buf), f);
        fclose(f);
	     printf("code: %p, size: %ld\n", buf, len);
        duk_push_lstring(ctx, (const char *) buf, (duk_size_t) len);
    } else {
        duk_push_undefined(ctx);
    }
}