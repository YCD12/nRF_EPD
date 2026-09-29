#include "ui_resources.h"
#include "resource_layout.h"
#include "resource_store.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define N 0xFFU

static uint8_t payload[512];
static uint32_t payload_length;
static bool mount_ok = true;

typedef struct {
    uint8_t mode, id, kind, flags, font_primary, font_secondary, font_tertiary, reserved;
    int16_t x, y, w, h, value0, value1;
} wire_component_t;
static const wire_component_t canonical[UI_COMPONENT_COUNT - 1U] = {
    {1,1,1,0,2,1,0,0,10,38,780,38,16,12}, {1,2,1,0,1,N,N,0,10,44,780,32,0,0},
    {1,3,3,0,2,1,0,0,10,84,780,386,0,0}, {2,4,2,0,N,N,N,0,15,45,770,2,0,0},
    {2,5,2,0,N,N,N,0,15,435,770,2,0,0}, {2,6,2,0,N,N,N,0,390,70,1,350,0,0},
    {2,7,1,0,0,N,N,0,400,86,388,16,77,788}, {2,8,1,0,1,N,N,0,397,96,398,32,0,0},
    {2,9,3,0,2,1,0,0,397,136,398,284,0,0}, {2,10,1,0,2,1,N,0,15,92,362,36,115,377},
    {2,11,2,0,N,N,N,0,15,128,362,1,0,0}, {2,12,4,0,N,N,N,0,0,169,384,150,6,2},
    {2,13,2,0,N,N,N,0,15,360,362,1,0,0}, {2,14,1,0,1,N,N,0,15,380,362,44,404,420},
    {3,15,1,0,2,1,0,0,100,36,600,32,58,138}, {3,16,2,0,N,N,N,0,90,68,620,1,0,0},
    {3,17,4,0,N,N,N,0,160,136,480,208,10,2}, {3,18,2,0,N,N,N,0,90,412,620,1,0,0},
    {3,19,1,0,1,N,N,0,100,442,600,38,462,700},
};

static void put16(uint8_t *p, uint16_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static void put32(uint8_t *p, uint32_t v) { for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(8*i)); }

rs_result_t resource_store_mount(void) { return mount_ok ? RS_OK : RS_NOT_FOUND; }
rs_result_t resource_store_info(uint16_t id, rs_package_info_t *info)
{
    if(id!=RS_PART_UI_RESOURCES||!info)return RS_NOT_FOUND;
    info->revision=7; info->length=payload_length; info->crc32=0; return RS_OK;
}
rs_result_t resource_store_open(uint16_t id, uint32_t *length)
{ if(id!=RS_PART_UI_RESOURCES||!length)return RS_NOT_FOUND;*length=payload_length;return RS_OK; }
rs_result_t resource_store_read(uint16_t id,uint32_t offset,void *data,size_t length)
{
    if(id!=RS_PART_UI_RESOURCES||!data||offset>payload_length||length>payload_length-offset)
        return RS_INVALID;
    memcpy(data,payload+offset,length);return RS_OK;
}
uint32_t resource_get_u32(const uint8_t *p)
{ return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }

static void make_payload(void)
{
    memset(payload,0,sizeof(payload));
    put32(payload,0x31524955); put16(payload+4,1); put16(payload+6,19);
    put16(payload+8,800); put16(payload+10,480); put16(payload+12,20);
    for(unsigned i=0;i<19;i++) {
        const wire_component_t *c=&canonical[i]; uint8_t *r=payload+16+i*20;
        r[0]=c->mode;r[1]=c->id;r[2]=c->kind;r[3]=c->flags;
        r[4]=c->font_primary;r[5]=c->font_secondary;r[6]=c->font_tertiary;r[7]=c->reserved;
        put16(r+8,(uint16_t)c->x);put16(r+10,(uint16_t)c->y);
        put16(r+12,(uint16_t)c->w);put16(r+14,(uint16_t)c->h);
        put16(r+16,(uint16_t)c->value0);put16(r+18,(uint16_t)c->value1);
    }
    payload_length=16+19*20;
}

int main(void)
{
    make_payload(); ui_resources_host_reset();
    assert(ui_resources_init()); assert(ui_resources_revision()==7);
    const ui_component_t *grid=ui_resource_component(UI_COMP_CAL_GRID);
    assert(grid&&grid->x==10&&grid->y==84&&grid->w==780&&grid->h==386);
    clock_refresh_geom_t geom;
    assert(ui_resources_clock_geom(false,800,480,&geom));
    assert(geom.origin_x==0&&geom.origin_y==169&&geom.canvas_w==384&&geom.cS==6);
    assert(ui_resources_clock_geom(true,800,480,&geom));
    assert(geom.origin_x==160&&geom.origin_y==136&&geom.canvas_h==208&&geom.cS==10);
    assert(!ui_resources_clock_geom(true,400,300,&geom));

    ui_resources_host_reset(); payload[16+4]=3; assert(!ui_resources_init());
    make_payload(); ui_resources_host_reset(); put16(payload+16+11*20+8,1); assert(!ui_resources_init());
    make_payload(); ui_resources_host_reset(); mount_ok=false; assert(!ui_resources_init());
    puts("UI resources: schema, fonts, bounds and clock geometry OK");
    return 0;
}
