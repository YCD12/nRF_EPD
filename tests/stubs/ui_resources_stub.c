#include "ui_resources.h"

#define N 0xFFU

static const ui_component_t components[UI_COMPONENT_COUNT - 1U] = {
    {1,1,UI_KIND_GROUP,0,2,1,0,0,10,38,780,38,16,12},
    {1,2,UI_KIND_GROUP,0,1,N,N,0,10,44,780,32,0,0},
    {1,3,UI_KIND_GRID,0,2,1,0,0,10,84,780,386,0,0},
    {2,4,UI_KIND_LINE,0,N,N,N,0,15,45,770,2,0,0},
    {2,5,UI_KIND_LINE,0,N,N,N,0,15,435,770,2,0,0},
    {2,6,UI_KIND_LINE,0,N,N,N,0,390,70,1,350,0,0},
    {2,7,UI_KIND_GROUP,0,0,N,N,0,400,86,388,16,77,788},
    {2,8,UI_KIND_GROUP,0,1,N,N,0,397,96,398,32,0,0},
    {2,9,UI_KIND_GRID,0,2,1,0,0,397,136,398,284,0,0},
    {2,10,UI_KIND_GROUP,0,2,1,N,0,15,92,362,36,115,377},
    {2,11,UI_KIND_LINE,0,N,N,N,0,15,128,362,1,0,0},
    {2,12,UI_KIND_CLOCK,0,N,N,N,0,0,169,384,150,6,2},
    {2,13,UI_KIND_LINE,0,N,N,N,0,15,360,362,1,0,0},
    {2,14,UI_KIND_GROUP,0,1,N,N,0,15,380,362,44,404,420},
    {3,15,UI_KIND_GROUP,0,2,1,0,0,100,36,600,32,58,138},
    {3,16,UI_KIND_LINE,0,N,N,N,0,90,68,620,1,0,0},
    {3,17,UI_KIND_CLOCK,0,N,N,N,0,160,136,480,208,10,2},
    {3,18,UI_KIND_LINE,0,N,N,N,0,90,412,620,1,0,0},
    {3,19,UI_KIND_GROUP,0,1,N,N,0,100,442,600,38,462,700},
};

static uint32_t revision = 1U;

bool ui_resources_init(void) { return true; }
bool ui_resources_ready(void) { return true; }
uint32_t ui_resources_revision(void) { return revision; }

const ui_component_t *ui_resource_component(ui_component_id_t id)
{
    return id > 0 && id < UI_COMPONENT_COUNT ? &components[id - 1U] : 0;
}

bool ui_resources_clock_geom(bool fullscreen, uint16_t screen_w, uint16_t screen_h,
                             clock_refresh_geom_t *g)
{
    const ui_component_t *c;
    if (!g || screen_w != 800U || screen_h != 480U) return false;
    c = ui_resource_component(fullscreen ? UI_COMP_CLOCK_TIME : UI_COMP_SPLIT_TIME);
    g->origin_x=(uint16_t)c->x; g->origin_y=(uint16_t)c->y;
    g->canvas_w=(uint16_t)c->w; g->canvas_h=(uint16_t)c->h;
    g->cS=(uint16_t)c->value0; g->nD=(uint16_t)c->value1;
    g->span_gap=fullscreen?6U:4U;
    return true;
}

void ui_resources_stub_set_revision(uint32_t value) { revision = value; }
