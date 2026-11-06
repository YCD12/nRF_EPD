void test_critical_enter(void);
void test_critical_exit(void);
#define CRITICAL_REGION_ENTER() { test_critical_enter();
#define CRITICAL_REGION_EXIT() test_critical_exit(); }
