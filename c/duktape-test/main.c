#include "duktape.h"

duk_context* create_duktape_vm(){
  duk_context *ctx = duk_create_heap_default();
  if (!ctx) { exit(1); }
  return ctx;
}

// push the contract code to duktape vm
static void duk_load_contract(duk_context *ctx, const uint8_t* code_data, const size_t code_size){
  duk_push_lstring(ctx, (const char *) code_data, (duk_size_t) code_size);
  if (duk_peval(ctx) != 0) {
    printf("Error: %s\n", duk_safe_to_string(ctx, -1));
  }
  duk_pop(ctx);  /* ignore result */
  duk_push_global_object(ctx);
}

int main(){
  duk_context *ctx = create_duktape_vm();
  char code_data[] = "const storedData = 0;function set(x) {storedData = x;}function get() {return storedData;}"; 
  const size_t code_size = sizeof(code_data);
  duk_push_lstring(ctx, (const char *) code_data, (duk_size_t) code_size);
  if (duk_peval(ctx) != 0) {
    printf("Error: %s\n", duk_safe_to_string(ctx, -1));
  }
  duk_pop(ctx);  /* ignore result */
  duk_push_global_object(ctx);
  printf("finished");
  return 0;
}