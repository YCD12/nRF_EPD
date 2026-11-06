/* Exercise the production UIR2 consumer with source-generated external data. */
#include "ui_default_payload.h"
#include "resource_store.h"
#include "resource_layout.h"
#include <string.h>
#define resource_store_mount ui_stub_mount
#define resource_store_open_info ui_stub_open_info
#define resource_store_read ui_stub_read
#define resource_get_u32 ui_stub_u32
static uint8_t ui_variant[8192];
static const uint8_t *ui_bytes=ui_default_payload;
static size_t ui_length=sizeof(ui_default_payload);
static bool available=true;
static uint32_t revision=1;
static unsigned read_count, fault_at, fault_count;
static bool render_valid = true;
void resource_store_render_begin(void) { render_valid = true; }
bool resource_store_render_valid(void) { return render_valid; }
rs_result_t ui_stub_mount(void){return available?RS_OK:RS_NOT_FOUND;}
rs_result_t ui_stub_open_info(uint16_t id,rs_package_info_t *info)
{if(id!=6)return RS_NOT_FOUND;info->length=ui_length;info->revision=revision;return RS_OK;}
rs_result_t ui_stub_read(uint16_t id,uint32_t at,void *out,size_t n)
{
    if (++read_count == fault_at) { fault_count++; render_valid=false; return RS_IO; }
    if(id!=6||at>ui_length||n>ui_length-at)return RS_INVALID;
    memcpy(out,ui_bytes+at,n);return RS_OK;
}
uint32_t ui_stub_u32(const uint8_t *p){return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
#include "../../GUI/ui_resources.c"
#undef resource_store_mount
#undef resource_store_open_info
#undef resource_store_read
#undef resource_get_u32
void ui_resources_stub_set_revision(uint32_t value){revision=value;m_revision=value;}
void ui_resources_stub_fail_read(unsigned nth){read_count=0;fault_at=nth;fault_count=0;}
unsigned ui_resources_stub_read_count(void){return read_count;}
unsigned ui_resources_stub_fault_count(void){return fault_count;}
void ui_resources_stub_set_available(bool value){available=value;ui_resources_invalidate();}
void ui_resources_stub_set_component(ui_component_id_t id,const ui_component_t *value)
{for(unsigned i=0;i<m_count;i++)if(m_components[i].id==id)m_components[i]=*value;}

void ui_resources_stub_load(const uint8_t *bytes,size_t n)
{if(n>sizeof(ui_variant))return;memcpy(ui_variant,bytes,n);ui_bytes=ui_variant;ui_length=n;ui_resources_invalidate();}
