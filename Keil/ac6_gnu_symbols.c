/* GNU-style section boundary symbols required by the nRF5 SDK when armclang
 * defines __GNUC__. The linker places the observer sections in auto-sized
 * regions and provides Image$$<region>$$Base/Limit symbols; alias them so the
 * SDK's section iterator sees exact boundaries (no hard-coded addresses and no
 * padding counted as items). Used only by the Keil ARM Compiler 6 project. */
__asm__(
    ".global __isr_vector\n"
    ".set __isr_vector, Image$$ER_IROM1$$Base\n"
    ".global __data_start__\n"
    ".set __data_start__, Image$$RW_IRAM1$$Base\n"
    ".global __start_sdh_ble_observers\n"
    ".set __start_sdh_ble_observers, Image$$ER_SDH_BLE$$Base\n"
    ".global __stop_sdh_ble_observers\n"
    ".set __stop_sdh_ble_observers, Image$$ER_SDH_BLE$$Limit\n"
    ".global __start_sdh_soc_observers\n"
    ".set __start_sdh_soc_observers, Image$$ER_SDH_SOC$$Base\n"
    ".global __stop_sdh_soc_observers\n"
    ".set __stop_sdh_soc_observers, Image$$ER_SDH_SOC$$Limit\n"
    ".global __start_sdh_state_observers\n"
    ".set __start_sdh_state_observers, Image$$ER_SDH_STATE$$Base\n"
    ".global __stop_sdh_state_observers\n"
    ".set __stop_sdh_state_observers, Image$$ER_SDH_STATE$$Limit\n"
    ".global __start_sdh_stack_observers\n"
    ".set __start_sdh_stack_observers, Image$$ER_SDH_STACK$$Base\n"
    ".global __stop_sdh_stack_observers\n"
    ".set __stop_sdh_stack_observers, Image$$ER_SDH_STACK$$Limit\n"
    ".global __start_sdh_req_observers\n"
    ".set __start_sdh_req_observers, Image$$ER_SDH_REQ$$Base\n"
    ".global __stop_sdh_req_observers\n"
    ".set __stop_sdh_req_observers, Image$$ER_SDH_REQ$$Limit\n"
    ".global __start_pwr_mgmt_data\n"
    ".set __start_pwr_mgmt_data, Image$$ER_PWR_MGMT$$Base\n"
    ".global __stop_pwr_mgmt_data\n"
    ".set __stop_pwr_mgmt_data, Image$$ER_PWR_MGMT$$Limit\n"
);
