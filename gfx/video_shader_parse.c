/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2010-2014 - Hans-Kristian Arntzen
 *  Copyright (C) 2011-2017 - Daniel De Matteis
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <string.h>

#ifdef HAVE_CONFIG_H
#include "../config.h"
#endif

#include <libretro.h>
#include <compat/posix_string.h>
#include <compat/msvc.h>
#include <compat/strl.h>
#include <file/file_path.h>
#include <rhash.h>
#include <string/stdstring.h>
#include <streams/interface_stream.h>
#include <lists/string_list.h>

#include "../configuration.h"
#include "../retroarch.h"
#include "../verbosity.h"
#include "../frontend/frontend_driver.h"
#include "../command.h"
#include "../file_path_special.h"
#include "video_shader_parse.h"

#if defined(HAVE_SLANG) && defined(HAVE_SPIRV_CROSS)
#include "drivers_shader/slang_preprocess.h"
#endif

static path_change_data_t *file_change_data = NULL;

/**
 * wrap_mode_to_str:
 * @type              : Wrap type.
 *
 * Translates wrap mode to human-readable string identifier.
 *
 * Returns: human-readable string identifier of wrap mode.
 **/
static const char *wrap_mode_to_str(enum gfx_wrap_type type)
{
   switch (type)
   {
      case RARCH_WRAP_BORDER:
         return "clamp_to_border";
      case RARCH_WRAP_EDGE:
         return "clamp_to_edge";
      case RARCH_WRAP_REPEAT:
         return "repeat";
      case RARCH_WRAP_MIRRORED_REPEAT:
         return "mirrored_repeat";
      default:
         break;
   }

   return "???";
}

/**
 * wrap_str_to_mode:
 * @type              : Wrap type in human-readable string format.
 *
 * Translates wrap mode from human-readable string to enum mode value.
 *
 * Returns: enum mode value of wrap type.
 **/
static enum gfx_wrap_type wrap_str_to_mode(const char *wrap_mode)
{
   if (string_is_equal(wrap_mode,      "clamp_to_border"))
      return RARCH_WRAP_BORDER;
   else if (string_is_equal(wrap_mode, "clamp_to_edge"))
      return RARCH_WRAP_EDGE;
   else if (string_is_equal(wrap_mode, "repeat"))
      return RARCH_WRAP_REPEAT;
   else if (string_is_equal(wrap_mode, "mirrored_repeat"))
      return RARCH_WRAP_MIRRORED_REPEAT;

   RARCH_WARN("Invalid wrapping type %s. Valid ones are: clamp_to_border"
         " (default), clamp_to_edge, repeat and mirrored_repeat. Falling back to default.\n",
         wrap_mode);
   return RARCH_WRAP_DEFAULT;
}

/**
 * video_shader_parse_pass:
 * @conf              : Preset file to read from.
 * @pass              : Shader passes handle.
 * @i                 : Index of shader pass.
 *
 * Parses shader pass from preset file.
 *
 * Returns: true (1) if successful, otherwise false (0).
 **/
static bool video_shader_parse_pass(config_file_t *conf,
      struct video_shader_pass *pass, unsigned i)
{
   char shader_name[64];
   char filter_name_buf[64];
   char wrap_name_buf[64];
   char wrap_mode[64];
   char frame_count_mod_buf[64];
   char srgb_output_buf[64];
   char fp_fbo_buf[64];
   char mipmap_buf[64];
   char alias_buf[64];
   char scale_name_buf[64];
   char attr_name_buf[64];
   char scale_type[64];
   char scale_type_x[64];
   char scale_type_y[64];
   char frame_count_mod[64];
   size_t path_size             = PATH_MAX_LENGTH * sizeof(char);
   char *tmp_str                = (char*)malloc(path_size);
   char *tmp_path               = NULL;
   struct gfx_fbo_scale *scale  = NULL;
   bool tmp_bool                = false;
   float fattr                  = 0.0f;
   int iattr                    = 0;

   fp_fbo_buf[0]     = mipmap_buf[0]    = alias_buf[0]           =
   scale_name_buf[0] = attr_name_buf[0] = scale_type[0]          =
   scale_type_x[0]   = scale_type_y[0]  = frame_count_mod[0]     =
   tmp_str[0]        = shader_name[0]   = filter_name_buf[0]     =
   wrap_name_buf[0]  = wrap_mode[0]     = frame_count_mod_buf[0] = '\0';
   srgb_output_buf[0] = '\0';

   /* Source */
   snprintf(shader_name, sizeof(shader_name), "shader%u", i);
   if (!config_get_path(conf, shader_name, tmp_str, path_size))
   {
      RARCH_ERR("Couldn't parse shader source (%s).\n", shader_name);
      if (tmp_str)
         free(tmp_str);
      return false;
   }

   tmp_path = (char*)malloc(PATH_MAX_LENGTH * sizeof(char));
   strlcpy(tmp_path, tmp_str, path_size);
   path_resolve_realpath(tmp_path, path_size);

   if (!path_is_valid(tmp_path))
      strlcpy(pass->source.path, tmp_str, sizeof(pass->source.path));
   else
      strlcpy(pass->source.path, tmp_path, sizeof(pass->source.path));

   free(tmp_str);
   free(tmp_path);

   /* Smooth */
   snprintf(filter_name_buf, sizeof(filter_name_buf), "filter_linear%u", i);

   if (config_get_bool(conf, filter_name_buf, &tmp_bool))
   {
      bool smooth = tmp_bool;
      pass->filter = smooth ? RARCH_FILTER_LINEAR : RARCH_FILTER_NEAREST;
   }
   else
      pass->filter = RARCH_FILTER_UNSPEC;

   /* Wrapping mode */
   snprintf(wrap_name_buf, sizeof(wrap_name_buf), "wrap_mode%u", i);
   if (config_get_array(conf, wrap_name_buf, wrap_mode, sizeof(wrap_mode)))
      pass->wrap = wrap_str_to_mode(wrap_mode);

   /* Frame count mod */
   snprintf(frame_count_mod_buf, sizeof(frame_count_mod_buf), "frame_count_mod%u", i);
   if (config_get_array(conf, frame_count_mod_buf,
            frame_count_mod, sizeof(frame_count_mod)))
      pass->frame_count_mod = (unsigned)strtoul(frame_count_mod, NULL, 0);

   /* FBO types and mipmapping */
   snprintf(srgb_output_buf, sizeof(srgb_output_buf), "srgb_framebuffer%u", i);
   if (config_get_bool(conf, srgb_output_buf, &tmp_bool))
      pass->fbo.srgb_fbo = tmp_bool;

   snprintf(fp_fbo_buf, sizeof(fp_fbo_buf), "float_framebuffer%u", i);
   if (config_get_bool(conf, fp_fbo_buf, &tmp_bool))
      pass->fbo.fp_fbo = tmp_bool;

   snprintf(mipmap_buf, sizeof(mipmap_buf), "mipmap_input%u", i);
   if (config_get_bool(conf, mipmap_buf, &tmp_bool))
      pass->mipmap = tmp_bool;

   snprintf(alias_buf, sizeof(alias_buf), "alias%u", i);
   if (!config_get_array(conf, alias_buf, pass->alias, sizeof(pass->alias)))
      *pass->alias = '\0';

   /* Scale */
   scale = &pass->fbo;
   snprintf(scale_name_buf, sizeof(scale_name_buf), "scale_type%u", i);
   config_get_array(conf, scale_name_buf, scale_type, sizeof(scale_type));

   snprintf(scale_name_buf, sizeof(scale_name_buf), "scale_type_x%u", i);
   config_get_array(conf, scale_name_buf, scale_type_x, sizeof(scale_type_x));

   snprintf(scale_name_buf, sizeof(scale_name_buf), "scale_type_y%u", i);
   config_get_array(conf, scale_name_buf, scale_type_y, sizeof(scale_type_y));

   if (!*scale_type && !*scale_type_x && !*scale_type_y)
      return true;

   if (*scale_type)
   {
      strlcpy(scale_type_x, scale_type, sizeof(scale_type_x));
      strlcpy(scale_type_y, scale_type, sizeof(scale_type_y));
   }

   scale->valid   = true;
   scale->type_x  = RARCH_SCALE_INPUT;
   scale->type_y  = RARCH_SCALE_INPUT;
   scale->scale_x = 1.0;
   scale->scale_y = 1.0;

   if (*scale_type_x)
   {
      if (string_is_equal(scale_type_x, "source"))
         scale->type_x = RARCH_SCALE_INPUT;
      else if (string_is_equal(scale_type_x, "viewport"))
         scale->type_x = RARCH_SCALE_VIEWPORT;
      else if (string_is_equal(scale_type_x, "absolute"))
         scale->type_x = RARCH_SCALE_ABSOLUTE;
      else
      {
         RARCH_ERR("Invalid attribute.\n");
         return false;
      }
   }

   if (*scale_type_y)
   {
      if (string_is_equal(scale_type_y, "source"))
         scale->type_y = RARCH_SCALE_INPUT;
      else if (string_is_equal(scale_type_y, "viewport"))
         scale->type_y = RARCH_SCALE_VIEWPORT;
      else if (string_is_equal(scale_type_y, "absolute"))
         scale->type_y = RARCH_SCALE_ABSOLUTE;
      else
      {
         RARCH_ERR("Invalid attribute.\n");
         return false;
      }
   }

   snprintf(attr_name_buf, sizeof(attr_name_buf), "scale%u", i);

   if (scale->type_x == RARCH_SCALE_ABSOLUTE)
   {
      if (config_get_int(conf, attr_name_buf, &iattr))
         scale->abs_x = iattr;
      else
      {
         snprintf(attr_name_buf, sizeof(attr_name_buf), "scale_x%u", i);
         if (config_get_int(conf, attr_name_buf, &iattr))
            scale->abs_x = iattr;
      }
   }
   else
   {
      if (config_get_float(conf, attr_name_buf, &fattr))
         scale->scale_x = fattr;
      else
      {
         snprintf(attr_name_buf, sizeof(attr_name_buf), "scale_x%u", i);
         if (config_get_float(conf, attr_name_buf, &fattr))
            scale->scale_x = fattr;
      }
   }

   snprintf(attr_name_buf, sizeof(attr_name_buf), "scale%u", i);

   if (scale->type_y == RARCH_SCALE_ABSOLUTE)
   {
      if (config_get_int(conf, attr_name_buf, &iattr))
         scale->abs_y = iattr;
      else
      {
         snprintf(attr_name_buf, sizeof(attr_name_buf), "scale_y%u", i);
         if (config_get_int(conf, attr_name_buf, &iattr))
            scale->abs_y = iattr;
      }
   }
   else
   {
      if (config_get_float(conf, attr_name_buf, &fattr))
         scale->scale_y = fattr;
      else
      {
         snprintf(attr_name_buf, sizeof(attr_name_buf), "scale_y%u", i);
         if (config_get_float(conf, attr_name_buf, &fattr))
            scale->scale_y = fattr;
      }
   }

   return true;
}

/**
 * video_shader_parse_textures:
 * @conf              : Preset file to read from.
 * @shader            : Shader pass handle.
 *
 * Parses shader textures.
 *
 * Returns: true (1) if successful, otherwise false (0).
 **/
static bool video_shader_parse_textures(config_file_t *conf,
      struct video_shader *shader)
{
   size_t path_size     = PATH_MAX_LENGTH * sizeof(char);
   const char *id       = NULL;
   char *save           = NULL;
   char *textures       = (char*)malloc(1024 * sizeof(char));

   textures[0]          = '\0';

   if (!config_get_array(conf, "textures", textures, 1024 * sizeof(char)))
   {
      free(textures);
      return true;
   }

   for (id = strtok_r(textures, ";", &save);
         id && shader->luts < GFX_MAX_TEXTURES;
         shader->luts++, id = strtok_r(NULL, ";", &save))
   {
      char id_filter[64];
      char id_wrap[64];
      char wrap_mode[64];
      char id_mipmap[64];
      bool mipmap         = false;
      bool smooth         = false;
      char *tmp_path      = NULL;

      id_filter[0] = id_wrap[0] = wrap_mode[0] = id_mipmap[0] = '\0';

      if (!config_get_array(conf, id, shader->lut[shader->luts].path,
               sizeof(shader->lut[shader->luts].path)))
      {
         RARCH_ERR("Cannot find path to texture \"%s\" ...\n", id);
         free(textures);
         return false;
      }

      tmp_path            = (char*)malloc(PATH_MAX_LENGTH * sizeof(char));
      tmp_path[0]         = '\0';
      strlcpy(tmp_path, shader->lut[shader->luts].path,
            path_size);
      path_resolve_realpath(tmp_path, path_size);

      if (path_is_valid(tmp_path))
         strlcpy(shader->lut[shader->luts].path,
            tmp_path, sizeof(shader->lut[shader->luts].path));
      free(tmp_path);

      strlcpy(shader->lut[shader->luts].id, id,
            sizeof(shader->lut[shader->luts].id));

      snprintf(id_filter, sizeof(id_filter), "%s_linear", id);
      if (config_get_bool(conf, id_filter, &smooth))
         shader->lut[shader->luts].filter = smooth ?
            RARCH_FILTER_LINEAR : RARCH_FILTER_NEAREST;
      else
         shader->lut[shader->luts].filter = RARCH_FILTER_UNSPEC;

      snprintf(id_wrap, sizeof(id_wrap), "%s_wrap_mode", id);
      if (config_get_array(conf, id_wrap, wrap_mode, sizeof(wrap_mode)))
         shader->lut[shader->luts].wrap = wrap_str_to_mode(wrap_mode);

      snprintf(id_mipmap, sizeof(id_mipmap), "%s_mipmap", id);
      if (config_get_bool(conf, id_mipmap, &mipmap))
         shader->lut[shader->luts].mipmap = mipmap;
      else
         shader->lut[shader->luts].mipmap = false;
   }

   free(textures);
   return true;
}

/**
 * video_shader_parse_find_parameter:
 * @params            : Shader parameter handle.
 * @num_params        : Number of shader params in @params.
 * @id                : Identifier to search for.
 *
 * Finds a shader parameter with identifier @id in @params..
 *
 * Returns: handle to shader parameter if successful, otherwise NULL.
 **/
static struct video_shader_parameter *video_shader_parse_find_parameter(
      struct video_shader_parameter *params,
      unsigned num_params, const char *id)
{
   unsigned i;

   for (i = 0; i < num_params; i++)
   {
      if (string_is_equal(params[i].id, id))
         return &params[i];
   }

   return NULL;
}

/**
 * video_shader_set_current_parameters:
 * @conf              : Preset file to read from.
 * @shader            : Shader passes handle.
 *
 * Reads the current value for all parameters from config file.
 *
 * Returns: true (1) if successful, otherwise false (0).
 **/
bool video_shader_resolve_current_parameters(config_file_t *conf,
      struct video_shader *shader)
{
   size_t param_size     = 4096 * sizeof(char);
   const char *id        = NULL;
   char *parameters      = NULL;
   char *save            = NULL;

   if (!conf)
      return false;

   parameters            = (char*)malloc(param_size);

   if (!parameters)
      return false;

   parameters[0]         = '\0';

   /* Read in parameters which override the defaults. */
   if (!config_get_array(conf, "parameters",
            parameters, param_size))
   {
      free(parameters);
      return true;
   }

   for (id = strtok_r(parameters, ";", &save); id;
         id = strtok_r(NULL, ";", &save))
   {
      struct video_shader_parameter *parameter =
         (struct video_shader_parameter*)
         video_shader_parse_find_parameter(
               shader->parameters, shader->num_parameters, id);

      if (!parameter)
      {
         RARCH_WARN("[CGP/GLSLP]: Parameter %s is set in the preset,"
               " but no shader uses this parameter, ignoring.\n", id);
         continue;
      }

      if (!config_get_float(conf, id, &parameter->current))
         RARCH_WARN("[CGP/GLSLP]: Parameter %s is not set in preset.\n", id);
   }

   free(parameters);
   return true;
}

/**
 * video_shader_resolve_parameters:
 * @conf              : Preset file to read from.
 * @shader            : Shader passes handle.
 *
 * Resolves all shader parameters belonging to shaders.
 *
 * Returns: true (1) if successful, otherwise false (0).
 **/
bool video_shader_resolve_parameters(config_file_t *conf,
      struct video_shader *shader)
{
   unsigned i;
   struct video_shader_parameter *param = &shader->parameters[0];

   shader->num_parameters = 0;

   /* Find all parameters in our shaders. */

   for (i = 0; i < shader->passes; i++)
   {
      intfstream_t *file = NULL;
      size_t line_size   = 4096 * sizeof(char);
      char *line         = NULL;
      const char *path   = shader->pass[i].source.path;

     if (string_is_empty(path))
        continue;

#if defined(HAVE_SLANG) && defined(HAVE_SPIRV_CROSS)
      /* First try to use the more robust slang
       * implementation to support #includes. */
      /* FIXME: The check for slang can be removed
       * if it's sufficiently tested for
       * GLSL/Cg as well, it should be the same implementation. */
      if (string_is_equal(path_get_extension(path), "slang") &&
            slang_preprocess_parse_parameters(path, shader))
         continue;

      /* If that doesn't work, fallback to the old path.
       * Ideally, we'd get rid of this path sooner or later. */
#endif
      file = intfstream_open_file(path,
            RETRO_VFS_FILE_ACCESS_READ,
            RETRO_VFS_FILE_ACCESS_HINT_NONE);

      if (!file)
         continue;

      line    = (char*)malloc(line_size);
      line[0] = '\0';

      /* even though the pass is set in the loop too, not all passes have parameters */
      param->pass = i;

      while (shader->num_parameters < ARRAY_SIZE(shader->parameters)
            && intfstream_gets(file, line, line_size))
      {
         int ret = sscanf(line,
               "#pragma parameter %63s \"%63[^\"]\" %f %f %f %f",
               param->id,        param->desc,    &param->initial,
               &param->minimum, &param->maximum, &param->step);

         if (ret < 5)
            continue;

         param->id[63]   = '\0';
         param->desc[63] = '\0';

         if (ret == 5)
            param->step = 0.1f * (param->maximum - param->minimum);

         param->pass = i;

         RARCH_LOG("Found #pragma parameter %s (%s) %f %f %f %f in pass %d\n",
               param->desc,    param->id,      param->initial,
               param->minimum, param->maximum, param->step, param->pass);
         param->current = param->initial;

         shader->num_parameters++;
         param++;
      }

      free(line);
      intfstream_close(file);
      free(file);
   }

   if (conf && !video_shader_resolve_current_parameters(conf, shader))
      return false;

   return true;
}

/**
 * video_shader_read_conf_preset:
 * @conf              : Preset file to read from.
 * @shader            : Shader passes handle.
 *
 * Loads preset file and all associated state (passes,
 * textures, imports, etc).
 *
 * Returns: true (1) if successful, otherwise false (0).
 **/
bool video_shader_read_conf_preset(config_file_t *conf,
      struct video_shader *shader)
{
   unsigned i;
   union string_list_elem_attr attr;
   unsigned shaders                 = 0;
   settings_t *settings             = config_get_ptr();
   struct string_list *file_list    = NULL;
   bool watch_files                 = settings->bools.video_shader_watch_files;

   (void)file_list;

   memset(shader, 0, sizeof(*shader));

   if (!config_get_uint(conf, "shaders", &shaders))
   {
      RARCH_ERR("Cannot find \"shaders\" param.\n");
      return false;
   }

   if (!shaders)
   {
      RARCH_ERR("Need to define at least 1 shader.\n");
      return false;
   }

   if (!config_get_int(conf, "feedback_pass",
            &shader->feedback_pass))
      shader->feedback_pass = -1;

   shader->passes = MIN(shaders, GFX_MAX_SHADERS);
   attr.i         = 0;

   strlcpy(shader->path, conf->path, sizeof(shader->path));

   if (watch_files)
   {
      if (file_change_data)
         frontend_driver_watch_path_for_changes(NULL,
               0, &file_change_data);

      file_change_data = NULL;
      file_list        = string_list_new();
      string_list_append(file_list, conf->path, attr);
   }

   for (i = 0; i < shader->passes; i++)
   {
      if (!video_shader_parse_pass(conf, &shader->pass[i], i))
      {
         if (file_list)
         {
            string_list_free(file_list);
            file_list = NULL;
         }
         return false;
      }

      if (watch_files && file_list)
         string_list_append(file_list,
               shader->pass[i].source.path, attr);
   }

   if (watch_files)
   {
      int flags = PATH_CHANGE_TYPE_MODIFIED          |
                  PATH_CHANGE_TYPE_WRITE_FILE_CLOSED |
                  PATH_CHANGE_TYPE_FILE_MOVED        |
                  PATH_CHANGE_TYPE_FILE_DELETED;

      frontend_driver_watch_path_for_changes(file_list,
            flags, &file_change_data);
      if (file_list)
         string_list_free(file_list);
   }

   command_event(CMD_EVENT_SHADER_PRESET_LOADED, NULL);

   if (!video_shader_parse_textures(conf, shader))
      return false;

   return true;
}

/* CGP store */
static const char *scale_type_to_str(enum gfx_scale_type type)
{
   switch (type)
   {
      case RARCH_SCALE_INPUT:
         return "source";
      case RARCH_SCALE_VIEWPORT:
         return "viewport";
      case RARCH_SCALE_ABSOLUTE:
         return "absolute";
      default:
         break;
   }

   return "?";
}

static void shader_write_scale_dim(config_file_t *conf,
      const char *dim,
      enum gfx_scale_type type, float scale,
      unsigned absolute, unsigned i)
{
   char key[64];

   key[0] = '\0';

   snprintf(key, sizeof(key), "scale_type_%s%u", dim, i);
   config_set_string(conf, key, scale_type_to_str(type));

   snprintf(key, sizeof(key), "scale_%s%u", dim, i);
   if (type == RARCH_SCALE_ABSOLUTE)
      config_set_int(conf, key, absolute);
   else
      config_set_float(conf, key, scale);
}

static void shader_write_fbo(config_file_t *conf,
      const struct gfx_fbo_scale *fbo, unsigned i)
{
   char key[64];

   key[0] = '\0';

   snprintf(key, sizeof(key), "float_framebuffer%u", i);
   config_set_bool(conf, key, fbo->fp_fbo);
   snprintf(key, sizeof(key), "srgb_framebuffer%u", i);
   config_set_bool(conf, key, fbo->srgb_fbo);

   if (!fbo->valid)
      return;

   shader_write_scale_dim(conf, "x", fbo->type_x,
         fbo->scale_x, fbo->abs_x, i);
   shader_write_scale_dim(conf, "y", fbo->type_y,
         fbo->scale_y, fbo->abs_y, i);
}

/**
 * video_shader_write_conf_preset:
 * @conf              : Preset file to write to.
 * @shader            : Shader passes handle.
 * @preset_path       : Optional path to where the preset will be written.
 *
 * Saves preset and all associated state (passes,
 * textures, imports, etc) to disk.
 * If @preset_path is not NULL, shader paths are saved
 * relative to it.
 **/
void video_shader_write_conf_preset(config_file_t *conf,
      struct video_shader *shader, const char *preset_path)
{
   unsigned i;
   char key[64];
   size_t tmp_size = PATH_MAX_LENGTH;
   char *tmp       = (char*)malloc(tmp_size);
   char *tmp_rel   = (char*)malloc(tmp_size);
   char *tmp_base  = (char*)malloc(tmp_size);

   if (!tmp || !tmp_rel || !tmp_base)
      return;

   config_set_int(conf, "shaders", shader->passes);
   if (shader->feedback_pass >= 0)
      config_set_int(conf, "feedback_pass", shader->feedback_pass);

   for (i = 0; i < shader->passes; i++)
   {
      const struct video_shader_pass *pass = &shader->pass[i];

      snprintf(key, sizeof(key), "shader%u", i);
      strlcpy(tmp, pass->source.path, tmp_size);

      if (preset_path)
      {
         strlcpy(tmp_base, preset_path, tmp_size);

         path_basedir(tmp_base);
         path_relative_to(tmp_rel, tmp, tmp_base, tmp_size);

         config_set_path(conf, key, tmp_rel);
      }
      else
         config_set_path(conf, key, tmp);


      if (pass->filter != RARCH_FILTER_UNSPEC)
      {
         snprintf(key, sizeof(key), "filter_linear%u", i);
         config_set_bool(conf, key, pass->filter == RARCH_FILTER_LINEAR);
      }

      snprintf(key, sizeof(key), "wrap_mode%u", i);
      config_set_string(conf, key, wrap_mode_to_str(pass->wrap));

      if (pass->frame_count_mod)
      {
         snprintf(key, sizeof(key), "frame_count_mod%u", i);
         config_set_int(conf, key, pass->frame_count_mod);
      }

      snprintf(key, sizeof(key), "mipmap_input%u", i);
      config_set_bool(conf, key, pass->mipmap);

      snprintf(key, sizeof(key), "alias%u", i);
      config_set_string(conf, key, pass->alias);

      shader_write_fbo(conf, &pass->fbo, i);
   }

   free(tmp);
   free(tmp_rel);
   free(tmp_base);

   if (shader->num_parameters)
   {
      size_t param_size = 4096 * sizeof(char);
      char *parameters  = (char*)malloc(param_size);

      if (parameters)
      {
         parameters[0] = '\0';

         strlcpy(parameters, shader->parameters[0].id, param_size);

         for (i = 1; i < shader->num_parameters; i++)
         {
            /* O(n^2), but number of parameters is very limited. */
            strlcat(parameters, ";", param_size);
            strlcat(parameters, shader->parameters[i].id, param_size);
         }

         config_set_string(conf, "parameters", parameters);

         for (i = 0; i < shader->num_parameters; i++)
            config_set_float(conf, shader->parameters[i].id,
                  shader->parameters[i].current);
         free(parameters);
      }
   }

   if (shader->luts)
   {
      size_t tex_size = 4096 * sizeof(char);
      char *textures  = (char*)malloc(tex_size);

      if (textures)
      {
         textures[0] = '\0';

         strlcpy(textures, shader->lut[0].id, tex_size);

         for (i = 1; i < shader->luts; i++)
         {
            /* O(n^2), but number of textures is very limited. */
            strlcat(textures, ";", tex_size);
            strlcat(textures, shader->lut[i].id, tex_size);
         }

         config_set_string(conf, "textures", textures);

         free(textures);

         for (i = 0; i < shader->luts; i++)
         {
            char key[128];

            key[0] = '\0';

            config_set_string(conf, shader->lut[i].id, shader->lut[i].path);

            if (shader->lut[i].filter != RARCH_FILTER_UNSPEC)
            {
               snprintf(key, sizeof(key), "%s_linear", shader->lut[i].id);
               config_set_bool(conf, key,
                     shader->lut[i].filter == RARCH_FILTER_LINEAR);
            }

            snprintf(key, sizeof(key),
                  "%s_wrap_mode", shader->lut[i].id);
            config_set_string(conf, key,
                  wrap_mode_to_str(shader->lut[i].wrap));

            snprintf(key, sizeof(key),
                  "%s_mipmap", shader->lut[i].id);
            config_set_bool(conf, key,
                  shader->lut[i].mipmap);
         }
      }
   }
}

const char *video_shader_to_str(enum rarch_shader_type type)
{
   switch (type)
   {
      case RARCH_SHADER_CG:
         return "Cg";
      case RARCH_SHADER_HLSL:
         return "HLSL";
      case RARCH_SHADER_GLSL:
         return "GLSL";
      case RARCH_SHADER_SLANG:
         return "Slang";
      case RARCH_SHADER_METAL:
         return "Metal";
      case RARCH_SHADER_NONE:
         return "none";
      default:
         break;
   }

   return "???";
}

/**
 * video_shader_is_supported:
 * Tests if a shader type is supported.
 * This is only accurate once the context driver was initialized.
 **/
bool video_shader_is_supported(enum rarch_shader_type type)
{
   gfx_ctx_flags_t flags;
   enum display_flags testflag;

   switch (type)
   {
      case RARCH_SHADER_SLANG:
         testflag = GFX_CTX_FLAGS_SHADERS_SLANG;
         break;
      case RARCH_SHADER_GLSL:
         testflag = GFX_CTX_FLAGS_SHADERS_GLSL;
         break;
      case RARCH_SHADER_CG:
         testflag = GFX_CTX_FLAGS_SHADERS_CG;
         break;
      case RARCH_SHADER_HLSL:
         testflag = GFX_CTX_FLAGS_SHADERS_HLSL;
         break;
      case RARCH_SHADER_NONE:
      default:
         return false;
   }
   video_context_driver_get_flags(&flags);

   return BIT32_GET(flags.flags, testflag);
}

const char *video_shader_get_preset_extension(enum rarch_shader_type type)
{
   switch (type)
   {
      case RARCH_SHADER_GLSL:
         return file_path_str(FILE_PATH_GLSLP_EXTENSION);
      case RARCH_SHADER_SLANG:
         return file_path_str(FILE_PATH_SLANGP_EXTENSION);
      case RARCH_SHADER_HLSL:
      case RARCH_SHADER_CG:
         return file_path_str(FILE_PATH_CGP_EXTENSION);
      default:
         break;
   }

   return NULL;
}

bool video_shader_any_supported(void)
{
   return
      video_shader_is_supported(RARCH_SHADER_SLANG) ||
      video_shader_is_supported(RARCH_SHADER_HLSL)  ||
      video_shader_is_supported(RARCH_SHADER_GLSL)  ||
      video_shader_is_supported(RARCH_SHADER_CG);
}

enum rarch_shader_type video_shader_get_type_from_ext(const char *ext,
      bool *is_preset)
{
   if (string_is_empty(ext))
      return RARCH_SHADER_NONE;

   if (strlen(ext) > 1 && ext[0] == '.')
      ext++;

   *is_preset =
      string_is_equal_case_insensitive(ext, "cgp")   ||
      string_is_equal_case_insensitive(ext, "glslp") ||
      string_is_equal_case_insensitive(ext, "slangp");

   if (string_is_equal_case_insensitive(ext, "cgp") ||
       string_is_equal_case_insensitive(ext, "cg")
      )
      return RARCH_SHADER_CG;

   if (string_is_equal_case_insensitive(ext, "glslp") ||
       string_is_equal_case_insensitive(ext, "glsl")
      )
      return RARCH_SHADER_GLSL;

   if (string_is_equal_case_insensitive(ext, "slangp") ||
       string_is_equal_case_insensitive(ext, "slang")
      )
      return RARCH_SHADER_SLANG;

   return RARCH_SHADER_NONE;
}

/**
 * video_shader_parse_type:
 * @path              : Shader path.
 *
 * Parses type of shader.
 *
 * Returns: value of shader type if it could be determined,
 * otherwise RARCH_SHADER_NONE.
 **/
enum rarch_shader_type video_shader_parse_type(const char *path)
{
   bool is_preset = false;
   if (!path)
      return RARCH_SHADER_NONE;
   return video_shader_get_type_from_ext(path_get_extension(path), &is_preset);
}

/**
 * video_shader_resolve_relative:
 * @shader            : Shader pass handle.
 * @ref_path          : Relative shader path.
 *
 * Resolves relative shader path (@ref_path) into absolute
 * shader paths.
 **/
void video_shader_resolve_relative(struct video_shader *shader,
      const char *ref_path)
{
   unsigned i;
   size_t tmp_path_size = 4096 * sizeof(char);
   char *tmp_path       = (char*)malloc(tmp_path_size);

   if (!tmp_path)
      return;

   tmp_path[0] = '\0';

   for (i = 0; i < shader->passes; i++)
   {
      if (!*shader->pass[i].source.path)
         continue;

      strlcpy(tmp_path, shader->pass[i].source.path, tmp_path_size);
      fill_pathname_resolve_relative(shader->pass[i].source.path,
            ref_path, tmp_path, sizeof(shader->pass[i].source.path));
   }

   for (i = 0; i < shader->luts; i++)
   {
      strlcpy(tmp_path, shader->lut[i].path, tmp_path_size);
      fill_pathname_resolve_relative(shader->lut[i].path,
            ref_path, tmp_path, sizeof(shader->lut[i].path));
   }

   free(tmp_path);
}

bool video_shader_check_for_changes(void)
{
   if (!file_change_data)
      return false;

   return frontend_driver_check_for_path_changes(file_change_data);
}
