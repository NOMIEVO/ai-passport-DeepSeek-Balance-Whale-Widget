// main/whale_store.h —— 配置与账本的 NVS 持久化。
#pragma once

#include <stdbool.h>

#include "whale_model.h"

#ifdef __cplusplus
extern "C" {
#endif

// 读不到(或长度对不上)时返回 false,调用方按默认值继续运行。
bool whale_store_load_config(whale_config_t *cfg);
bool whale_store_save_config(const whale_config_t *cfg);

bool whale_store_load_ledger(whale_ledger_t *ledger);
bool whale_store_save_ledger(const whale_ledger_t *ledger);

// 清掉已保存的 Wi-Fi 与 API Key,保留设置与账本。
bool whale_store_clear_credentials(void);

#ifdef __cplusplus
}
#endif
