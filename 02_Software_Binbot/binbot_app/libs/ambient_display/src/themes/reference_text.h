/* Inline source fragments keep their paint order and colors while reflowing
 * around measured live text. No browser, extra page or persistent bitmap. */
static int text_width(layer_t *l)
{
    if(!l->value[0])return 0;
    lv_point_t size;lv_text_get_size(&size,l->value,l->font,
        lv_obj_get_style_text_letter_space(l->object,0),0,LV_COORD_MAX,LV_TEXT_FLAG_NONE);
    return size.x;
}
static void flows_update(reference_t *s)
{
    for(unsigned i=0;i<s->count;++i){
        layer_t *first=&s->layers[i];if(first->kind!=TEXT||first->flow<0)continue;
        bool seen=false;for(unsigned j=0;j<i;++j)if(s->layers[j].kind==TEXT&&s->layers[j].flow==first->flow){seen=true;break;}
        if(seen)continue;
        float total=0;unsigned count=0;
        for(unsigned j=i;j<s->count;++j){layer_t *l=&s->layers[j];
            if(l->kind!=TEXT||l->flow!=first->flow||lv_obj_has_flag(l->object,LV_OBJ_FLAG_HIDDEN))continue;
            total+=(count++?l->flow_gap:0)+text_width(l);
        }
        float x=first->flow_center>=0?fmaxf(first->flow_start,first->flow_center-total/2):first->flow_start;
        count=0;
        for(unsigned j=i;j<s->count;++j){layer_t *l=&s->layers[j];
            if(l->kind!=TEXT||l->flow!=first->flow||lv_obj_has_flag(l->object,LV_OBJ_FLAG_HIDDEN))continue;
            if(count++)x+=l->flow_gap;
            int width=text_width(l);l->x=x;
            lv_obj_set_width(l->object,(int)fmaxf(1,fminf(width+2,first->flow_right-x)));
            if(l->parent<0)lv_obj_set_x(l->object,lroundf(x));
            x+=width;
        }
    }
}
static void text_shadow(lv_event_t *event)
{
    layer_t *l=lv_event_get_user_data(event);
    if(lv_event_get_code(event)==LV_EVENT_REFR_EXT_DRAW_SIZE){
        for(unsigned i=0;i<l->shadow_count;++i){shadow_t *s=&l->shadows[i];
            lv_event_set_ext_draw_size(event,LV_MAX(abs(s->x),abs(s->y))+s->blur+2);
        }
        return;
    }
    if(lv_event_get_code(event)!=LV_EVENT_DRAW_MAIN_BEGIN)return;
    lv_layer_t *canvas=lv_event_get_layer(event);lv_area_t a;lv_obj_get_coords(l->object,&a);
    for(unsigned i=l->shadow_count;i>0;--i){shadow_t *s=&l->shadows[i-1];
        lv_draw_label_dsc_t d;lv_draw_label_dsc_init(&d);lv_obj_init_draw_label_dsc(l->object,LV_PART_MAIN,&d);
        d.text=lv_label_get_text(l->object);d.color=lv_color_hex(s->color);d.opa=LV_OPA_MIX2(d.opa,s->opacity);
        if(s->blur){
            d.base.drop_shadow_color=d.color;d.base.drop_shadow_opa=255;
            d.base.drop_shadow_blur_radius=(s->blur+1)/2;d.base.drop_shadow_quality=LV_BLUR_QUALITY_AUTO;
        }
        lv_area_t target=a;lv_area_move(&target,s->x,s->y);lv_draw_label(canvas,&d,&target);
    }
}
