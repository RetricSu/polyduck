#include <stdio.h>
#include "duktape.h"
#include "duktape_utils.h"


int main(int argc, char *argv[]){
  duk_context *ctx = duk_create_heap_default();
  
  duk_push_c_function(ctx, native_print, 1 /*nargs*/);
  duk_put_global_string(ctx, "print");

  push_file_as_string(ctx, "contract.js");

  if (duk_peval(ctx) != 0) {
    printf("Error: %s\n", duk_safe_to_string(ctx, -1));
  }
  
  duk_pop(ctx); 
  duk_push_global_object(ctx);

  encode_duktape_vm_props(ctx);
  load_contract_status(ctx);
  save_contract_status(ctx);

 duk_get_prop_string(ctx, -1, "set");
 duk_push_int(ctx, 10);

 if (duk_pcall(ctx, 1 /*nargs*/) != 0) {
    printf("set Error: %s\n", duk_safe_to_string(ctx, -1));
 } else {
    printf("set result: %s\n", duk_safe_to_string(ctx, -1));
 }

 duk_pop(ctx);  /* pop result/error */

  duk_get_prop_string(ctx, -1 /*index*/, "get");
  if (duk_pcall(ctx, 0 /*nargs*/) != 0) {
     printf("get Error: %s\n", duk_safe_to_string(ctx, -1));
  } else {
     printf("get result: %s\n", duk_safe_to_string(ctx, -1));
  }
  duk_pop(ctx);  /* pop result/error */

  duk_destroy_heap(ctx);
  printf("finished\n");
  return 0;
}