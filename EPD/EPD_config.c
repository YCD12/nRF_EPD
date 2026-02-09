#include "EPD_config.h"

#include <string.h>

#include "app_scheduler.h"
#include "fds.h"
#include "nordic_common.h"
#include "nrf_log.h"

// Flash Data Storage (FDS) file ID for configuration
// Flash数据存储(FDS)配置文件ID
#define CONFIG_FILE_ID 0x0000

// FDS record key for configuration
// FDS配置记录键值
#define CONFIG_REC_KEY 0x0001

// FDS event handler for flash operations
// FDS事件处理器，用于处理Flash操作事件
static void fds_evt_handler(fds_evt_t const* const p_fds_evt) {
    // Log FDS event ID and result
    // 记录FDS事件ID和结果
    NRF_LOG_DEBUG("fds evt: id=%d result=%d\n", p_fds_evt->id, p_fds_evt->result);
}

// Run FDS garbage collection to reclaim flash space
// 运行FDS垃圾回收以回收Flash空间
static void run_fds_gc(void* p_event_data, uint16_t event_size) {
    // Execute garbage collection to free up deleted record space
    // 执行垃圾回收以释放已删除记录的空间
    NRF_LOG_DEBUG("run garbage collection (fds_gc)\n");
    fds_gc();
}

// Initialize EPD configuration and Flash Data Storage (FDS)
// 初始化EPD配置和Flash数据存储(FDS)
void epd_config_init(epd_config_t* cfg) {
    ret_code_t ret;

    // Register FDS event handler for flash operations
    // 注册FDS事件处理器用于Flash操作
    ret = fds_register(fds_evt_handler);
    if (ret != NRF_SUCCESS) {
        NRF_LOG_ERROR("fds_register failed, code=%d\n", ret);
        return;
    }

    // Initialize Flash Data Storage module
    // 初始化Flash数据存储模块
    ret = fds_init();
    if (ret != NRF_SUCCESS) {
        NRF_LOG_ERROR("fds_init failed, code=%d\n", ret);
        return;
    }

    // Run garbage collection to clean up flash
    // 运行垃圾回收以清理Flash空间
    run_fds_gc(NULL, 0);
}

// Read EPD configuration from flash memory
// 从Flash存储器读取EPD配置
void epd_config_read(epd_config_t* cfg) {
    fds_flash_record_t flash_record;   // Flash record structure - Flash记录结构
    fds_record_desc_t record_desc;     // Record descriptor - 记录描述符
    fds_find_token_t ftok;             // Find token for searching - 搜索令牌

    // Initialize configuration with 0xFF (empty flash state)
    // 将配置初始化为0xFF（空Flash状态）
    memset(cfg, 0xFF, sizeof(epd_config_t));
    memset(&ftok, 0x00, sizeof(fds_find_token_t));

    // Search for configuration record in flash
    // 在Flash中搜索配置记录
    if (fds_record_find(CONFIG_FILE_ID, CONFIG_REC_KEY, &record_desc, &ftok) != NRF_SUCCESS) {
        NRF_LOG_DEBUG("epd_config_load: record not found\n");
        return;
    }

    // Open the found record for reading
    // 打开找到的记录以便读取
    if (fds_record_open(&record_desc, &flash_record) != NRF_SUCCESS) {
        NRF_LOG_ERROR("epd_config_load: record open failed!");
        return;
    }

    // Get record length based on SoftDevice version
    // 根据SoftDevice版本获取记录长度
#ifdef S112
    uint32_t record_len = flash_record.p_header->length_words * sizeof(uint32_t);
#else
    uint32_t record_len = flash_record.p_header->tl.length_words * sizeof(uint32_t);
#endif

    // Copy data from flash to configuration structure
    // 从Flash复制数据到配置结构体
    memcpy(cfg, flash_record.p_data, MIN(sizeof(epd_config_t), record_len));

    // Close the record after reading
    // 读取完成后关闭记录
    fds_record_close(&record_desc);
}

// Write EPD configuration to flash memory
// 将EPD配置写入Flash存储器
void epd_config_write(epd_config_t* cfg) {
    ret_code_t ret;
    fds_record_t record;               // Record to write - 要写入的记录
    fds_record_desc_t record_desc;     // Record descriptor - 记录描述符
    fds_find_token_t ftok;             // Find token - 搜索令牌

    // Set up record file ID and key
    // 设置记录文件ID和键值
    record.file_id = CONFIG_FILE_ID;
    record.key = CONFIG_REC_KEY;

    // Configure record data based on SoftDevice version
    // 根据SoftDevice版本配置记录数据
#ifdef S112
    // S112 SoftDevice uses direct data pointer
    // S112 SoftDevice使用直接数据指针
    record.data.p_data = (void*)cfg;
    record.data.length_words = BYTES_TO_WORDS(sizeof(epd_config_t));
#else
    // Other SoftDevices use chunk-based approach
    // 其他SoftDevice使用分块方式
    fds_record_chunk_t record_chunk;
    record_chunk.p_data = cfg;
    record_chunk.length_words = BYTES_TO_WORDS(sizeof(epd_config_t));
    record.data.p_chunks = &record_chunk;
    record.data.num_chunks = 1;
#endif

    // Check if record already exists in flash
    // 检查记录是否已存在于Flash中
    memset(&ftok, 0x00, sizeof(fds_find_token_t));
    ret = fds_record_find(CONFIG_FILE_ID, CONFIG_REC_KEY, &record_desc, &ftok);

    // Update existing record or write new record
    // 更新已存在的记录或写入新记录
    if (ret == NRF_SUCCESS)
        ret = fds_record_update(&record_desc, &record);  // Update existing record - 更新已存在的记录
    else
        ret = fds_record_write(&record_desc, &record);   // Write new record - 写入新记录

    // Handle write/update errors
    // 处理写入/更新错误
    if (ret != NRF_SUCCESS) {
        NRF_LOG_ERROR("epd_config_save: record write/update failed, code=%d\n", ret);
        // Schedule garbage collection if flash is full
        // 如果Flash已满，则安排垃圾回收
        if (ret == FDS_ERR_NO_SPACE_IN_FLASH) app_sched_event_put(NULL, 0, run_fds_gc);
    }
}

// Clear EPD configuration from flash memory
// 从Flash存储器清除EPD配置
void epd_config_clear(epd_config_t* cfg) {
    ret_code_t ret;
    fds_record_desc_t record_desc;     // Record descriptor - 记录描述符
    fds_find_token_t ftok;             // Find token - 搜索令牌

    // Search for configuration record in flash
    // 在Flash中搜索配置记录
    memset(&ftok, 0x00, sizeof(fds_find_token_t));
    if (fds_record_find(CONFIG_FILE_ID, CONFIG_REC_KEY, &record_desc, &ftok) != NRF_SUCCESS) {
        NRF_LOG_DEBUG("epd_config_clear: record not found\n");
        return;
    }

    // Delete the configuration record from flash
    // 从Flash删除配置记录
    ret = fds_record_delete(&record_desc);
    if (ret != NRF_SUCCESS) {
        NRF_LOG_ERROR("fds_record_delete failed, code=%d\n", ret);
    }
}

// Check if EPD configuration is empty (all bytes are 0xFF)
// 检查EPD配置是否为空（所有字节都是0xFF）
bool epd_config_empty(epd_config_t* cfg) {
    // Iterate through all bytes in the configuration
    // 遍历配置中的所有字节
    for (uint8_t i = 0; i < EPD_CONFIG_SIZE; i++) {
        // If any byte is not 0xFF, configuration is not empty
        // 如果有任何字节不是0xFF，则配置不为空
        if (((uint8_t*)cfg)[i] != 0xFF) return false;
    }
    // All bytes are 0xFF, configuration is empty
    // 所有字节都是0xFF，配置为空
    return true;
}
