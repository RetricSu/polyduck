#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "ckb_syscalls.h"
#include "godwoken.h"

#include <ethash/keccak.hpp>
#include <evmc/evmc.h>
#include <evmc/evmc.hpp>
#include <evmone/evmone.h>
#include "duktape_utils.h"

#ifdef NO_DEBUG_LOG
int printf(const char *format, ...) { return 0; }
#else
int printf(const char *format, ...) {
  ckb_debug(format);
  return 0;
}
#endif

/* https://stackoverflow.com/a/1545079 */
#pragma push_macro("errno")
#undef errno
#include "gw_syscalls.h"
#pragma pop_macro("errno")

#include "common.h"

#include "sudt_utils.h"
#include "polyjuice_globals.h"
#include "polyjuice_errors.h"
#include "polyjuice_utils.h"

#ifdef GW_GENERATOR
#include "generator/secp256k1_helper.h"
#else
#include "validator/secp256k1_helper.h"
#endif
#include "contracts.h"


#define is_create(kind) ((kind) == EVMC_CREATE || (kind) == EVMC_CREATE2)
#define is_special_call(kind) \
  ((kind) == EVMC_CALLCODE || (kind) == EVMC_DELEGATECALL)

/* Max data buffer size: 24KB */
#define MAX_DATA_SIZE 24576
/* Max evm_memory size 512KB */
#define MAX_EVM_MEMORY_SIZE 524288
#define POLYJUICE_SYSTEM_PREFIX 0xFF
#define POLYJUICE_CONTRACT_CODE 0x01
#define POLYJUICE_DESTRUCTED 0x02

void polyjuice_build_system_key(uint32_t id, uint8_t polyjuice_field_type,
                                uint8_t key[GW_KEY_BYTES]) {
  memset(key, 0, GW_KEY_BYTES);
  memcpy(key, (uint8_t*)(&id), sizeof(uint32_t));
  key[4] = POLYJUICE_SYSTEM_PREFIX;
  key[5] = polyjuice_field_type;
}

void polyjuice_build_contract_code_key(uint32_t id, uint8_t key[GW_KEY_BYTES]) {
  polyjuice_build_system_key(id, POLYJUICE_CONTRACT_CODE, key);
}
void polyjuice_build_destructed_key(uint32_t id, uint8_t key[GW_KEY_BYTES]) {
  polyjuice_build_system_key(id, POLYJUICE_DESTRUCTED, key);
}

/* assume `account_id` already exists */
int gw_increase_nonce(gw_context_t *ctx, uint32_t account_id, uint32_t *new_nonce) {
  uint32_t old_nonce;
  int ret = ctx->sys_get_account_nonce(ctx, account_id, &old_nonce);
  if (ret != 0) {
    return ret;
  }
  uint32_t next_nonce = old_nonce + 1;

  uint8_t nonce_key[GW_KEY_BYTES];
  uint8_t nonce_value[GW_VALUE_BYTES];
  memset(nonce_value, 0, GW_VALUE_BYTES);
  gw_build_account_field_key(account_id, GW_ACCOUNT_NONCE, nonce_key);
  memcpy(nonce_value, (uint8_t *)(&next_nonce), 4);
  ret = ctx->_internal_store_raw(ctx, nonce_key, nonce_value);
  if (ret != 0) {
    return ret;
  }
  if (new_nonce != NULL) {
    *new_nonce = next_nonce;
  }
  return 0;
}

int handle_message(gw_context_t* ctx,
                   uint32_t parent_from_id,
                   uint32_t parent_to_id,
                   evmc_address *parent_destination,
                   const evmc_message* msg, struct evmc_result* res);
typedef int (*stream_data_loader_fn)(gw_context_t* ctx, long data_id,
                                     uint32_t* len, uint32_t offset,
                                     uint8_t* data);

struct evmc_host_context {
  gw_context_t* gw_ctx;
  const uint8_t* code_data;
  const size_t code_size;
  uint32_t from_id;
  uint32_t to_id;
  evmc_address sender;
  evmc_address destination;
  int error_code;
};

int load_account_script(gw_context_t* gw_ctx, uint32_t account_id,
                        uint8_t* buffer, uint32_t buffer_size,
                        mol_seg_t* script_seg) {
  debug_print_int("load_account_script, account_id:", account_id);
  int ret;
  uint64_t len = buffer_size;
  ret = gw_ctx->sys_get_account_script(gw_ctx, account_id, &len, 0, buffer);
  if (ret != 0) {
    ckb_debug("load account script failed");
    return ret;
  }
  script_seg->ptr = buffer;
  script_seg->size = len;
  if (MolReader_Script_verify(script_seg, false) != MOL_OK) {
    ckb_debug("load account script: invalid script");
    return FATAL_POLYJUICE;
  }
  return 0;
}

/**
   Message = [
     header     : [u8; 8]            0xff, 0xff, 0xff, "POLY", call_kind
     gas_limit  : u64                (little endian)
     gas_price  : u128               (little endian)
     value      : u128               (little endian)
     input_size : u32                (little endian)
     input_data : [u8; input_size]
   ]
 */
int parse_args(struct evmc_message* msg, uint128_t* gas_price,
               gw_context_t* ctx) {
  gw_transaction_context_t *tx_ctx = &ctx->transaction_context;
  debug_print_int("args_len", tx_ctx->args_len);
  if (tx_ctx->args_len < (8 + 8 + 16 + 16 + 4)) {
    ckb_debug("invalid polyjuice arguments data");
    return -1;
  }
  /* == Args decoder */
  size_t offset = 0;
  uint8_t* args = tx_ctx->args;

  /* args[0..8] magic header + call kind */
  static const uint8_t polyjuice_args_header[7] = {0xff, 0xff, 0xff, 'P', 'O', 'L', 'Y'};
  if (memcmp(polyjuice_args_header, args, 7) != 0) {
    debug_print_data("invalid polyjuice args header", args, 7);
    return -1;
  }
  debug_print_int("[call_kind]", args[7]);
  if (args[7] != EVMC_CALL && args[7] != EVMC_CREATE) {
    ckb_debug("invalid call kind");
    return -1;
  }
  evmc_call_kind kind = (evmc_call_kind)args[7];
  offset += 8;

  /* args[8..16] gas limit  */
  int64_t gas_limit;
  memcpy(&gas_limit, args + offset, sizeof(int64_t));
  offset += 8;
  debug_print_int("[gas_limit]", gas_limit);

  /* args[16..32] gas price */
  memcpy(gas_price, args + offset, sizeof(uint128_t));
  offset += 16;
  debug_print_int("[gas_price]", (int64_t)(*gas_price));

  /* args[32..48] transfer value */
  evmc_uint256be value{0};
  for (size_t i = 0; i < 16; i++) {
    value.bytes[31 - i] = args[offset + i];
  }
  offset += 16;

  /* args[48..52] */
  uint32_t input_size = *((uint32_t*)(args + offset));
  offset += 4;
  debug_print_int("[input_size]", input_size);

  if (input_size > tx_ctx->args_len) {
    /* If input size large enough may overflow `input_size + offset` */
    ckb_debug("input_size too large");
    return -1;
  }
  if (tx_ctx->args_len != (input_size + offset)) {
    ckb_debug("invalid polyjuice transaction");
    return -1;
  }

  /* args[52..52+input_size] */
  uint8_t* input_data = args + offset;

  int ret;
  evmc_address sender{0};
  uint8_t from_script_hash[32] = {0};
  ret = ctx->sys_get_script_hash_by_account_id(ctx, tx_ctx->from_id, from_script_hash);
  if (ret != 0) {
    debug_print_int("get from script hash failed", tx_ctx->from_id);
    return ret;
  }
  memcpy(sender.bytes, from_script_hash, 20);
  memcpy(g_tx_origin.bytes, sender.bytes, 20);

  msg->kind = kind;
  msg->flags = 0;
  msg->depth = 0;
  msg->value = value;
  msg->input_data = input_data;
  msg->input_size = input_size;
  msg->gas = gas_limit;
  msg->sender = sender;
  msg->destination = evmc_address{0};
  msg->create2_salt = evmc_bytes32{};
  return 0;
}

void release_result(const struct evmc_result* result) {
  if (result->output_data != NULL) {
    free((void*)result->output_data);
  }
  return;
}

int load_account_code(gw_context_t* gw_ctx, uint32_t account_id,
                      uint64_t* code_size, uint64_t offset, uint8_t* code) {

  int ret;
  uint8_t buffer[GW_MAX_SCRIPT_SIZE];
  mol_seg_t script_seg;
  ret = load_account_script(gw_ctx, account_id, buffer, GW_MAX_SCRIPT_SIZE, &script_seg);
  if (ret == GW_ERROR_ACCOUNT_NOT_EXISTS) {
    // This is an EoA or other kind of account, and not yet created
    debug_print_int("account not found", account_id);
    *code_size = 0;
    return 0;
  }
  if (ret != 0) {
    return ret;
  }
  mol_seg_t code_hash_seg = MolReader_Script_get_code_hash(&script_seg);
  mol_seg_t hash_type_seg = MolReader_Script_get_hash_type(&script_seg);
  mol_seg_t args_seg = MolReader_Script_get_args(&script_seg);
  mol_seg_t raw_args_seg = MolReader_Bytes_raw_bytes(&args_seg);
  if (raw_args_seg.size != CONTRACT_ACCOUNT_SCRIPT_ARGS_SIZE) {
    debug_print_int("[load_account_code] invalid account script", account_id);
    debug_print_int("[load_account_code] raw_args_seg.size", raw_args_seg.size);
    // This is an EoA or other kind of account
    *code_size = 0;
    return 0;
  }
  if (memcmp(code_hash_seg.ptr, g_script_code_hash, 32) != 0
      || *hash_type_seg.ptr != g_script_hash_type
      /* compare rollup_script_hash */
      || memcmp(raw_args_seg.ptr, g_rollup_script_hash, 32) != 0
      /* compare creator account id */
      || memcmp(&g_creator_account_id, raw_args_seg.ptr + 32, sizeof(uint32_t)) != 0
  ) {
    debug_print_int("[load_account_code] creator account id not match for account", account_id);
    // This is an EoA or other kind of account
    *code_size = 0;
    return 0;
  }

  debug_print_int("[load_account_code] account_id:", account_id);
  uint8_t key[32];
  uint8_t data_hash[32];
  polyjuice_build_contract_code_key(account_id, key);
  ret = gw_ctx->sys_load(gw_ctx, account_id, key, GW_KEY_BYTES, data_hash);
  if (ret != 0) {
    debug_print_int("[load_account_code] sys_load failed", ret);
    return ret;
  }

  bool is_data_hash_zero = true;
  for (size_t i = 0; i < 32; i++) {
    if (data_hash[i] != 0) {
      is_data_hash_zero = false;
      break;
    }
  }
  if (is_data_hash_zero) {
    ckb_debug("[load_account_code] data hash all zero");
    *code_size = 0;
    return 0;
  }

  uint64_t old_code_size = *code_size;
  debug_print_data("data_hash:", data_hash, 32);
  ret = gw_ctx->sys_load_data(gw_ctx, data_hash, code_size, offset, code);
  if (ret != 0) {
    ckb_debug("[load_account_code] sys_load_data failed");
    return ret;
  }
  if (*code_size > old_code_size) {
    debug_print_int("[load_account_code] code can't be larger than", MAX_DATA_SIZE);
    return -1;
  }
  return 0;
}

////////////////////////////////////////////////////////////////////////////
//// Callbacks
////////////////////////////////////////////////////////////////////////////
struct evmc_tx_context get_tx_context(struct evmc_host_context* context) {
  struct evmc_tx_context ctx{0};
  /* gas price = 1 */
  ctx.tx_gas_price.bytes[31] = 0x01;
  memcpy(ctx.tx_origin.bytes, g_tx_origin.bytes, 20);
  uint8_t coinbase_script_hash[32] = {0};
  int ret = context->gw_ctx->sys_get_script_hash_by_account_id(context->gw_ctx,
                                                               context->gw_ctx->block_info.block_producer_id,
                                                               coinbase_script_hash);
  if (ret != 0) {
    debug_print_int("get script hash by block producer id failed", context->gw_ctx->block_info.block_producer_id);
    context->error_code = ret;
  }
  memcpy(ctx.block_coinbase.bytes, coinbase_script_hash, 20);
  ctx.block_number = context->gw_ctx->block_info.number;
  /*
    block_timestamp      => second
    block_info.timestamp => millisecond
  */
  ctx.block_timestamp = context->gw_ctx->block_info.timestamp / 1000;
  /* Ethereum block gas limit */
  ctx.block_gas_limit = 12500000;
  /* 2500000000000000 */
  ctx.block_difficulty = {
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x08, 0xe1, 0xbc, 0x9b, 0xf0, 0x40, 0x00,
  };
  /* chain_id = creator_account_id */
  uint8_t *creator_account_id_ptr = (uint8_t *)(&g_creator_account_id);
  ctx.chain_id.bytes[31] = creator_account_id_ptr[0];
  ctx.chain_id.bytes[30] = creator_account_id_ptr[1];
  ctx.chain_id.bytes[29] = creator_account_id_ptr[2];
  ctx.chain_id.bytes[28] = creator_account_id_ptr[3];
  return ctx;
}

bool account_exists(struct evmc_host_context* context,
                    const evmc_address* address) {
  debug_print_data("BEGIN account_exists", address->bytes, 20);
  uint8_t script_hash[32] = {0};
  bool exists = true;
  int ret = context->gw_ctx->sys_get_script_hash_by_prefix(context->gw_ctx, (uint8_t *)address->bytes, 20, script_hash);
  if (ret != 0) {
    exists = false;
    return ret;
  }
  debug_print_int("END account_exists", (int)exists);
  return exists;
}

evmc_bytes32 get_storage(struct evmc_host_context* context,
                         const evmc_address* address, const evmc_bytes32* key) {
  ckb_debug("BEGIN get_storage");
  evmc_bytes32 value{0};
  int ret = context->gw_ctx->sys_load(context->gw_ctx, context->to_id,
                                      key->bytes, GW_KEY_BYTES, (uint8_t*)value.bytes);
  if (ret != 0) {
    debug_print_int("get_storage, sys_load failed", ret);
    if (is_fatal_error(ret)) {
      context->error_code = ret;
    }
  }
  ckb_debug("END get_storage");
  return value;
}

enum evmc_storage_status set_storage(struct evmc_host_context* context,
                                     const evmc_address* address,
                                     const evmc_bytes32* key,
                                     const evmc_bytes32* value) {
  ckb_debug("BEGIN set_storage");
  evmc_storage_status status = EVMC_STORAGE_ADDED;
  int ret = context->gw_ctx->sys_store(context->gw_ctx, context->to_id,
                                       key->bytes, GW_KEY_BYTES, value->bytes);
  if (ret != 0) {
    debug_print_int("sys_store failed", ret);
    if (is_fatal_error(ret)) {
      context->error_code = ret;
    }
    status = EVMC_STORAGE_UNCHANGED;
  }
  /* TODO: more rich evmc_storage_status */
  ckb_debug("END set_storage");
  return status;
}

size_t get_code_size(struct evmc_host_context* context,
                     const evmc_address* address) {
  ckb_debug("BEGIN get_code_size");
  int ret;
  uint32_t account_id = 0;
  ret = address_to_account_id(context->gw_ctx, address->bytes, &account_id);
  if (ret != 0) {
    ckb_debug("get contract account id failed");
    return 0;
  }
  uint8_t code[MAX_DATA_SIZE];
  uint64_t code_size = MAX_DATA_SIZE;
  ret = load_account_code(context->gw_ctx, account_id, &code_size, 0, code);
  if (ret != 0) {
    ckb_debug("load_account_code failed");
    context->error_code = ret;
    return 0;
  }
  ckb_debug("END get_code_size");
  return code_size;
}

evmc_bytes32 get_code_hash(struct evmc_host_context* context,
                           const evmc_address* address) {
  ckb_debug("BEGIN get_code_hash");
  evmc_bytes32 hash{0};
  int ret;
  uint32_t account_id = 0;
  ret = address_to_account_id(context->gw_ctx, address->bytes, &account_id);
  if (ret != 0) {
    ckb_debug("get contract account id failed");
    context->error_code = ret;
    return hash;
  }

  uint8_t code[MAX_DATA_SIZE];
  uint64_t code_size = MAX_DATA_SIZE;
  ret = load_account_code(context->gw_ctx, account_id, &code_size, 0, code);
  if (ret != 0) {
    ckb_debug("load_account_code failed");
    context->error_code = ret;
    return hash;
  }

  if (code_size > 0) {
    union ethash_hash256 hash_result = ethash::keccak256(code, code_size);
    memcpy(hash.bytes, hash_result.bytes, 32);
  }
  ckb_debug("END get_code_hash");
  return hash;
}

size_t copy_code(struct evmc_host_context* context, const evmc_address* address,
                 size_t code_offset, uint8_t* buffer_data, size_t buffer_size) {
  ckb_debug("BEGIN copy_code");
  int ret;
  uint32_t account_id = 0;
  ret = address_to_account_id(context->gw_ctx, address->bytes, &account_id);
  if (ret != 0) {
    ckb_debug("get contract account id failed");
    context->error_code = ret;
    return 0;
  }

  uint64_t code_size = (uint32_t)buffer_size;
  ret = load_account_code(context->gw_ctx, account_id, &code_size,
                          (uint32_t)code_offset, buffer_data);
  if (ret != 0) {
    ckb_debug("load account code failed");
    context->error_code = ret;
    return 0;
  }
  ckb_debug("END copy_code");
  return 0;
}

evmc_uint256be get_balance(struct evmc_host_context* context,
                           const evmc_address* address) {
  ckb_debug("BEGIN get_balance");
  int ret;
  evmc_uint256be balance{};
  uint128_t value_u128 = 0;
  ret = sudt_get_balance(context->gw_ctx, g_sudt_id, POLYJUICE_SHORT_ADDR_LEN, address->bytes, &value_u128);
  /* g_sudt_id account must exists */
  if (ret != 0) {
    ckb_debug("sudt_get_balance failed");
    context->error_code = FATAL_POLYJUICE;
    return balance;
  }
  uint8_t* value_ptr = (uint8_t*)(&value_u128);
  for (int i = 0; i < 16; i++) {
    balance.bytes[31 - i] = *(value_ptr + i);
  }
  ckb_debug("END get_balance");
  debug_print_data("address", address->bytes, 20);
  debug_print_int("balance", value_u128);
  return balance;
}

void selfdestruct(struct evmc_host_context* context,
                  const evmc_address* address,
                  const evmc_address* beneficiary) {
  int ret;
  uint128_t balance;
  ret = sudt_get_balance(context->gw_ctx, g_sudt_id, POLYJUICE_SHORT_ADDR_LEN, context->destination.bytes, &balance);
  /* g_sudt_id account must exists */
  if (ret != 0) {
    ckb_debug("get balance failed");
    context->error_code = ret;
    return;
  }
  if (balance > 0) {
    ret = sudt_transfer(context->gw_ctx, g_sudt_id,
                        POLYJUICE_SHORT_ADDR_LEN,
                        context->destination.bytes,
                        beneficiary->bytes,
                        balance);
    if (ret != 0) {
      ckb_debug("transfer beneficiary failed");
      context->error_code = ret;
      return;
    }
  }

  uint8_t raw_key[GW_KEY_BYTES];
  uint8_t value[GW_VALUE_BYTES];
  polyjuice_build_destructed_key(context->to_id, raw_key);
  memset(value, 1, GW_VALUE_BYTES);
  ret = context->gw_ctx->_internal_store_raw(context->gw_ctx, raw_key, value);
  if (ret != 0) {
    ckb_debug("update selfdestruct special key failed");
    context->error_code = ret;
  }
  ckb_debug("END selfdestruct");
  return;
}

struct evmc_result call(struct evmc_host_context* context,
                        const struct evmc_message* msg) {
  ckb_debug("BEGIN call");
  debug_print_int("msg.gas", msg->gas);
  debug_print_int("msg.depth", msg->depth);
  debug_print_int("msg.kind", msg->kind);
  debug_print_data("call.sender", msg->sender.bytes, 20);
  debug_print_data("call.destination", msg->destination.bytes, 20);
  int ret;
  struct evmc_result res;
  memset(&res, 0, sizeof(res));
  res.release = release_result;
  gw_context_t* gw_ctx = context->gw_ctx;

  precompiled_contract_gas_fn contract_gas;
  precompiled_contract_fn contract;
  if (match_precompiled_address(&msg->destination, &contract_gas, &contract)) {
    uint64_t gas_cost = 0;
    ret = contract_gas(msg->input_data, msg->input_size, &gas_cost);
    if (is_fatal_error(ret)) {
      context->error_code = ret;
    }
    if (ret != 0) {
      ckb_debug("call pre-compiled contract gas failed");
      res.status_code = EVMC_INTERNAL_ERROR;
      return res;
    }
    if ((uint64_t)msg->gas < gas_cost) {
      ckb_debug("call pre-compiled contract out of gas");
      res.status_code = EVMC_OUT_OF_GAS;
      return res;
    }
    res.gas_left = msg->gas - (int64_t)gas_cost;
    ret = contract(gw_ctx,
                   context->code_data, context->code_size,
                   msg->flags == EVMC_STATIC,
                   msg->input_data, msg->input_size,
                   (uint8_t**)&res.output_data, &res.output_size);
    if (is_fatal_error(ret)) {
      context->error_code = ret;
    }
    if (ret != 0) {
      debug_print_int("call pre-compiled contract failed", ret);
      res.status_code = EVMC_INTERNAL_ERROR;
      return res;
    }
    res.status_code = EVMC_SUCCESS;
  } else {
    ret = handle_message(gw_ctx, context->from_id, context->to_id, &context->destination, msg, &res);
    if (is_fatal_error(ret)) {
      /* stop as soon as possible */
      context->error_code = ret;
    }
    if (ret != 0) {
      debug_print_int("inner call failed (transfer/contract call contract)", ret);
      if (is_evmc_error(ret)) {
        res.status_code = (evmc_status_code)ret;
      } else {
        res.status_code = EVMC_INTERNAL_ERROR;
      }
    }
  }
  debug_print_int("call.res.status_code", res.status_code);
  ckb_debug("END call");

  return res;
}

evmc_bytes32 get_block_hash(struct evmc_host_context* context, int64_t number) {
  ckb_debug("BEGIN get_block_hash");
  evmc_bytes32 block_hash{};
  int ret = context->gw_ctx->sys_get_block_hash(context->gw_ctx, number,
                                                (uint8_t*)block_hash.bytes);
  if (ret != 0) {
    ckb_debug("sys_get_block_hash failed");
    context->error_code = ret;
    return block_hash;
  }
  ckb_debug("END get_block_hash");
  return block_hash;
}

void emit_log(struct evmc_host_context* context, const evmc_address* address,
              const uint8_t* data, size_t data_size,
              const evmc_bytes32 topics[], size_t topics_count) {
  ckb_debug("BEGIN emit_log");
  /*
    output[ 0..20]                     = callee_contract.address
    output[20..24]                     = data_size_u32
    output[24..24+data_size]           = data
    ouptut[24+data_size..28+data_size] = topics_count_u32
    ouptut[28+data_size..]             = topics
   */
  size_t output_size = 20 + (4 + data_size) + (4 + topics_count * 32);
  uint8_t* output = (uint8_t*)malloc(output_size);
  if (output == NULL) {
    context->error_code = -1;
    return;
  }
  uint32_t data_size_u32 = (uint32_t)(data_size);
  uint32_t topics_count_u32 = (uint32_t)(topics_count);

  uint8_t* output_current = output;
  memcpy(output_current, address->bytes, 20);
  output_current += 20;
  memcpy(output_current, (uint8_t*)(&data_size_u32), 4);
  output_current += 4;
  if (data_size > 0) {
    memcpy(output_current, data, data_size);
    output_current += data_size;
  }
  memcpy(output_current, (uint8_t*)(&topics_count_u32), 4);
  output_current += 4;
  for (size_t i = 0; i < topics_count; i++) {
    debug_print_data("log.topic", topics[i].bytes, 32);
    memcpy(output_current, topics[i].bytes, 32);
    output_current += 32;
  }
  int ret = context->gw_ctx->sys_log(context->gw_ctx, context->to_id,
                                     GW_LOG_POLYJUICE_USER, (uint32_t)output_size, output);
  if (ret != 0) {
    ckb_debug("sys_log failed");
    context->error_code = ret;
  }
  free(output);
  ckb_debug("END emit_log");
  return;
}

int check_destructed(gw_context_t* ctx, uint32_t to_id) {
  int ret;
  uint8_t destructed_raw_key[GW_KEY_BYTES];
  uint8_t destructed_raw_value[GW_VALUE_BYTES] = {0};
  polyjuice_build_destructed_key(to_id, destructed_raw_key);
  ret = ctx->_internal_load_raw(ctx, destructed_raw_key, destructed_raw_value);
  if (ret != 0) {
    debug_print_int("load destructed key failed", ret);
    return ret;
  }
  bool destructed = true;
  for (int i = 0; i < GW_VALUE_BYTES; i++) {
    if (destructed_raw_value[i] == 0) {
      destructed = false;
      break;
    }
  }
  if (destructed) {
    ckb_debug("call a contract that was already destructed");
    return FATAL_POLYJUICE;
  }
  return 0;
}

int load_globals(gw_context_t* ctx, uint32_t to_id, evmc_call_kind call_kind) {
  uint8_t buffer[GW_MAX_SCRIPT_SIZE];
  mol_seg_t script_seg;
  int ret = load_account_script(ctx, to_id, buffer, GW_MAX_SCRIPT_SIZE, &script_seg);
  if (ret != 0) {
    return ret;
  }
  mol_seg_t code_hash_seg = MolReader_Script_get_code_hash(&script_seg);
  mol_seg_t hash_type_seg = MolReader_Script_get_hash_type(&script_seg);
  mol_seg_t args_seg = MolReader_Script_get_args(&script_seg);
  mol_seg_t raw_args_seg = MolReader_Bytes_raw_bytes(&args_seg);

  memcpy(g_script_code_hash, code_hash_seg.ptr, 32);
  g_script_hash_type = *hash_type_seg.ptr;

  uint8_t creator_script_buffer[GW_MAX_SCRIPT_SIZE];
  mol_seg_t creator_script_seg;
  mol_seg_t creator_raw_args_seg;
  if (raw_args_seg.size == 36) {
    /* polyjuice creator account */
    g_creator_account_id = to_id;
    creator_raw_args_seg = raw_args_seg;
  } else if (raw_args_seg.size == CONTRACT_ACCOUNT_SCRIPT_ARGS_SIZE) {
    /* read creator account and then read sudt id from it */
    memcpy(&g_creator_account_id, raw_args_seg.ptr + 32, sizeof(uint32_t));
    int ret = load_account_script(ctx,
                                  g_creator_account_id,
                                  creator_script_buffer,
                                  GW_MAX_SCRIPT_SIZE,
                                  &creator_script_seg);
    if (ret != 0) {
      return ret;
    }
    mol_seg_t creator_code_hash_seg = MolReader_Script_get_code_hash(&creator_script_seg);
    mol_seg_t creator_hash_type_seg = MolReader_Script_get_hash_type(&creator_script_seg);
    mol_seg_t creator_args_seg = MolReader_Script_get_args(&creator_script_seg);
    creator_raw_args_seg = MolReader_Bytes_raw_bytes(&creator_args_seg);
    if (memcmp(creator_code_hash_seg.ptr, code_hash_seg.ptr, 32) != 0
        || *creator_hash_type_seg.ptr != *hash_type_seg.ptr
        /* compare rollup_script_hash */
        || memcmp(creator_raw_args_seg.ptr, raw_args_seg.ptr, 32) != 0
        || creator_raw_args_seg.size != 36) {
      debug_print_int("invalid creator account id in normal contract account script args", g_creator_account_id);
      return FATAL_POLYJUICE;
    }
  } else {
    debug_print_data("invalid to account script args", raw_args_seg.ptr, raw_args_seg.size);
    return FATAL_POLYJUICE;
  }

  memcpy(g_rollup_script_hash, creator_raw_args_seg.ptr, 32);
  memcpy(&g_sudt_id, creator_raw_args_seg.ptr + 32, sizeof(uint32_t));
  debug_print_data("rollup_script_hash", g_rollup_script_hash, 32);
  debug_print_int("sudt id", g_sudt_id);
  return 0;
}

int create_new_account(gw_context_t* ctx,
                       const evmc_message* msg,
                       uint32_t from_id,
                       uint32_t* to_id,
                       uint8_t* code_data,
                       size_t code_size) {
  if (code_size == 0) {
    ckb_debug("can't create new account by empty code data");
    return FATAL_POLYJUICE;
  }

  int ret = 0;
  uint8_t script_args[SCRIPT_ARGS_LEN];
  uint8_t data[128] = {0};
  uint32_t data_len = 0;
  if (msg->kind == EVMC_CREATE) {
    /* normal contract account script.args[36..36+20] content before hash
       Include:
       - [20 bytes] sender address
       - [4  bytes] sender nonce (NOTE: only use first 4 bytes (u32))

       Above data will be RLP encoded.
    */
    uint32_t nonce;
    /* from_id must already exists */
    ret = ctx->sys_get_account_nonce(ctx, from_id, &nonce);
    if (ret != 0) {
      return ret;
    }
    debug_print_data("sender", msg->sender.bytes, 20);
    debug_print_int("from_id", from_id);
    debug_print_int("nonce", nonce);
    rlp_encode_sender_and_nonce(&msg->sender, nonce, data, &data_len);
  } else if (msg->kind == EVMC_CREATE2) {
    /* CREATE2 contract account script.args[36..36+20] content before hash
       Include:
       - [ 1 byte ] 0xff (refer to ethereum)
       - [20 bytes] sender address
       - [32 bytes] create2_salt
       - [32 bytes] keccak256(init_code)
    */
    union ethash_hash256 hash_result = ethash::keccak256(code_data, code_size);
    data[0] = 0xff;
    memcpy(data + 1, msg->sender.bytes, 20);
    memcpy(data + 1 + 20, msg->create2_salt.bytes, 32);
    memcpy(data + 1 + 20 + 32, hash_result.bytes, 32);
    data_len = 1 + 20 + 32 + 32;
  } else {
    ckb_debug("unreachable");
    return FATAL_POLYJUICE;
  }

  /* contract account script.args
     Include:
     - [32 bytes] rollup type hash
     - [ 4 bytes] creator account id (chain id)
     - [20 bytes] keccak256(data)[12..]
  */
  memcpy(script_args, g_rollup_script_hash, 32);
  memcpy(script_args + 32, (uint8_t*)(&g_creator_account_id), 4);
  union ethash_hash256 data_hash_result = ethash::keccak256(data, data_len);
  memcpy(script_args + 32 + 4, data_hash_result.bytes + 12, 20);

  mol_seg_t new_script_seg;
  uint32_t new_account_id;
  ret = build_script(g_script_code_hash, g_script_hash_type, script_args,
                     SCRIPT_ARGS_LEN, &new_script_seg);
  if (ret != 0) {
    return ret;
  }
  uint8_t script_hash[32];
  blake2b_hash(script_hash, new_script_seg.ptr, new_script_seg.size);
  ret = ctx->sys_create(ctx, new_script_seg.ptr, new_script_seg.size, &new_account_id);
  if (ret != 0) {
    debug_print_int("sys_create error", ret);
    ckb_debug("create account failed assume account already created by meta_contract");
    ret = ctx->sys_get_account_id_by_script_hash(ctx, script_hash, &new_account_id);
    if (ret != 0) {
      return ret;
    }
  }
  free(new_script_seg.ptr);
  *to_id = new_account_id;
  memcpy((uint8_t *)msg->destination.bytes, script_hash, 20);
  debug_print_int(">> new to id", *to_id);
  return 0;
}

int handle_transfer(gw_context_t* ctx,
                    const evmc_message* msg,
                    uint8_t tx_origin_addr[20],
                    bool to_address_is_eoa) {
  int ret;
  uint8_t value_u128_bytes[16];
  for (int i = 0; i < 16; i++) {
    if (msg->value.bytes[i] != 0) {
      ckb_debug("transfer value can not larger than u128::max()");
      return FATAL_POLYJUICE;
    }
    value_u128_bytes[i] = msg->value.bytes[31 - i];
  }
  uint128_t value_u128 = *(uint128_t*)value_u128_bytes;
  debug_print_data("sender", msg->sender.bytes, 20);
  debug_print_data("destination", msg->destination.bytes, 20);
  debug_print_int("transfer value", value_u128);
  ret = sudt_transfer(ctx, g_sudt_id,
                      POLYJUICE_SHORT_ADDR_LEN,
                      msg->sender.bytes,
                      msg->destination.bytes,
                      value_u128);
  if (ret != 0) {
    ckb_debug("sudt_transfer failed");
    return ret;
  }

  if (msg->kind == EVMC_CALL && memcmp(msg->sender.bytes, tx_origin_addr, 20) == 0 && to_address_is_eoa) {
    ckb_debug("transfer value from eoa to eoa");
    return FATAL_POLYJUICE;
  }

  return 0;
}

void test_save_var(gw_context_t* gw_ctx, uint32_t account_id){
  const uint8_t k[8] = "123"; 
  const uint8_t * key = k;
  const uint64_t key_len = sizeof(key);
  const uint8_t value[GW_VALUE_BYTES] = "456";
  sys_store(gw_ctx, account_id, key, key_len, value);
  uint8_t return_value[GW_VALUE_BYTES]; 
  sys_load(gw_ctx, account_id, key, key_len, return_value);
  const char* val = (char *) return_value;
  ckb_debug("test load from gw:");
  ckb_debug(val);
}

uint8_t* calc_method_selector(const uint8_t* name, size_t size){
  union ethash_hash256 ethash = ethash::keccak256(name, size);
  uint8_t hash[32]; 
  memcpy(hash, ethash.bytes, 32);
  return ethash.bytes;
}

int store_js_contract(gw_context_t* ctx,
                        uint32_t to_id,
                        uint8_t* code,
                        uint32_t code_size
                      ) {
  int ret;
  uint8_t key[32];
  uint8_t data_hash[32];
  blake2b_hash(data_hash, code, code_size);
  polyjuice_build_contract_code_key(to_id, key);
  ckb_debug("BEGIN store data key for js contract");
  debug_print_data("data_hash", data_hash, 32);
  /* to_id must exists here */
  ret = ctx->sys_store(ctx, to_id, key, GW_KEY_BYTES, data_hash);
  if (ret != 0) {
    return ret;
  }
  ckb_debug("BEGIN store data for js contract");
  ret = ctx->sys_store_data(ctx, code_size, code);
  ckb_debug("END store data");
  if (ret != 0) {
    return ret;
  }
  return 0;
}

void hex_to_bin(const char * str, uint8_t * bytes, size_t blen)
{
   uint8_t  pos;
   uint8_t  idx0;
   uint8_t  idx1;

   // mapping of ASCII characters to hex values
   const uint8_t hashmap[] =
   {
     0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, // 01234567
     0x08, 0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 89:;<=>?
     0x00, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x00, // @ABCDEFG
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // HIJKLMNO
   };

   memset(bytes, 0, blen);
   for (pos = 0; ((pos < (blen*2)) && (pos < strlen(str))); pos += 2)
   {
      idx0 = ((uint8_t)str[pos+0] & 0x1F) ^ 0x10;
      idx1 = ((uint8_t)str[pos+1] & 0x1F) ^ 0x10;
      bytes[pos/2] = (uint8_t)(hashmap[idx0] << 4) | hashmap[idx1];
   };
}

int test_store_js_contract(
  gw_context_t* ctx,
  uint32_t to_id
){
  uint8_t code[MAX_DATA_SIZE] = {
    0x76,0x61,0x72,0x20,0x73,0x74,0x6f,0x72,0x65,0x64,0x44,0x61,0x74,0x61,0x20,0x3d,0x20,0x30,0x3b,0x0a,0x0a,0x66,0x75,0x6e,0x63,0x74,0x69,0x6f,0x6e,0x20,0x73,0x65,0x74,0x28,0x78,0x29,0x20,0x7b,0x0a,0x20,0x20,0x73,0x74,0x6f,0x72,0x65,0x64,0x44,0x61,0x74,0x61,0x20,0x3d,0x20,0x78,0x3b,0x0a,0x7d,0x0a,0x0a,0x66,0x75,0x6e,0x63,0x74,0x69,0x6f,0x6e,0x20,0x67,0x65,0x74,0x28,0x29,0x20,0x7b,0x0a,0x20,0x20,0x72,0x65,0x74,0x75,0x72,0x6e,0x20,0x73,0x74,0x6f,0x72,0x65,0x64,0x44,0x61,0x74,0x61,0x3b,0x0a,0x7d,0x0a    
  };
  int ret = store_js_contract(ctx, to_id, (uint8_t*) code, 100);
  if(ret != 0){
    return ret;
  } 
  return 0;
}

int store_js_contract_code(
  gw_context_t* ctx,
  uint32_t to_id
){
  uint8_t code[MAX_DATA_SIZE] = {
    0x76,0x61,0x72,0x20,0x73,0x74,0x6f,0x72,0x65,0x64,0x44,0x61,0x74,0x61,0x20,0x3d,0x20,0x30,0x3b,0x0a,0x0a,0x66,0x75,0x6e,0x63,0x74,0x69,0x6f,0x6e,0x20,0x73,0x65,0x74,0x28,0x78,0x29,0x20,0x7b,0x0a,0x20,0x20,0x73,0x74,0x6f,0x72,0x65,0x64,0x44,0x61,0x74,0x61,0x20,0x3d,0x20,0x78,0x3b,0x0a,0x7d,0x0a,0x0a,0x66,0x75,0x6e,0x63,0x74,0x69,0x6f,0x6e,0x20,0x67,0x65,0x74,0x28,0x29,0x20,0x7b,0x0a,0x20,0x20,0x72,0x65,0x74,0x75,0x72,0x6e,0x20,0x73,0x74,0x6f,0x72,0x65,0x64,0x44,0x61,0x74,0x61,0x3b,0x0a,0x7d,0x0a    
  };
  int ret = store_js_contract(ctx, to_id, (uint8_t*) code, 100);
  if(ret != 0){
    return ret;
  } 
  return 0;
}

uint64_t test_load_js_contract(
  gw_context_t* ctx,
  uint32_t to_id,
  uint8_t code[MAX_DATA_SIZE]
){
  uint64_t code_size = MAX_DATA_SIZE;
  int ret = load_account_code(ctx, to_id, &code_size, 0, code);
  if (ret != 0) {
    ckb_debug("load_account_code failed");
    return 0;
  }
  return code_size;
}

void test_duktape_vm(gw_context_t* gw_ctx, uint32_t to_id){
  // initialize duktape vm
  debug_print_int("[execute_in_duktape] init duktape vm instace", 1);
  duk_context* duk_ctx = create_duktape_vm();
  duk_push_c_function(duk_ctx, native_print, 1 /*nargs*/);
  duk_put_global_string(duk_ctx, "print");

  // load contract
  uint8_t code[MAX_DATA_SIZE];
  uint64_t code_size = test_load_js_contract(gw_ctx, to_id, code);
  ckb_debug("load js contract code...");
  ckb_debug((char*) code);
  debug_print_int("[execute_in_duktape] code size", code_size);
  char expected_code[MAX_DATA_SIZE] = {
    0x76,0x61,0x72,0x20,0x73,0x74,0x6f,0x72,0x65,0x64,0x44,0x61,0x74,0x61,0x20,0x3d,0x20,0x30,0x3b,0x0a,0x0a,0x66,0x75,0x6e,0x63,0x74,0x69,0x6f,0x6e,0x20,0x73,0x65,0x74,0x28,0x78,0x29,0x20,0x7b,0x0a,0x20,0x20,0x73,0x74,0x6f,0x72,0x65,0x64,0x44,0x61,0x74,0x61,0x20,0x3d,0x20,0x78,0x3b,0x0a,0x7d,0x0a,0x0a,0x66,0x75,0x6e,0x63,0x74,0x69,0x6f,0x6e,0x20,0x67,0x65,0x74,0x28,0x29,0x20,0x7b,0x0a,0x20,0x20,0x72,0x65,0x74,0x75,0x72,0x6e,0x20,0x73,0x74,0x6f,0x72,0x65,0x64,0x44,0x61,0x74,0x61,0x3b,0x0a,0x7d,0x0a  
  };
  ckb_debug(expected_code);

  duk_register_js_contract(duk_ctx, (const char*) expected_code, (size_t) code_size);

  if (duk_peval(duk_ctx) != 0) {
    ckb_debug("duktape vm Error");
    ckb_debug(duk_safe_to_string(duk_ctx, -1));
    // printf("duktape vm Error: %s\n", duk_safe_to_string(duk_ctx, -1));
  }
  duk_pop(duk_ctx); 
  duk_push_global_object(duk_ctx);
  encode_duktape_vm_props(duk_ctx);
  
  // load saving status of properties from godwoken sys storage
  load_contract_status(duk_ctx);

  //saving status of properties to godwoken sys storage
  save_contract_status(duk_ctx);

  /* Execute the code in JS VM */

  /* get method */
  duk_get_prop_string(duk_ctx, -1 /*index*/, "get");
  if (duk_pcall(duk_ctx, 0 /*nargs*/) != 0) {
    ckb_debug("duktape contract: get Error");
    ckb_debug(duk_safe_to_string(duk_ctx, -1));
    // printf("get Error: %s\n", duk_safe_to_string(duk_ctx, -1));
  } else {
    ckb_debug("duktape contract: get Result");
    ckb_debug(duk_safe_to_string(duk_ctx, -1)); 
    // printf("get result: %s\n", duk_safe_to_string(duk_ctx, -1));
  }
  duk_pop(duk_ctx);  /* pop result/error */

  /* set method */
  duk_get_prop_string(duk_ctx, -1, "set");
  duk_push_int(duk_ctx, 10);
  if (duk_pcall(duk_ctx, 1 /*nargs*/) != 0) {
     ckb_debug("duktape contract: set Error");
     ckb_debug(duk_safe_to_string(duk_ctx, -1));
     // printf("set Error: %s\n", duk_safe_to_string(duk_ctx, -1));
  } else {
     const char* result = duk_safe_to_string(duk_ctx, -1);
     ckb_debug("duktape contract: set Result");
     ckb_debug(result);
     // printf("set result: %s\n", duk_safe_to_string(duk_ctx, -1));
  }
  duk_pop(duk_ctx);  /* pop result/error */

  /* get method */
  duk_get_prop_string(duk_ctx, -1 /*index*/, "get");
  if (duk_pcall(duk_ctx, 0 /*nargs*/) != 0) {
    ckb_debug("duktape contract: get Error");
    ckb_debug(duk_safe_to_string(duk_ctx, -1));
    // printf("get Error: %s\n", duk_safe_to_string(duk_ctx, -1));
  } else {
    ckb_debug("duktape contract: get Result");
    ckb_debug(duk_safe_to_string(duk_ctx, -1)); 
    // printf("get result: %s\n", duk_safe_to_string(duk_ctx, -1));
  }
  duk_pop(duk_ctx);  /* pop result/error */

  printf("execute in duktape vm finished!\n");
  duk_destroy_heap(duk_ctx);

  //  res->status_code = EVMC_SUCCESS;
  //  res->gas_left = 100;
  //  res->output_data = NULL;
  //  res->output_size = 0;
  // duktape_vm_cleanup:
  //   duk_destroy_heap(duk_ctx); // destroy the VM instance
}

int duktape_dummy_int_parameter_4bytes_encode(uint8_t* value, int length){
  //int result = 0;
  //uint8_t* value_int;
  //memcpy(value_int, value, length);
  int value_int;
  sscanf((char*)value, "%d", &value_int);
  return value_int;
  // for(int i=0;i<length;i++){
  //   //result = result + (int)value_int[i] * pow(16, (length - i - 1)*2 );
  // }
  // return result;
}

int set_duktape_storage(gw_context_t* gw_ctx,
                        uint32_t to_id,
                                     uint8_t* key,
                                     uint8_t* value) {
  //ckb_debug("BEGIN duktape set_storage");
  int status = 0;
  int ret = gw_ctx->sys_store(gw_ctx, to_id,
                                       key, GW_KEY_BYTES, value);
  if (ret != 0) {
    debug_print_int("sys_store failed", ret);
    status = 1;
  }
  //ckb_debug("END duktape set_storage");
  return status;
}

uint8_t* get_duktape_storage(gw_context_t* gw_ctx, uint32_t to_id, uint8_t* key) {
  //ckb_debug("BEGIN get_storage");
  uint8_t* value{0};
  int ret = gw_ctx->sys_load(gw_ctx, to_id,
                                      key, GW_KEY_BYTES, value);
  if (ret != 0) {
    debug_print_int("get_duktape_storage, sys_load failed", ret);
  }
  //ckb_debug("END get_duktape_storage");
  return value;
}

static void save_duktape_contract_status(duk_context *ctx, gw_context_t* gw_ctx, uint32_t to_id){
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
    const char* prop_value = duk_to_string(ctx, -1);
    //ckb_debug(prop_name);
    //ckb_debug(prop_value);
    // ready to save
    set_duktape_storage(gw_ctx, to_id, (uint8_t*)prop_name, (uint8_t*)prop_value); 
    duk_pop(ctx);

    // uint8_t* v = get_duktape_storage(gw_ctx, to_id, (uint8_t*)prop_name);
    // ckb_debug("get the value just stored:");
    // ckb_debug((const char*)v);
   }else{
     const char* prop_value = duk_to_string(ctx, -1);
     //ckb_debug(prop_name);
     //ckb_debug(prop_value);
     // ready to save
     set_duktape_storage(gw_ctx, to_id, (uint8_t*)prop_name, (uint8_t*)prop_value); 
     duk_pop(ctx);

     // uint8_t* v = get_duktape_storage(gw_ctx, to_id, (uint8_t*)prop_name);
     // ckb_debug("get the value just stored:");
     // ckb_debug((const char*)v);
   }
  }
}

static void load_duktape_contract_status(duk_context *ctx, gw_context_t* gw_ctx, uint32_t to_id){
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
    // ckb_debug("load props from storage");
    // ckb_debug(prop_name);
    uint8_t* load_value = get_duktape_storage(gw_ctx, to_id, (uint8_t*)prop_name );
    //ckb_debug((const char*)load_value);
    int load_value_int = duktape_dummy_int_parameter_4bytes_encode(load_value, 4); // 4 bytes for int parameter 
    debug_print_int("load int props value from storage", load_value_int);
    duk_push_int(ctx, load_value_int);
    duk_put_global_string(ctx, prop_name);
   }else{
     duk_safe_to_string(ctx, -1);
     duk_pop(ctx);
   }
  }
}

int StringHextoHex(char *str, unsigned char *out, int *outlen)
{
    if (str == NULL || out == NULL)
        return -1;

    int i = 0, ret = 0;

    ret = (strlen(str) / (2 * sizeof(char))) + strlen(str) % (2 * sizeof(char));

    for (i = 0; i < ret; i++)
        sscanf(str + 2 * i, "%02X", (unsigned int *)(out + i));

    if (outlen!= NULL)
        *outlen = ret;

    return ret;
}

// initialize duktape vm
duk_context* init_duktape_vm(){
  ckb_debug("[execute_in_duktape] init duktape vm instace");
  duk_context* duk_ctx = create_duktape_vm();
  duk_push_c_function(duk_ctx, native_print, 1 /*nargs*/);
  duk_put_global_string(duk_ctx, "print");
  return duk_ctx;
}

uint64_t load_js_contract(
  gw_context_t* ctx,
  uint32_t to_id,
  uint8_t code[MAX_DATA_SIZE]
){
  uint64_t code_size = MAX_DATA_SIZE;
  int ret = load_account_code(ctx, to_id, &code_size, 0, code);
  if (ret != 0) {
    ckb_debug("load_account_code failed");
    return 0;
  }
  return code_size;
}

// load and initialize duktape js contract
void init_duktape_contract(gw_context_t* gw_ctx, duk_context* duk_ctx, uint32_t to_id){
  uint8_t code[MAX_DATA_SIZE];
  uint64_t code_size = load_js_contract(gw_ctx, to_id, code);
  ckb_debug("load js contract code...");
  //ckb_debug((char*) code);
  debug_print_data("[execute_in_duktape] load code", code, code_size);
  debug_print_int("[execute_in_duktape] code size", code_size);

  duk_register_js_contract(duk_ctx, (const char*) code, (size_t) code_size);

  if (duk_peval(duk_ctx) != 0) {
    ckb_debug("duktape vm Error");
    ckb_debug(duk_safe_to_string(duk_ctx, -1));
  }
  duk_pop(duk_ctx); 
  duk_push_global_object(duk_ctx);
  encode_duktape_vm_props(duk_ctx);

  // load saving status of properties from godwoken sys storage 
  load_duktape_contract_status(duk_ctx, gw_ctx, to_id);
}

// only support one int type parameter for now
const char* execute_contract_method_with_dummy_parameters(duk_context* duk_ctx, char* method_name, int parameter, int status, gw_context_t* gw_ctx, uint32_t to_id){
  duk_get_prop_string(duk_ctx, -1 /*index*/, method_name);
  duk_push_int(duk_ctx, parameter);
  if (duk_pcall(duk_ctx, 1 /*nargs*/) != 0) {
    const char* err = duk_safe_to_string(duk_ctx, -1);
    ckb_debug("[execute in duktape] contract: execute method Error");
    ckb_debug(err);
    duk_pop(duk_ctx);  /* pop result/error */
    status = 1;
    return err;
  }

  if (duk_check_type(duk_ctx, -1, DUK_TYPE_NUMBER)){
    const char* result = (const char*) duk_to_uint32(duk_ctx, -1);
    debug_print_data("[execute in duktape] execute method with_dummy_parameters result (number)", (const uint8_t*) result, (uint32_t) sizeof(result) );
    ckb_debug(result);
    duk_pop(duk_ctx);  /* pop result/error */ 
    // saving status of properties to godwoken sys storage
    save_duktape_contract_status(duk_ctx, gw_ctx, to_id);
    status = 0;
    return result;
  }else{
    const char* result = duk_safe_to_string(duk_ctx, -1);
    debug_print_data("[execute in duktape] execute method with_dummy_parameters result", (const uint8_t*) result, (uint32_t) sizeof(result) );
    ckb_debug(result);
    duk_pop(duk_ctx);  /* pop result/error */
    // saving status of properties to godwoken sys storage
    save_duktape_contract_status(duk_ctx, gw_ctx, to_id);
    status = 0;
    return result;
  }
}

const char* execute_contract_method(duk_context* duk_ctx, char* method_name, int status, uint8_t* execute_result, gw_context_t* gw_ctx, uint32_t to_id){
  duk_get_prop_string(duk_ctx, -1 /*index*/, method_name);
  if (duk_pcall(duk_ctx, 0 /*nargs*/) != 0) {
    const char* err = duk_safe_to_string(duk_ctx, -1);
    ckb_debug("[execute in duktape] contract: execute method Error");
    ckb_debug(err);
    duk_pop(duk_ctx);  /* pop result/error */
    status = 1;
    return err;
  }

 // const char* result = duk_safe_to_string(duk_ctx, -1);
 // debug_print_data("[execute in duktape] execute method result", (const uint8_t*) result, (uint32_t) sizeof(result) );
 // ckb_debug(result);
 // duk_pop(duk_ctx);  /* pop result/error */
 // // saving status of properties to godwoken sys storage
 // save_duktape_contract_status(duk_ctx, gw_ctx, to_id);
 // status = 0;
 // return result;

  if (duk_check_type(duk_ctx, -1, DUK_TYPE_NUMBER)){
    const char* result = duk_safe_to_string(duk_ctx, -1);
    debug_print_data("[execute in duktape] execute method result (number)", (const uint8_t*) result, (uint32_t) sizeof(result));
    ckb_debug(result);
    duk_pop(duk_ctx);  /* pop result/error */

    //duk_to_int(duk_ctx, -1);
    //const char* result 
    // double a = (double)duk_get_number(duk_ctx, -1);
    // char arr[sizeof(a)];
    // memcpy(arr,&a,sizeof(a));

    debug_print_data("result: ", (const uint8_t*)result, (uint32_t) sizeof(result));

    //covert result to output(which is a simple 4 bytes hex value)
    int result_int;

    // char* data = (char*)result;
    // size_t data_len = sizeof(result); 
    // char* g_debug_buffer;
    // int offset = 0;
    // for (size_t i = 0; i < data_len; i++) {
    //   offset += sprintf(g_debug_buffer + offset, "%02x", data[i]);
    // }
    // g_debug_buffer[offset] = '\0';
    // ckb_debug(g_debug_buffer);

    sscanf(result, "%d", &result_int);
    debug_print_int("result int:", result_int);
    char strHexTemp[8];
    sprintf(strHexTemp, "%x", result_int);
    char* hexstr = strHexTemp;
    ckb_debug(hexstr);
    unsigned char out[8];
    int outlen = 0;
    StringHextoHex(hexstr, out, &outlen);

    debug_print_data("[execute in duktape] execute method result (hex number)", (const uint8_t*) out, (uint32_t) sizeof(out));
    execute_result = (uint8_t*) out;
    //debug_print_data("[execute in duktape] execute method result (hex number)", (const uint8_t*) execute_result, (uint32_t) sizeof(execute_result));
    // const uint8_t* final_output =  (const uint8_t*) out;
    // char* output;
    // char* data = (char*)result;
    // int offset = 0;
    // for (size_t i = 0; i < sizeof(data); i++) {
    //   offset += sprintf(output + offset, "%02x", data[i]);
    // }
    // output[offset] = '\0';
    // ckb_debug(output);

    // saving status of properties to godwoken sys storage
    //save_duktape_contract_status(duk_ctx, gw_ctx, to_id);
    status = 0;

    return (const char*)hexstr;
  }else{
    const char* result = duk_safe_to_string(duk_ctx, -1);
    debug_print_data("[execute in duktape] execute method result", (const uint8_t*) result, (uint32_t) sizeof(result) );
    ckb_debug(result);
    duk_pop(duk_ctx);  /* pop result/error */
    // saving status of properties to godwoken sys storage
    save_duktape_contract_status(duk_ctx, gw_ctx, to_id);
    status = 0;
    return result;
  }
}

// the js vm
int execute_in_duktape(gw_context_t* ctx,
                      evmc_message* msg,
                      uint32_t _parent_from_id,
                      uint32_t from_id,
                      uint32_t to_id,
                      const uint8_t* code_data,
                      const size_t code_size,
                      struct evmc_result* res) {
  int ret = 0;
  evmc_address sender = msg->sender;
  evmc_address destination = msg->destination;
  struct evmc_host_context context {ctx, code_data, code_size, from_id, to_id, sender, destination, 0};
  struct evmc_vm* vm = evmc_create_evmone();
  struct evmc_host_interface interface = {account_exists, get_storage,    set_storage,    get_balance,
                                          get_code_size,  get_code_hash,  copy_code,      selfdestruct,
                                          call,           get_tx_context, get_block_hash, emit_log};


  debug_print_int("[execute_in_duktape] code size", code_size);
  debug_print_int("[execute_in_duktape] input_size", msg->input_size);
  debug_print_data("msg->input_data", msg->input_data, msg->input_size);

  duk_context* duk_ctx = init_duktape_vm();

  if(is_create(msg->kind)){
     res->output_data = code_data;
     res->output_size = code_size; 
     ret = EVMC_SUCCESS;
     goto duktape_vm_cleanup;
  }else{
    int status;
    uint8_t method_exec_result[32] = {0};

    init_duktape_contract(ctx, duk_ctx, to_id);
    const char* method_name = (const char*) msg->input_data;
    debug_print_data("extract method name from input_data", msg->input_data, 4);
    ckb_debug(method_name);

    if(msg->input_size >= 36){ // has method parameter
      uint8_t* method_parameter;
      // we use dummy parameter encoding which suppose last 4 bytes is a hex value for simplity
      memcpy(method_parameter, &(msg->input_data)[32], 4);
      debug_print_data("extract method parameter from input_data", (const uint8_t*) method_parameter, 4);
      int parameter_int = (int)method_parameter[0] * pow(16, 6) + (int)method_parameter[1] * pow(16, 4) + (int)method_parameter[2] * pow(16, 2) + (int)method_parameter[3];
      debug_print_int("covert parameter to int", parameter_int);
      const char* exec_result = execute_contract_method_with_dummy_parameters(duk_ctx, (char*)method_name, parameter_int, status, ctx, to_id);
      ckb_debug(exec_result);
      //memcpy(&method_exec_result, &exec_result, sizeof(exec_result));
    }else{
      uint8_t tr[8];
      uint8_t* test_result = tr;
      const char* exec_result = execute_contract_method(duk_ctx, (char*)method_name, status, test_result, ctx, to_id);
      debug_print_data("[execute in duktape] execute method result (hex number)", (const uint8_t*) test_result, (uint32_t) sizeof(test_result));
      //memcpy(&method_exec_result, &exec_result, sizeof(exec_result));
    }

    if(status != 0){
      res->status_code = EVMC_REVERT;
    }
    if(status == 0){
      res->status_code = EVMC_SUCCESS;
      res->output_data = (const uint8_t*) method_exec_result;
      res->output_size = sizeof(method_exec_result);
    }

    if (res->status_code != EVMC_SUCCESS && res->status_code != EVMC_REVERT) {
      res->output_data = NULL;
      res->output_size = 0;
    }
  }

  if (context.error_code != 0) {
    debug_print_int("[execute_in_duktape] context.error_code", context.error_code);
    ret = context.error_code;
    goto duktape_vm_cleanup;
  }
  if (res->gas_left < 0) {
    ckb_debug("[execute_in_duktape] gas not enough");
    ret = EVMC_OUT_OF_GAS;
    goto duktape_vm_cleanup;
  }

duktape_vm_cleanup:
  // duk_destroy_heap(duk_ctx); // destroy the VM instance
  ckb_debug("execute in duktape vm finished!\n");
  duk_destroy_heap(duk_ctx);
  evmc_destroy(vm);
  return ret;
}

int execute_in_evmone(gw_context_t* ctx,
                      evmc_message* msg,
                      uint32_t _parent_from_id,
                      uint32_t from_id,
                      uint32_t to_id,
                      const uint8_t* code_data,
                      const size_t code_size,
                      struct evmc_result* res) {
  int ret = 0;
  evmc_address sender = msg->sender;
  evmc_address destination = msg->destination;
  struct evmc_host_context context {ctx, code_data, code_size, from_id, to_id, sender, destination, 0};
  struct evmc_vm* vm = evmc_create_evmone();
  struct evmc_host_interface interface = {account_exists, get_storage,    set_storage,    get_balance,
                                          get_code_size,  get_code_hash,  copy_code,      selfdestruct,
                                          call,           get_tx_context, get_block_hash, emit_log};
  /* Execute the code in EVM */
  debug_print_int("[execute_in_evmone] code size", code_size);
  debug_print_int("[execute_in_evmone] input_size", msg->input_size);
  debug_print_int("[execute_in_evmone] input_data", 1);
  //ckb_debug((const char*) msg->input_data);
  ckb_debug("load done for msg->input_data");
  *res = vm->execute(vm, &interface, &context, EVMC_MAX_REVISION, msg, code_data, code_size);
  if (res->status_code != EVMC_SUCCESS && res->status_code != EVMC_REVERT) {
    res->output_data = NULL;
    res->output_size = 0;
  }
  if (context.error_code != 0) {
    debug_print_int("[execute_in_evmone] context.error_code", context.error_code);
    ret = context.error_code;
    goto evmc_vm_cleanup;
  }
  if (res->gas_left < 0) {
    ckb_debug("[execute_in_evmone] gas not enough");
    ret = EVMC_OUT_OF_GAS;
    goto evmc_vm_cleanup;
  }

evmc_vm_cleanup:
  evmc_destroy(vm); // destroy the VM instance
  return ret;
}

int store_contract_code(gw_context_t* ctx,
                        uint32_t to_id,
                        struct evmc_result* res) {
  int ret;
  uint8_t key[32];
  uint8_t data_hash[32];
  blake2b_hash(data_hash, (uint8_t*)res->output_data, res->output_size);
  polyjuice_build_contract_code_key(to_id, key);
  ckb_debug("BEGIN store data key");
  debug_print_data("data_hash", data_hash, 32);
  /* to_id must exists here */
  ret = ctx->sys_store(ctx, to_id, key, GW_KEY_BYTES, data_hash);
  if (ret != 0) {
    return ret;
  }
  ckb_debug("BEGIN store data");
  ret = ctx->sys_store_data(ctx, res->output_size, (uint8_t*)res->output_data);
  ckb_debug("END store data");
  if (ret != 0) {
    return ret;
  }
  return 0;
}

/**
 * call/create contract
 *
 * Must allocate an account id before create contract
 */
int handle_message(gw_context_t* ctx,
                   uint32_t parent_from_id,
                   uint32_t parent_to_id,
                   evmc_address *parent_destination,
                   const evmc_message* msg_origin, struct evmc_result* res) {
  static const evmc_address zero_address{0};

  ckb_debug("[handle_message] BEGIN handle_message");
  evmc_message msg = *msg_origin;
  int ret;

  bool to_address_exists = false;
  uint32_t to_id = 0;
  uint32_t from_id;
  // TODO: passing this value from function argument
  uint8_t from_script_hash[32] = {0};
  uint8_t to_script_hash[32] = {0};
  if (memcmp(zero_address.bytes, msg.destination.bytes, 20) != 0) {
    ret = ctx->sys_get_script_hash_by_prefix(ctx, msg.destination.bytes, 20, to_script_hash);
    if (ret == 0) {
      to_address_exists = true;
      ret = ctx->sys_get_account_id_by_script_hash(ctx, to_script_hash, &to_id);
      if (ret != 0) {
        debug_print_data("[handle_message] get to account id by script hash failed", to_script_hash, 32);
        return ret;
      }
    }
  } else {
    /* When msg.destination is zero
        1. if is_create(msg.kind) == true, we will run msg.input_data as code in EVM
        2. if is_create(msg.kind) == false, code_size must be zero, so it's simply a transfer action
    */
  }

  ret = ctx->sys_get_script_hash_by_prefix(ctx, msg.sender.bytes, 20, from_script_hash);
  if (ret == 0) {
    ret = ctx->sys_get_account_id_by_script_hash(ctx, from_script_hash, &from_id);
    if (ret != 0) {
      debug_print_data("[handle_message] get from account id by script hash failed", from_script_hash, 32);
      return ret;
    }
  } else {
    debug_print_data("[handle_message] get sender script hash failed", msg.sender.bytes, 20);
    return ret;
  }

  /* an assert */
  if (msg.kind == EVMC_DELEGATECALL && from_id != parent_from_id) {
    debug_print_int("[handle_message] from_id", from_id);
    debug_print_int("[handle_message] parent_from_id", parent_from_id);
    ckb_debug("[handle_message] from id != parent from id");
    return FATAL_POLYJUICE;
  }

  /* Check if target contract is destructed */
  if (!is_create(msg.kind) && to_address_exists) {
    ret = check_destructed(ctx, to_id);
    if (ret != 0) {
      return ret;
    }
  }

  /* Load contract code from evmc_message or by sys_load_data */
  uint8_t* code_data = NULL;
  size_t code_size = 0;
  uint8_t code_data_buffer[MAX_DATA_SIZE];
  uint64_t code_size_u32 = MAX_DATA_SIZE;
  if (is_create(msg.kind)) {
    /* use input as code */
    code_data = (uint8_t*)msg.input_data;
    code_size = msg.input_size;
    msg.input_data = NULL;
    msg.input_size = 0;
  } else if (to_address_exists) {
    /* call kind: CALL/CALLCODE/DELEGATECALL */
    ret = load_account_code(ctx, to_id, &code_size_u32, 0, code_data_buffer);
    if (ret != 0) {
      return ret;
    }
    if (code_size_u32 == 0) {
      debug_print_int("[handle_message] empty contract code for account (EoA account)", to_id);
      code_data = NULL;
    } else {
      code_data = code_data_buffer;
    }
    code_size = (size_t)code_size_u32;
  } else {
    // Call non-exists address
  }

  /* Handle special call: CALLCODE/DELEGATECALL */
  if (is_special_call(msg.kind)) {
    /* This action must after load the contract code */
    to_id = parent_to_id;
    if (parent_destination == NULL) {
      ckb_debug("[handle_message] parent_destination is NULL");
      return FATAL_POLYJUICE;
    }
    memcpy(msg.destination.bytes, parent_destination->bytes, 20);
  }

  /* Create new account by script */
  /* NOTE: to_id may be rewritten */
  if (is_create(msg.kind)) {
    ret = create_new_account(ctx, &msg, from_id, &to_id, code_data, code_size);
    if (ret != 0) {
      return ret;
    }
    to_address_exists = true;

    /* It's a creation polyjuice transaction */
    if (parent_from_id == UINT32_MAX && parent_to_id == UINT32_MAX) {
      g_created_id = to_id;
      memcpy(g_created_address, msg.destination.bytes, 20);
    }

    /* Increase from_id's nonce:
         1. Must increase nonce after new address created and before run vm
         2. Only increase contract account's nonce when it create contract (https://github.com/ethereum/EIPs/blob/master/EIPS/eip-161.md)
     */
    ret = gw_increase_nonce(ctx, from_id, NULL);
    if (ret != 0) {
      debug_print_int("increase nonce failed", ret);
      return ret;
    }
  }

  /* Handle transfer logic.
     NOTE: MUST do this before vm.execute and after to_id finalized */
  bool to_address_is_eoa = !to_address_exists || (to_address_exists && code_size == 0);
  ret = handle_transfer(ctx, &msg, (uint8_t *)g_tx_origin.bytes, to_address_is_eoa);
  if (ret != 0) {
    return ret;
  }

  debug_print_int("[handle_message] msg.kind", msg.kind);
  /* NOTE: msg and res are updated */
  if (to_address_exists && code_size > 0 && (is_create(msg.kind) || msg.input_size > 0)) {
    printf("ready to execute in both vms");
    
    ret = execute_in_duktape(ctx, &msg, parent_from_id, from_id, to_id, code_data, code_size, res);
    if (ret != 0) {
      return ret;
    }
  } else {
    ckb_debug("[handle_message] Don't run evm and return empty data");
    res->output_data = NULL;
    res->output_size = 0;
    res->gas_left = msg.gas;
    res->status_code = EVMC_SUCCESS;
  }

  /* Store contract code though syscall */
  if (is_create(msg.kind)) {
    // TODO: check code length < MAX_DATA_SIZE
    debug_print_data("ready to store js contract code", res->output_data, res->output_size);
    int ret = store_js_contract(ctx, to_id, (uint8_t*)res->output_data, res->output_size);
    if(ret != 0){
      return ret;
    } 
    return 0;
  }

  /* Rewrite create_address when call kind is CREATE/CREATE2 */
  if (is_create(msg.kind)) {
    memcpy(res->create_address.bytes, msg.destination.bytes, 20);
  }

  debug_print_int("[handle_message] output size", res->output_size);
  debug_print_int("[handle_message] gas left", res->gas_left);
  uint32_t used_memory;
  memcpy(&used_memory, res->padding, sizeof(uint32_t));
  debug_print_int("[handle_message] used_memory(Bytes)", used_memory);
  debug_print_int("[handle_message] status_code", res->status_code);
  ckb_debug("[handle_message] END handle_message");
  return (int)res->status_code;
}

int emit_evm_result_log(gw_context_t* ctx, const uint64_t gas_used, const int status_code) {
  /*
    data = { gasUsed: u64, cumulativeGasUsed: u64, contractAddress: [u8;20], status_code: u32 }

    data[ 0.. 8] = gas_used
    data[ 8..16] = cumulative_gas_used
    data[16..36] = created_address ([0u8; 20] means not created)
    data[36..40] = status_code (EVM status_code)
   */
  uint64_t cumulative_gas_used = gas_used;
  uint32_t status_code_u32 = (uint32_t)status_code;

  uint32_t data_size = 8 + 8 + 20 + 4;
  uint8_t data[8 + 8 + 20 + 4] = {0};
  uint8_t *ptr = data;
  memcpy(ptr, (uint8_t *)(&gas_used), 8);
  ptr += 8;
  memcpy(ptr, (uint8_t *)(&cumulative_gas_used), 8);
  ptr += 8;
  memcpy(ptr, (uint8_t *)(&g_created_address), 20);
  ptr += 20;
  memcpy(ptr, (uint8_t *)(&status_code_u32), 4);
  ptr += 4;

  /* NOTE: if create account failed the `to_id` will also be `context->to_id` */
  uint32_t to_id = g_created_id == UINT32_MAX ? ctx->transaction_context.to_id : g_created_id;
  /* to_id must already exists here */
  int ret = ctx->sys_log(ctx, to_id, GW_LOG_POLYJUICE_SYSTEM, data_size, data);
  if (ret != 0) {
    debug_print_int("sys_log evm result failed", ret);
    return ret;
  }
  return 0;
}

int clean_evmc_result_and_return(evmc_result *res, int code) {
  if (res->release) res->release(res);
  return code;
}

int run_polyjuice() {
#ifndef NO_DEBUG_LOG
  // init buffer for debug_print
  char buffer[DEBUG_BUFFER_SIZE];
  g_debug_buffer = buffer;
#endif

  int ret;

  /* prepare context */
  gw_context_t context;
  ret = gw_context_init(&context);
  if (ret != 0) {
    return ret;
  }

  evmc_message msg;
  uint128_t gas_price;
  /* Parse message */
  ckb_debug("BEGIN parse_message()");
  ret = parse_args(&msg, &gas_price, &context);
  ckb_debug("END parse_message()");
  if (ret != 0) {
    return ret;
  }

  /* Load: validator_code_hash, hash_type, sudt_id */
  ret = load_globals(&context, context.transaction_context.to_id, msg.kind);
  if (ret != 0) {
    return ret;
  }

  /* Fill msg.destination after load globals */
  if (msg.kind != EVMC_CREATE) {
    uint8_t script_hash[32] = {0};
    ret = context.sys_get_script_hash_by_account_id(&context,context.transaction_context.to_id, script_hash);
    if (ret != 0) {
      return ret;
    }
    memcpy(msg.destination.bytes, script_hash, 20);
  }

  uint8_t evm_memory[MAX_EVM_MEMORY_SIZE];
  init_evm_memory(evm_memory, MAX_EVM_MEMORY_SIZE);

  struct evmc_result res;
  memset(&res, 0, sizeof(evmc_result));
  res.status_code = EVMC_FAILURE; // Generic execution failure

  int ret_handle_message = handle_message(&context, UINT32_MAX, UINT32_MAX, NULL, &msg, &res);
  uint64_t gas_used = (uint64_t)(msg.gas - res.gas_left);
  ret = emit_evm_result_log(&context, gas_used, res.status_code);
  if (ret != 0) {
    ckb_debug("emit_evm_result_log failed");
    return clean_evmc_result_and_return(&res, ret);
  }
  if (ret_handle_message != 0) {
    ckb_debug("handle message failed");
    return clean_evmc_result_and_return(&res, ret_handle_message);
  }

  ret = context.sys_set_program_return_data(&context, (uint8_t*)res.output_data,
                                            res.output_size);
  if (ret != 0) {
    ckb_debug("set return data failed");
    return clean_evmc_result_and_return(&res, ret);
  }

  /* Handle transaction fee */
  if (res.gas_left < 0) {
    ckb_debug("gas not enough");
    return clean_evmc_result_and_return(&res, -1);
  }
  if (msg.gas < res.gas_left) {
    debug_print_int("msg.gas", msg.gas);
    debug_print_int("res.gas_left", res.gas_left);
    ckb_debug("unreachable!");
    return clean_evmc_result_and_return(&res, -1);
  }
  uint128_t fee = gas_price * (uint128_t)gas_used;
  debug_print_int("gas limit", msg.gas);
  debug_print_int("gas left", res.gas_left);
  debug_print_int("gas price", gas_price);
  debug_print_int("fee", fee);

  uint32_t used_memory;
  memcpy(&used_memory, res.padding, sizeof(uint32_t));
  debug_print_int("[run_polyjuice] used_memory(Bytes)", used_memory);

  /* g_sudt_id must already exists */
  ret = sudt_pay_fee(&context, g_sudt_id, POLYJUICE_SHORT_ADDR_LEN, msg.sender.bytes, fee);
  if (ret != 0) {
    debug_print_int("pay fee to block_producer failed", ret);
    return clean_evmc_result_and_return(&res, ret);
  }
  ret = sys_pay_fee(&context, msg.sender.bytes, POLYJUICE_SHORT_ADDR_LEN, g_sudt_id, fee);
  if (ret != 0) {
    debug_print_int("Record fee payment failed", ret);
    return clean_evmc_result_and_return(&res, ret);
  }

  ckb_debug("finalize");
  ret = gw_finalize(&context);
  if (ret != 0) {
    return clean_evmc_result_and_return(&res, ret);
  }
  return clean_evmc_result_and_return(&res, 0);
}
