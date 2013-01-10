/* Ask about compositing */
#include "e_wizard.h"
#include "e_comp.h"
#include "e_comp_cfdata.h"

static int do_gl = 0;
static int do_vsync = 0;

static int
match_file_glob(FILE *f, const char *globbing)
{
   char buf[32768];
   int found = 0;

   while (fgets(buf, sizeof(buf), f))
     {
        if (e_util_glob_match(buf, globbing))
          {
             found = 1;
             break;
          }
     }
   fclose(f);
   return found;
}

static int
match_xorg_log(const char *globbing)
{
   FILE *f;
   int i;
   char buf[PATH_MAX];

   for (i = 0; i < 5; i++)
     {
        snprintf(buf, sizeof(buf), "/var/log/Xorg.%i.log", i);
        f = fopen(buf, "rb");
        if (f)
          {
             if (match_file_glob(f, globbing)) return 1;
          }
     }
   return 0;
}

EAPI int
wizard_page_show(E_Wizard_Page *pg)
{
   Evas_Object *o, *of, *ob;
   Ecore_Evas *ee;
   Ecore_X_Window_Attributes att;

   if (!ecore_x_composite_query()) return 0;
   if (!ecore_x_damage_query()) return 0;

   memset((&att), 0, sizeof(Ecore_X_Window_Attributes));
   ecore_x_window_attributes_get(ecore_x_window_root_first_get(), &att);
   if ((att.depth <= 8)) return 0;

   if (!ecore_evas_engine_type_supported_get(ECORE_EVAS_ENGINE_OPENGL_X11))
     return 0;

   ee = ecore_evas_gl_x11_new(NULL, 0, 0, 0, 320, 240);
   if (ee)
     {
        ecore_evas_free(ee);
        if (
          (match_xorg_log("*(II)*NVIDIA*: Creating default Display*")) ||
          (match_xorg_log("*(II)*intel*: Creating default Display*")) ||
          (match_xorg_log("*(II)*NOUVEAU*: Creating default Display*")) ||
          (match_xorg_log("*(II)*RADEON*: Creating default Display*"))
          )
          {
             do_gl = 1;
             do_vsync = 1;
          }
     }

   o = e_widget_list_add(pg->evas, 1, 0);
   e_wizard_title_set(_("Engine"));

   of = e_widget_framelist_add(pg->evas, _("HW acceleration"), 0);

   ob = e_widget_check_add(pg->evas, _("Hardware Accelerated (OpenGL)"), &(do_gl));
   e_widget_framelist_object_append(of, ob);

   ob = e_widget_check_add(pg->evas, _("Tear-free Rendering (OpenGL only)"), &(do_vsync));
   e_widget_framelist_object_append(of, ob);

   e_widget_list_object_append(o, of, 0, 0, 0.5);
   evas_object_show(of);
   e_wizard_page_show(o);

   return 1; /* 1 == show ui, and wait for user, 0 == just continue */
}

EAPI int
wizard_page_hide(E_Wizard_Page *pg __UNUSED__)
{
   E_Config_DD *conf_edd = NULL;
   E_Config_DD *conf_match_edd = NULL;
   E_Comp_Config *cfg = NULL;

   e_comp_cfdata_edd_init(&(conf_edd), &(conf_match_edd));
   cfg = e_comp_cfdata_config_new();

   if (do_gl)
     {
        cfg->engine = E_COMP_ENGINE_GL;
        cfg->smooth_windows = 1;
        cfg->vsync = do_vsync;
     }
   else
     {
        cfg->engine = E_COMP_ENGINE_SW;
        cfg->smooth_windows = 0;
        cfg->vsync = 0;
     }

   e_config_domain_save("e_comp", conf_edd, cfg);
   E_CONFIG_DD_FREE(conf_match_edd);
   E_CONFIG_DD_FREE(conf_edd);
   e_comp_cfdata_config_free(cfg);

   e_config_save_queue();

   return 1;
}
