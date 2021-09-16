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

char* hex_to_buffer(const char* pos, size_t length){
    char val[length];
    char* new_val;
     /* WARNING: no sanitization or error-checking whatsoever */
    for (size_t count = 0; count < sizeof val/sizeof *val; count++) {
        sscanf(pos, "%2hhx", &val[count]);
        pos += 2;
    }
    printf("0x");
    for(size_t count = 0; count < sizeof val/sizeof *val; count++)
        printf("%02x", val[count]);
    printf("\n");
    new_val = val;
    return new_val;
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
	    printf("code: %s, size: %ld\n", buf, len);

        const char* pos = "7661722073746F72656444617461203D20303B0A0A66756E6374696F6E20736574287829207B0A202073746F72656444617461203D20783B0A7D0A0A66756E6374696F6E206765742829207B0A202072657475726E2073746F726564446174613B0A7D0A";
        char* mybuf = hex_to_buffer(pos, len);
        char buf2[100] = {
            0x76,0x61,0x72,0x20,0x73,0x74,0x6f,0x72,0x65,0x64,0x44,0x61,0x74,0x61,0x20,0x3d,0x20,0x30,0x3b,0x0a,0x0a,0x66,0x75,0x6e,0x63,0x74,0x69,0x6f,0x6e,0x20,0x73,0x65,0x74,0x28,0x78,0x29,0x20,0x7b,0x0a,0x20,0x20,0x73,0x74,0x6f,0x72,0x65,0x64,0x44,0x61,0x74,0x61,0x20,0x3d,0x20,0x78,0x3b,0x0a,0x7d,0x0a,0x0a,0x66,0x75,0x6e,0x63,0x74,0x69,0x6f,0x6e,0x20,0x67,0x65,0x74,0x28,0x29,0x20,0x7b,0x0a,0x20,0x20,0x72,0x65,0x74,0x75,0x72,0x6e,0x20,0x73,0x74,0x6f,0x72,0x65,0x64,0x44,0x61,0x74,0x61,0x3b,0x0a,0x7d,0x0a    
        };
        printf("%s", buf2);
        // for(int i=0;i<sizeof(buf2);i++){
        //     char a = buf2[i];
        //     printf("%c", a);
        // }
        
        duk_push_lstring(ctx, (const char *) buf2, (duk_size_t) len);
    } else {
        duk_push_undefined(ctx);
    }
}