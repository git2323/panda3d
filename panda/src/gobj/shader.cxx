/**
 * PANDA 3D SOFTWARE
 * Copyright (c) Carnegie Mellon University.  All rights reserved.
 *
 * All use of this software is subject to the terms of the revised BSD
 * license.  You should have received a copy of this license along
 * with this source code in a file named "LICENSE."
 *
 * @file shader.cxx
 * @author jyelon
 * @date 2005-09-01
 * @author fperazzi, PandaSE
 * @date 2010-04-06
 * @author fperazzi, PandaSE
 * @date 2010-04-29
 */

#include "pandabase.h"
#include "shader.h"
#include "preparedGraphicsObjects.h"
#include "virtualFileSystem.h"
#include "config_putil.h"
#include "bamCache.h"
#include "string_utils.h"
#include "shaderModuleGlsl.h"
#include "shaderCompilerRegistry.h"
#include "shaderCompiler.h"
#include "shaderCompilerCg.h"

#ifdef HAVE_CG
#include <Cg/cg.h>
#endif

using std::istream;
using std::move;
using std::ostream;
using std::ostringstream;
using std::string;

TypeHandle Shader::_type_handle;
Shader::ShaderTable Shader::_load_table;
Shader::ShaderTable Shader::_make_table;
Shader::ShaderCaps Shader::_default_caps;
int Shader::_shaders_generated;

#ifdef HAVE_CG
CGcontext Shader::_cg_context = 0;

#endif

/**
 * Determine whether the source file hints at being a Cg shader
 */
static bool has_cg_header(const std::string &shader_text) {
  size_t newline_pos = shader_text.find('\n');
  std::string search_str = shader_text.substr(0, newline_pos);
  return search_str.rfind("//Cg") != std::string::npos;
}

/**
 * Generate an error message including a description of the specified
 * parameter.
 */
void Shader::
cp_report_error(ShaderArgInfo &p, const string &msg) {
  Filename fn = get_filename(p._id._type);
  shader_cat.error() << fn << ": " << *p._type << ' ' <<
    p._id._name << ": " << msg << "\n";
}

/**
 * Make sure the provided parameter contains the specified number of words.
 * If not, print error message and return false.
 */
bool Shader::
cp_errchk_parameter_words(ShaderArgInfo &p, int len)
{
  vector_string words;
  tokenize(p._id._name, words, "_");
  if ((int)words.size() != len) {
    cp_report_error(p, "parameter name has wrong number of words");
    return false;
  }
  return true;
}

/**
 * Make sure the provided parameter has a floating point type.  If not, print
 * error message and return false.
 */
bool Shader::
cp_errchk_parameter_float(ShaderArgInfo &p, int lo, int hi)
{
  int nfloat;
  if (p._type->as_scalar()) {
    nfloat = 1;
  }
  else if (const ::ShaderType::Vector *vector = p._type->as_vector()) {
    nfloat = vector->get_num_elements();
  }
  else if (const ::ShaderType::Matrix *matrix = p._type->as_matrix()) {
    nfloat = matrix->get_num_rows() * matrix->get_num_columns();
  }
  else {
    nfloat = 0;
  }
  if (nfloat < lo || nfloat > hi) {
    string msg = "wrong type for parameter:";
    cp_report_error(p, msg);
    return false;
  }
  return true;
}

/**
 * Make sure the next thing on the word list is EOL
 */
bool Shader::
cp_parse_eol(ShaderArgInfo &p, vector_string &words, int &next) {
  if (words[next] != "") {
    cp_report_error(p, "Too many words in parameter");
    return false;
  }
  return true;
}

/**
 * Pop a delimiter ('to' or 'rel') from the word list.
 */
bool Shader::
cp_parse_delimiter(ShaderArgInfo &p, vector_string &words, int &next) {
  if ((words[next] != "to")&&(words[next] != "rel")) {
    cp_report_error(p, "Keyword 'to' or 'rel' expected");
    return false;
  }
  next += 1;
  return true;
}

/**
 * Pop a non-delimiter word from the word list.  Delimiters are 'to' and
 * 'rel.'
 */
string Shader::
cp_parse_non_delimiter(vector_string &words, int &next) {
  const string &nword = words[next];
  if ((nword == "")||(nword == "to")||(nword == "rel")) {
    return "";
  }
  next += 1;
  return nword;
}

/**
 * Convert a single-word coordinate system name into a PART/ARG of a
 * ShaderMatSpec.
 */
bool Shader::
cp_parse_coord_sys(ShaderArgInfo &p,
                   vector_string &pieces, int &next,
                   ShaderMatSpec &bind, bool fromflag) {

  string word1 = cp_parse_non_delimiter(pieces, next);
  if (pieces[next] == "of") next++;
  string word2 = cp_parse_non_delimiter(pieces, next);

  ShaderMatInput from_single;
  ShaderMatInput from_double;
  ShaderMatInput to_single;
  ShaderMatInput to_double;

  if (word1 == "") {
    cp_report_error(p, "Could not parse coordinate system name");
    return false;
  } else if (word1 == "world") {
    from_single = SMO_world_to_view;
    from_double = SMO_INVALID;
    to_single   = SMO_view_to_world;
    to_double   = SMO_INVALID;
  } else if (word1 == "model") {
    from_single = SMO_model_to_view;
    from_double = SMO_view_x_to_view;
    to_single   = SMO_view_to_model;
    to_double   = SMO_view_to_view_x;
  } else if (word1 == "clip") {
    from_single = SMO_clip_to_view;
    from_double = SMO_clip_x_to_view;
    to_single   = SMO_view_to_clip;
    to_double   = SMO_view_to_clip_x;
  } else if (word1 == "view") {
    from_single = SMO_identity;
    from_double = SMO_view_x_to_view;
    to_single   = SMO_identity;
    to_double   = SMO_view_to_view_x;
  } else if (word1 == "apiview") {
    from_single = SMO_apiview_to_view;
    from_double = SMO_apiview_x_to_view;
    to_single   = SMO_view_to_apiview;
    to_double   = SMO_view_to_apiview_x;
  } else if (word1 == "apiclip") {
    from_single = SMO_apiclip_to_view;
    from_double = SMO_apiclip_x_to_view;
    to_single   = SMO_view_to_apiclip;
    to_double   = SMO_view_to_apiclip_x;
  } else {
    from_single = SMO_view_x_to_view;
    from_double = SMO_view_x_to_view;
    to_single   = SMO_view_to_view_x;
    to_double   = SMO_view_to_view_x;
    word2 = word1;
  }

  if (fromflag) {
    if (word2 == "") {
      bind._part[0] = from_single;
      bind._arg[0] = nullptr;
    } else {
      if (from_double == SMO_INVALID) {
        cp_report_error(p, "Could not parse coordinate system name");
        return false;
      }
      bind._part[0] = from_double;
      bind._arg[0] = InternalName::make(word2);
    }
  } else {
    if (word2 == "") {
      bind._part[1] = to_single;
      bind._arg[1] = nullptr;
    } else {
      if (to_double == SMO_INVALID) {
        cp_report_error(p, "Could not parse coordinate system name");
        return false;
      }
      bind._part[1] = to_double;
      bind._arg[1] = InternalName::make(word2);
    }
  }
  return true;
}

/**
 * Given ShaderMatInput, returns an indication of what part or parts of the
 * state_and_transform the ShaderMatInput depends upon.
 */
int Shader::
cp_dependency(ShaderMatInput inp) {

  int dep = SSD_general;

  if (inp == SMO_INVALID) {
    return SSD_NONE;
  }
  if (inp == SMO_attr_material || inp == SMO_attr_material2) {
    dep |= SSD_material | SSD_frame;
  }
  if (inp == SMO_attr_color || inp == SMO_attr_material2) {
    dep |= SSD_color;
  }
  if (inp == SMO_attr_colorscale) {
    dep |= SSD_colorscale;
  }
  if (inp == SMO_attr_fog || inp == SMO_attr_fogcolor) {
    dep |= SSD_fog;
  }
  if ((inp == SMO_model_to_view) ||
      (inp == SMO_view_to_model) ||
      (inp == SMO_model_to_apiview) ||
      (inp == SMO_apiview_to_model)) {
    dep |= SSD_transform;
  }
  if ((inp == SMO_view_to_world) ||
      (inp == SMO_world_to_view) ||
      (inp == SMO_view_x_to_view) ||
      (inp == SMO_view_to_view_x) ||
      (inp == SMO_apiview_x_to_view) ||
      (inp == SMO_view_to_apiview_x) ||
      (inp == SMO_clip_x_to_view) ||
      (inp == SMO_view_to_clip_x) ||
      (inp == SMO_apiclip_x_to_view) ||
      (inp == SMO_view_to_apiclip_x) ||
      (inp == SMO_dlight_x) ||
      (inp == SMO_plight_x) ||
      (inp == SMO_slight_x)) {
    dep |= SSD_view_transform;
  }
  if ((inp == SMO_texpad_x) ||
      (inp == SMO_texpix_x) ||
      (inp == SMO_alight_x) ||
      (inp == SMO_dlight_x) ||
      (inp == SMO_plight_x) ||
      (inp == SMO_slight_x) ||
      (inp == SMO_satten_x) ||
      (inp == SMO_mat_constant_x) ||
      (inp == SMO_mat_constant_x_attrib) ||
      (inp == SMO_vec_constant_x) ||
      (inp == SMO_vec_constant_x_attrib) ||
      (inp == SMO_view_x_to_view) ||
      (inp == SMO_view_to_view_x) ||
      (inp == SMO_apiview_x_to_view) ||
      (inp == SMO_view_to_apiview_x) ||
      (inp == SMO_clip_x_to_view) ||
      (inp == SMO_view_to_clip_x) ||
      (inp == SMO_apiclip_x_to_view) ||
      (inp == SMO_view_to_apiclip_x)) {
    dep |= SSD_shaderinputs;

    if ((inp == SMO_texpad_x) ||
        (inp == SMO_texpix_x) ||
        (inp == SMO_alight_x) ||
        (inp == SMO_dlight_x) ||
        (inp == SMO_plight_x) ||
        (inp == SMO_slight_x) ||
        (inp == SMO_satten_x) ||
        (inp == SMO_mat_constant_x) ||
        (inp == SMO_mat_constant_x_attrib) ||
        (inp == SMO_vec_constant_x_attrib) ||
        (inp == SMO_view_x_to_view) ||
        (inp == SMO_view_to_view_x) ||
        (inp == SMO_apiview_x_to_view) ||
        (inp == SMO_view_to_apiview_x) ||
        (inp == SMO_clip_x_to_view) ||
        (inp == SMO_view_to_clip_x) ||
        (inp == SMO_apiclip_x_to_view) ||
        (inp == SMO_view_to_apiclip_x)) {
      // We can't track changes to these yet, so we have to assume that they
      // are modified every frame.
      dep |= SSD_frame;
    }
  }
  if ((inp == SMO_light_ambient) ||
      (inp == SMO_light_source_i_attrib) ||
      (inp == SMO_light_source_i_packed)) {
    dep |= SSD_light | SSD_frame;
  }
  if (inp == SMO_light_source_i_attrib ||
      inp == SMO_light_source_i_packed ||
      inp == SMO_mat_constant_x_attrib ||
      inp == SMO_vec_constant_x_attrib) {
    // Some light attribs (eg. position) need to be transformed to view space.
    dep |= SSD_view_transform;
  }
  if ((inp == SMO_light_product_i_ambient) ||
      (inp == SMO_light_product_i_diffuse) ||
      (inp == SMO_light_product_i_specular)) {
    dep |= (SSD_light | SSD_material);
  }
  if ((inp == SMO_clipplane_x) ||
      (inp == SMO_apiview_clipplane_i)) {
    dep |= SSD_clip_planes;
  }
  if (inp == SMO_texmat_i || inp == SMO_inv_texmat_i || inp == SMO_texscale_i) {
    dep |= SSD_tex_matrix;
  }
  if ((inp == SMO_window_size) ||
      (inp == SMO_pixel_size) ||
      (inp == SMO_frame_number) ||
      (inp == SMO_frame_time) ||
      (inp == SMO_frame_delta)) {
    dep |= SSD_frame;
  }
  if ((inp == SMO_clip_to_view) ||
      (inp == SMO_view_to_clip) ||
      (inp == SMO_apiclip_to_view) ||
      (inp == SMO_view_to_apiclip) ||
      (inp == SMO_apiview_to_apiclip) ||
      (inp == SMO_apiclip_to_apiview)) {
    dep |= SSD_projection;
  }
  if (inp == SMO_tex_is_alpha_i || inp == SMO_texcolor_i) {
    dep |= SSD_texture | SSD_frame;
  }

  return dep;
}

/**
 * Adds the given ShaderMatSpec to the shader's mat spec table.
 */
void Shader::
cp_add_mat_spec(ShaderMatSpec &spec) {
  // If we're composing with identity, simplify.

  if (spec._func == SMF_first) {
    spec._part[1] = SMO_INVALID;
    spec._arg[1] = nullptr;
  }
  if (spec._func == SMF_compose) {
    if (spec._part[1] == SMO_identity) {
      spec._func = SMF_first;
    }
  }
  if (spec._func == SMF_compose) {
    if (spec._part[0] == SMO_identity) {
      spec._func = SMF_first;
      spec._part[0] = spec._part[1];
      spec._arg[0] = spec._arg[1];
    }

    // More optimal combinations for common matrices.

    if (spec._part[0] == SMO_model_to_view &&
        spec._part[1] == SMO_view_to_apiclip) {
      spec._part[0] = SMO_model_to_apiview;
      spec._part[1] = SMO_apiview_to_apiclip;

    } else if (spec._part[0] == SMO_apiclip_to_view &&
               spec._part[1] == SMO_view_to_model) {
      spec._part[0] = SMO_apiclip_to_apiview;
      spec._part[1] = SMO_apiview_to_model;

    } else if (spec._part[0] == SMO_apiview_to_view &&
               spec._part[1] == SMO_view_to_apiclip) {
      spec._func = SMF_first;
      spec._part[0] = SMO_apiview_to_apiclip;
      spec._part[1] = SMO_identity;

    } else if (spec._part[0] == SMO_apiclip_to_view &&
               spec._part[1] == SMO_view_to_apiview) {
      spec._func = SMF_first;
      spec._part[0] = SMO_apiclip_to_apiview;
      spec._part[1] = SMO_identity;

    } else if (spec._part[0] == SMO_apiview_to_view &&
               spec._part[1] == SMO_view_to_model) {
      spec._func = SMF_first;
      spec._part[0] = SMO_apiview_to_model;
      spec._part[1] = SMO_identity;

    } else if (spec._part[0] == SMO_model_to_view &&
               spec._part[1] == SMO_view_to_apiview) {
      spec._func = SMF_first;
      spec._part[0] = SMO_model_to_apiview;
      spec._part[1] = SMO_identity;
    }
  }

  // Determine which part is an array, for determining which one the count and
  // index refer to.  (It can't be the case that both parts are arrays.)
  int begin[2] = {0, 0};
  int end[2] = {1, 1};
  if (spec._index > 0) {
    for (int i = 0; i < 2; ++i) {
      if (spec._part[i] == SMO_texmat_i ||
          spec._part[i] == SMO_inv_texmat_i ||
          spec._part[i] == SMO_light_source_i_attrib ||
          spec._part[i] == SMO_light_product_i_ambient ||
          spec._part[i] == SMO_light_product_i_diffuse ||
          spec._part[i] == SMO_light_product_i_specular ||
          spec._part[i] == SMO_apiview_clipplane_i ||
          spec._part[i] == SMO_tex_is_alpha_i ||
          spec._part[i] == SMO_transform_i ||
          spec._part[i] == SMO_slider_i ||
          spec._part[i] == SMO_light_source_i_packed ||
          spec._part[i] == SMO_texscale_i ||
          spec._part[i] == SMO_texcolor_i) {
        begin[i] = spec._index;
        end[i] = spec._index + 1;
      }
    }
    nassertv(end[0] == 1 || end[1] == 1);
  }

  // Make sure that we have a place in the part cache for both parts.
  int num_parts = (spec._func != SMF_first) ? 2 : 1;

  for (int p = 0; p < num_parts; ++p) {
    int dep = cp_dependency(spec._part[p]);
    spec._dep |= dep;

    // Do we already have a spot in the cache for this part?
    size_t i;
    size_t offset = 0;
    for (i = 0; i < _mat_parts.size(); ++i) {
      ShaderMatPart &part = _mat_parts[i];
      if (part._part == spec._part[p] && part._arg == spec._arg[p]) {
        int diff = end[p] - part._count;
        if (diff <= 0) {
          // The existing cache entry is big enough.
          break;
        } else {
          // It's not big enough.  Enlarge it, which means we have to change the
          // offset of some of the other spec entries.
          for (ShaderMatSpec &spec : _mat_spec) {
            if (spec._cache_offset[0] >= offset + part._count) {
              spec._cache_offset[0] += diff;
            }
            if (spec._cache_offset[1] >= offset + part._count) {
              spec._cache_offset[1] += diff;
            }
          }
          part._count = end[p];
          break;
        }
      }
      offset += part._count;
    }
    if (i == _mat_parts.size()) {
      // Didn't find this part yet, create a new one.
      ShaderMatPart part;
      part._part = spec._part[p];
      part._count = end[p];
      part._arg = spec._arg[p];
      part._dep = dep;
      _mat_parts.push_back(std::move(part));
    }
    spec._cache_offset[p] = offset + begin[p];
  }

  _mat_spec.push_back(spec);
  _mat_deps |= spec._dep;
}

/**
 * Returns the total size of the matrix part cache.
 */
size_t Shader::
cp_get_mat_cache_size() const {
  size_t size = 0;
  for (const ShaderMatPart &part : _mat_parts) {
    size += part._count;
  }
  return size;
}

/**
 * Analyzes a parameter and decides how to bind the parameter to some part of
 * panda's internal state.  Updates one of the bind arrays to cause the
 * binding to occur.
 *
 * If there is an error, this routine will append an error message onto the
 * error messages.
 */
bool Shader::
compile_parameter(ShaderArgInfo &p) {
  if (p._id._name.size() == 0) return true;
  if (p._id._name[0] == '$') return true;

  // It could be inside a struct, strip off everything before the last dot.
  size_t loc = p._id._name.find_last_of('.');

  string basename (p._id._name);
  string struct_name  ("");

  if (loc < string::npos) {
    basename = p._id._name.substr(loc + 1);
    struct_name =  p._id._name.substr(0,loc+1);
  }

  // Split it at the underscores.
  vector_string pieces;
  tokenize(basename, pieces, "_");

  if (basename.size() >= 2 && basename.substr(0, 2) == "__") {
    return true;
  }

  if (pieces[0] == "mat" && pieces[1] == "shadow") {
    if ((!cp_errchk_parameter_words(p,3))||
        (!cp_errchk_parameter_float(p,16,16))) {
      return false;
    }
    ShaderMatSpec bind;
    bind._id = p._id;
    bind._piece = SMP_whole;
    bind._func = SMF_compose;
    bind._part[1] = SMO_light_source_i_attrib;
    bind._arg[1] = InternalName::make("shadowViewMatrix");
    bind._part[0] = SMO_view_to_apiview;
    bind._arg[0] = nullptr;
    bind._index = atoi(pieces[2].c_str());

    cp_add_mat_spec(bind);
    return true;
  }

  // Implement some macros.  Macros work by altering the contents of the
  // 'pieces' array, and then falling through.

  if (pieces[0] == "mstrans") {
    pieces[0] = "trans";
    pieces.push_back("to");
    pieces.push_back("model");
  }
  if (pieces[0] == "wstrans") {
    pieces[0] = "trans";
    pieces.push_back("to");
    pieces.push_back("world");
  }
  if (pieces[0] == "vstrans") {
    pieces[0] = "trans";
    pieces.push_back("to");
    pieces.push_back("view");
  }
  if (pieces[0] == "cstrans") {
    pieces[0] = "trans";
    pieces.push_back("to");
    pieces.push_back("clip");
  }
  if (pieces[0] == "mspos") {
    pieces[0] = "row3";
    pieces.push_back("to");
    pieces.push_back("model");
  }
  if (pieces[0] == "wspos") {
    pieces[0] = "row3";
    pieces.push_back("to");
    pieces.push_back("world");
  }
  if (pieces[0] == "vspos") {
    pieces[0] = "row3";
    pieces.push_back("to");
    pieces.push_back("view");
  }
  if (pieces[0] == "cspos") {
    pieces[0] = "row3";
    pieces.push_back("to");
    pieces.push_back("clip");
  }

  // Implement the modelview macros.

  if ((pieces[0] == "mat")||(pieces[0] == "inv")||
      (pieces[0] == "tps")||(pieces[0] == "itp")) {
    if (!cp_errchk_parameter_words(p, 2)) {
      return false;
    }
    string trans = pieces[0];
    string matrix = pieces[1];
    pieces.clear();
    if (matrix == "modelview") {
      tokenize("trans_model_to_apiview", pieces, "_");
    } else if (matrix == "projection") {
      tokenize("trans_apiview_to_apiclip", pieces, "_");
    } else if (matrix == "modelproj") {
      tokenize("trans_model_to_apiclip", pieces, "_");
    } else {
      cp_report_error(p,"unrecognized matrix name");
      return false;
    }
    if (trans=="mat") {
      pieces[0] = "trans";
    } else if (trans=="inv") {
      string t = pieces[1];
      pieces[1] = pieces[3];
      pieces[3] = t;
    } else if (trans=="tps") {
      pieces[0] = "tpose";
    } else if (trans=="itp") {
      string t = pieces[1];
      pieces[1] = pieces[3];
      pieces[3] = t;
      pieces[0] = "tpose";
    }
  }

  // Implement the transform-matrix generator.

  if ((pieces[0]=="trans")||
      (pieces[0]=="tpose")||
      (pieces[0]=="row0")||
      (pieces[0]=="row1")||
      (pieces[0]=="row2")||
      (pieces[0]=="row3")||
      (pieces[0]=="col0")||
      (pieces[0]=="col1")||
      (pieces[0]=="col2")||
      (pieces[0]=="col3")) {

    ShaderMatSpec bind;
    bind._id = p._id;
    bind._piece = SMP_whole;
    bind._func = SMF_compose;
    bind._part[1] = SMO_light_source_i_attrib;
    bind._arg[1] = InternalName::make("shadowViewMatrix");
    bind._part[0] = SMO_view_to_apiview;
    bind._arg[0] = nullptr;
    bind._index = atoi(pieces[2].c_str());

    int next = 1;
    pieces.push_back("");

    // Decide whether this is a matrix or vector.
    if      (pieces[0]=="trans")   bind._piece = SMP_whole;
    else if (pieces[0]=="tpose")   bind._piece = SMP_transpose;
    else if (pieces[0]=="row0")    bind._piece = SMP_row0;
    else if (pieces[0]=="row1")    bind._piece = SMP_row1;
    else if (pieces[0]=="row2")    bind._piece = SMP_row2;
    else if (pieces[0]=="row3")    bind._piece = SMP_row3;
    else if (pieces[0]=="col0")    bind._piece = SMP_col0;
    else if (pieces[0]=="col1")    bind._piece = SMP_col1;
    else if (pieces[0]=="col2")    bind._piece = SMP_col2;
    else if (pieces[0]=="col3")    bind._piece = SMP_col3;
    if (bind._piece == SMP_whole || bind._piece == SMP_transpose) {
      if (const ::ShaderType::Matrix *matrix = p._type->as_matrix()) {
        if (matrix->get_num_rows() == 3 && matrix->get_num_columns() == 3) {
          if (bind._piece == SMP_transpose) {
            bind._piece = SMP_transpose3x3;
          } else {
            bind._piece = SMP_upper3x3;
          }
        } else {
          cp_errchk_parameter_float(p, 9, 9);
          return false;
        }
      } else if (!cp_errchk_parameter_float(p, 16, 16)) {
        return false;
      }
    } else {
      if (!cp_errchk_parameter_float(p, 4, 4)) return false;
    }

    if (!cp_parse_coord_sys(p, pieces, next, bind, true)) {
      return false;
    }
    if (!cp_parse_delimiter(p, pieces, next)) {
      return false;
    }
    if (!cp_parse_coord_sys(p, pieces, next, bind, false)) {
      return false;
    }
    if (!cp_parse_eol(p, pieces, next)) {
      return false;
    }
    cp_add_mat_spec(bind);
    return true;
  }

  // Special parameter: attr_material or attr_color

  if (pieces[0] == "attr") {
    if (!cp_errchk_parameter_words(p, 2)) {
      return false;
    }
    ShaderMatSpec bind;
    if (pieces[1] == "material") {
      if (!cp_errchk_parameter_float(p,16,16)) {
        return false;
      }
      bind._id = p._id;
      bind._piece = SMP_transpose;
      bind._func = SMF_first;
      bind._part[0] = SMO_attr_material;
      bind._arg[0] = nullptr;
      bind._part[1] = SMO_identity;
      bind._arg[1] = nullptr;
    } else if (pieces[1] == "color") {
      if (!cp_errchk_parameter_float(p,3,4)) {
        return false;
      }
      bind._id = p._id;
      bind._piece = SMP_row3;
      bind._func = SMF_first;
      bind._part[0] = SMO_attr_color;
      bind._arg[0] = nullptr;
      bind._part[1] = SMO_identity;
      bind._arg[1] = nullptr;
    } else if (pieces[1] == "colorscale") {
      if (!cp_errchk_parameter_float(p,3,4)) {
        return false;
      }
      bind._id = p._id;
      bind._piece = SMP_row3;
      bind._func = SMF_first;
      bind._part[0] = SMO_attr_colorscale;
      bind._arg[0] = nullptr;
      bind._part[1] = SMO_identity;
      bind._arg[1] = nullptr;
    } else if (pieces[1] == "fog") {
      if (!cp_errchk_parameter_float(p,3,4)) {
        return false;
      }
      bind._id = p._id;
      bind._piece = SMP_row3;
      bind._func = SMF_first;
      bind._part[0] = SMO_attr_fog;
      bind._arg[0] = nullptr;
      bind._part[1] = SMO_identity;
      bind._arg[1] = nullptr;
    } else if (pieces[1] == "fogcolor") {
      if (!cp_errchk_parameter_float(p,3,4)) {
        return false;
      }
      bind._id = p._id;
      bind._piece = SMP_row3;
      bind._func = SMF_first;
      bind._part[0] = SMO_attr_fogcolor;
      bind._arg[0] = nullptr;
      bind._part[1] = SMO_identity;
      bind._arg[1] = nullptr;
    } else if (pieces[1] == "ambient") {
      if (!cp_errchk_parameter_float(p,3,4)) {
        return false;
      }
      bind._id = p._id;
      bind._piece = SMP_row3;
      bind._func = SMF_first;
      bind._part[0] = SMO_light_ambient;
      bind._arg[0] = nullptr;
      bind._part[1] = SMO_identity;
      bind._arg[1] = nullptr;
    } else if (pieces[1].compare(0, 5, "light") == 0) {
      if (!cp_errchk_parameter_float(p,16,16)) {
        return false;
      }
      bind._id = p._id;
      bind._piece = SMP_transpose;
      bind._func = SMF_first;
      bind._part[0] = SMO_light_source_i_packed;
      bind._arg[0] = nullptr;
      bind._part[1] = SMO_identity;
      bind._arg[1] = nullptr;
      bind._index = atoi(pieces[1].c_str() + 5);
    } else if (pieces[1].compare(0, 5, "lspec") == 0) {
      if (!cp_errchk_parameter_float(p,3,4)) {
        return false;
      }
      bind._id = p._id;
      bind._piece = SMP_row3;
      bind._func = SMF_first;
      bind._part[0] = SMO_light_source_i_attrib;
      bind._arg[0] = InternalName::make("specular");
      bind._part[1] = SMO_identity;
      bind._arg[1] = nullptr;
      bind._index = atoi(pieces[1].c_str() + 5);
    } else {
      cp_report_error(p,"Unknown attr parameter.");
      return false;
    }

    cp_add_mat_spec(bind);
    return true;
  }

  if (pieces[0] == "color") {
    if (!cp_errchk_parameter_words(p, 1)) {
      return false;
    }
    ShaderMatSpec bind;

    cp_add_mat_spec(bind);
    return true;
  }

  // Keywords to access light properties.

  if (pieces[0] == "alight") {
    if ((!cp_errchk_parameter_words(p,2))||
        (!cp_errchk_parameter_float(p,3,4))) {
      return false;
    }
    ShaderMatSpec bind;
    bind._id = p._id;
    bind._piece = SMP_row3;
    bind._func = SMF_first;
    bind._part[0] = SMO_alight_x;
    bind._arg[0] = InternalName::make(pieces[1]);
    bind._part[1] = SMO_identity;
    bind._arg[1] = nullptr;

    cp_add_mat_spec(bind);
    return true;
  }

  if (pieces[0] == "satten") {
    if ((!cp_errchk_parameter_words(p,2))||
        (!cp_errchk_parameter_float(p,4,4))) {
      return false;
    }
    ShaderMatSpec bind;
    bind._id = p._id;
    bind._piece = SMP_row3;
    bind._func = SMF_first;
    bind._part[0] = SMO_satten_x;
    bind._arg[0] = InternalName::make(pieces[1]);
    bind._part[1] = SMO_identity;
    bind._arg[1] = nullptr;

    cp_add_mat_spec(bind);
    return true;
  }

  if ((pieces[0]=="dlight")||(pieces[0]=="plight")||(pieces[0]=="slight")) {
    if (!cp_errchk_parameter_float(p,16,16)) {
      return false;
    }
    ShaderMatSpec bind;
    bind._id = p._id;
    bind._piece = SMP_transpose;
    int next = 1;
    pieces.push_back("");
    if (pieces[next] == "") {
      cp_report_error(p, "Light name expected");
      return false;
    }
    if (pieces[0] == "dlight") {
      bind._func = SMF_transform_dlight;
      bind._part[0] = SMO_dlight_x;
    } else if (pieces[0] == "plight") {
      bind._func = SMF_transform_plight;
      bind._part[0] = SMO_plight_x;
    } else if (pieces[0] == "slight") {
      bind._func = SMF_transform_slight;
      bind._part[0] = SMO_slight_x;
    }
    bind._arg[0] = InternalName::make(pieces[next]);
    next += 1;
    if (!cp_parse_delimiter(p, pieces, next)) {
      return false;
    }
    if (!cp_parse_coord_sys(p, pieces, next, bind, false)) {
      return false;
    }
    if (!cp_parse_eol(p, pieces, next)) {
      return false;
    }
    cp_add_mat_spec(bind);
    return true;
  }

  if (pieces[0] == "texmat") {
    if ((!cp_errchk_parameter_words(p,2))||
        (!cp_errchk_parameter_float(p,16,16))) {
      return false;
    }
    ShaderMatSpec bind;
    bind._id = p._id;
    bind._piece = SMP_whole;
    bind._func = SMF_first;
    bind._part[0] = SMO_texmat_i;
    bind._arg[0] = nullptr;
    bind._part[1] = SMO_identity;
    bind._arg[1] = nullptr;
    bind._index = atoi(pieces[1].c_str());

    cp_add_mat_spec(bind);
    return true;
  }

  if (pieces[0] == "texscale") {
    if ((!cp_errchk_parameter_words(p,2))||
        (!cp_errchk_parameter_float(p,3,4))) {
      return false;
    }
    ShaderMatSpec bind;
    bind._id = p._id;
    bind._piece = SMP_row3;
    bind._func = SMF_first;
    bind._part[0] = SMO_texscale_i;
    bind._arg[0] = nullptr;
    bind._part[1] = SMO_identity;
    bind._arg[1] = nullptr;
    bind._index = atoi(pieces[1].c_str());

    cp_add_mat_spec(bind);
    return true;
  }

  if (pieces[0] == "texcolor") {
    if ((!cp_errchk_parameter_words(p,2))||
        (!cp_errchk_parameter_float(p,3,4))) {
      return false;
    }
    ShaderMatSpec bind;
    bind._id = p._id;
    bind._piece = SMP_row3;
    bind._func = SMF_first;
    bind._part[0] = SMO_texcolor_i;
    bind._arg[0] = nullptr;
    bind._part[1] = SMO_identity;
    bind._arg[1] = nullptr;
    bind._index = atoi(pieces[1].c_str());

    cp_add_mat_spec(bind);
    return true;
  }

  if (pieces[0] == "plane") {
    if ((!cp_errchk_parameter_words(p,2))||
        (!cp_errchk_parameter_float(p,4,4))) {
      return false;
    }
    ShaderMatSpec bind;
    bind._id = p._id;
    bind._piece = SMP_row3;
    bind._func = SMF_first;
    bind._part[0] = SMO_plane_x;
    bind._arg[0] = InternalName::make(pieces[1]);
    bind._part[1] = SMO_identity;
    bind._arg[1] = nullptr;

    cp_add_mat_spec(bind);
    return true;
  }

  if (pieces[0] == "clipplane") {
    if ((!cp_errchk_parameter_words(p,2))||
        (!cp_errchk_parameter_float(p,4,4))) {
      return false;
    }
    ShaderMatSpec bind;
    bind._id = p._id;
    bind._piece = SMP_row3;
    bind._func = SMF_first;
    bind._part[0] = SMO_clipplane_x;
    bind._arg[0] = InternalName::make(pieces[1]);
    bind._part[1] = SMO_identity;
    bind._arg[1] = nullptr;

    cp_add_mat_spec(bind);
    return true;
  }

  // Keywords to access unusual parameters.

  if (pieces[0] == "sys") {
    if (!cp_errchk_parameter_words(p, 2)) {
      return false;
    }
    ShaderMatSpec bind;
    bind._id = p._id;
    bind._piece = SMP_row3;
    bind._func = SMF_first;
    bind._part[1] = SMO_identity;
    bind._arg[1] = nullptr;
    if (pieces[1] == "pixelsize") {
      if (!cp_errchk_parameter_float(p, 2, 2)) {
        return false;
      }
      bind._part[0] = SMO_pixel_size;
      bind._arg[0] = nullptr;

    } else if (pieces[1] == "windowsize") {
      if (!cp_errchk_parameter_float(p, 2, 2)) {
        return false;
      }
      bind._part[0] = SMO_window_size;
      bind._arg[0] = nullptr;

    } else if (pieces[1] == "time") {
      if (!cp_errchk_parameter_float(p, 1, 1)) {
        return false;
      }
      bind._piece = SMP_row3x1;
      bind._part[0] = SMO_frame_time;
      bind._arg[0] = nullptr;

    } else {
      cp_report_error(p, "unknown system parameter");
      return false;
    }

    cp_add_mat_spec(bind);
    return true;
  }

  // Keywords to access textures.

  if (pieces[0] == "tex") {
    if (pieces.size() != 2 && pieces.size() != 3) {
      cp_report_error(p, "Invalid parameter name");
      return false;
    }
    ShaderTexSpec bind;
    bind._id = p._id;
    bind._name = nullptr;
    bind._stage = atoi(pieces[1].c_str());
    bind._part = STO_stage_i;

    if (const ::ShaderType::SampledImage *image = p._type->as_sampled_image()) {
      bind._desired_type = image->get_texture_type();
    }
    else {
      cp_report_error(p, "Invalid type for a tex-parameter");
      return false;
    }
    if (pieces.size() == 3) {
      bind._suffix = InternalName::make(((string)"-") + pieces[2]);
      shader_cat.warning()
        << "Parameter " << p._id._name << ": use of a texture suffix is deprecated.\n";
    }
    _tex_spec.push_back(bind);
    return true;
  }

  if (pieces[0] == "shadow") {
    if (pieces.size() != 2) {
      cp_report_error(p, "Invalid parameter name");
      return false;
    }
    ShaderTexSpec bind;
    bind._id = p._id;
    bind._name = nullptr;
    bind._stage = atoi(pieces[1].c_str());
    bind._part = STO_light_i_shadow_map;

    if (const ::ShaderType::SampledImage *image = p._type->as_sampled_image()) {
      bind._desired_type = image->get_texture_type();
    }
    else {
      cp_report_error(p, "Invalid type for a shadow-parameter");
      return false;
    }
    _tex_spec.push_back(bind);
    return true;
  }

  // Keywords to fetch texture parameter data.

  if (pieces[0] == "texpad") {
    if ((!cp_errchk_parameter_words(p,2)) ||
        (!cp_errchk_parameter_float(p,3,4))) {
      return false;
    }
    ShaderMatSpec bind;
    bind._id = p._id;
    bind._piece = SMP_row3;
    bind._func = SMF_first;
    bind._part[0] = SMO_texpad_x;
    bind._arg[0] = InternalName::make(pieces[1]);
    bind._part[1] = SMO_identity;
    bind._arg[1] = nullptr;
    cp_add_mat_spec(bind);
    return true;
  }

  if (pieces[0] == "texpix") {
    if ((!cp_errchk_parameter_words(p,2)) ||
        (!cp_errchk_parameter_float(p,2,4))) {
      return false;
    }
    ShaderMatSpec bind;
    bind._id = p._id;
    bind._piece = SMP_row3;
    bind._func = SMF_first;
    bind._part[0] = SMO_texpix_x;
    bind._arg[0] = InternalName::make(pieces[1]);
    bind._part[1] = SMO_identity;
    bind._arg[1] = nullptr;
    cp_add_mat_spec(bind);
    return true;
  }

  if (pieces[0] == "tbl") {
    // Handled elsewhere.
    return true;
  }

  // Fetch uniform parameters without prefix
  bool k_prefix = false;

  // solve backward compatibility issue
  if (pieces[0] == "k") {
    k_prefix = true;
    basename = basename.substr(2);
  }

  PT(InternalName) kinputname = InternalName::make(struct_name + basename);

  if (const ::ShaderType::SampledImage *image = p._type->as_sampled_image()) {
    ShaderTexSpec bind;
    bind._id = p._id;
    bind._name = kinputname;
    bind._part = STO_named_input;
    bind._desired_type = Texture::TT_cube_map_array;
    _tex_spec.push_back(bind);
    return true;
  }

  int arg_dim[3] = {1, 1, 1};
  const ::ShaderType *base_type = p._type;
  if (const ::ShaderType::Array *array = base_type->as_array()) {
    arg_dim[0] = array->get_num_elements();
    base_type = array->get_element_type();
  }
  if (const ::ShaderType::Matrix *matrix = base_type->as_matrix()) {
    arg_dim[1] = matrix->get_num_rows();
    arg_dim[2] = matrix->get_num_columns();
    base_type = matrix->get_base_type();
  }
  else if (const ::ShaderType::Vector *vector = base_type->as_vector()) {
    arg_dim[2] = vector->get_num_elements();
    base_type = matrix->get_base_type();
  }

  if (base_type->as_scalar() != nullptr) {
    ShaderPtrSpec bind;
    bind._id = p._id;
    bind._arg = kinputname;
    bind._info = p;

    // We specify SSD_frame because a PTA may be modified by the app from
    // frame to frame, and we have no way to know.  So, we must respecify a
    // PTA at least once every frame.
    bind._dep[0]  = SSD_general | SSD_shaderinputs | SSD_frame;
    bind._dep[1]  = SSD_NONE;

    memcpy(bind._dim, arg_dim, sizeof(int)*3);

    // if dim[0] = -1,  glShaderContext will not check the param size
    if (k_prefix) bind._dim[0] = -1;
    _ptr_spec.push_back(bind);
    return true;
  }

  cp_report_error(p, "unrecognized parameter name");
  return false;
}

/**
 *
 */
void Shader::
clear_parameters() {
  _mat_spec.clear();
  _var_spec.clear();
  _tex_spec.clear();
}

/**
 * Called by the back-end when the shader has compiled data available.
 */
void Shader::
set_compiled(unsigned int format, const char *data, size_t length) {
  _compiled_format = format;
  _compiled_binary.assign(data, length);

  // Store the compiled shader in the cache.
  /*if (_cache_compiled_shader && !_record.is_null()) {
    _record->set_data(this);

    BamCache *cache = BamCache::get_global_ptr();
    cache->store(_record);
  }*/
}

/**
 * Called by the back-end to retrieve compiled data.
 */
bool Shader::
get_compiled(unsigned int &format, string &binary) const {
  format = _compiled_format;
  binary = _compiled_binary;
  return !binary.empty();
}

/**
 * Called by the graphics back-end to specify the caps with which we will
 * likely want to be compiling our shaders.
 */
void Shader::
set_default_caps(const ShaderCaps &caps) {
  _default_caps = caps;
}

#ifdef HAVE_CG
/**
 *
 */
const ::ShaderType *Shader::
cg_scalar_type(int type) {
  switch (type) {
  case CG_UINT:
  case CG_ULONG:
  case CG_USHORT:
  case CG_UCHAR:
    return ::ShaderType::uint_type;

  case CG_BOOL:
    return ::ShaderType::bool_type;

  case CG_INT:
  case CG_LONG:
  case CG_SHORT:
  case CG_CHAR:
    return ::ShaderType::int_type;

  default:
    return ::ShaderType::float_type;
  }
  return nullptr;
}

/**
 *
 */
const ::ShaderType *Shader::
cg_parameter_type(CGparameter p) {
  switch (cgGetParameterClass(p)) {
  case CG_PARAMETERCLASS_SCALAR:
    return cg_scalar_type(cgGetParameterType(p));

  case CG_PARAMETERCLASS_VECTOR:
    return ::ShaderType::register_type(::ShaderType::Vector(
      cg_scalar_type(cgGetParameterBaseType(p)),
      cgGetParameterColumns(p)));

  case CG_PARAMETERCLASS_MATRIX:
    return ::ShaderType::register_type(::ShaderType::Matrix(
      cg_scalar_type(cgGetParameterBaseType(p)),
      cgGetParameterRows(p),
      cgGetParameterColumns(p)));

  case CG_PARAMETERCLASS_STRUCT:
    {
      ::ShaderType::Struct type;
      CGparameter member = cgGetFirstStructParameter(p);
      while (member) {
        type.add_member(
          cg_parameter_type(member),
          InternalName::make(cgGetParameterName(p))
        );
        member = cgGetNextParameter(member);
      }
      return ::ShaderType::register_type(std::move(type));
    }

  case CG_PARAMETERCLASS_ARRAY:
    return ::ShaderType::register_type(::ShaderType::Array(
      cg_parameter_type(cgGetArrayParameter(p, 0)),
      cgGetArraySize(p, 0)));

  case CG_PARAMETERCLASS_SAMPLER:
    {
      Texture::TextureType texture_type;
      switch (cgGetParameterType(p)) {
      case CG_SAMPLER1D:
      case 1313: // CG_SAMPLER1DSHADOW
        texture_type = Texture::TT_1d_texture;
        break;
      case CG_SAMPLER2D:
      case 1314: // CG_SAMPLER2DSHADOW
        texture_type = Texture::TT_2d_texture;
        break;
      case CG_SAMPLER3D:
        texture_type = Texture::TT_3d_texture;
        break;
      case CG_SAMPLER2DARRAY:
        texture_type = Texture::TT_2d_texture_array;
        break;
      case CG_SAMPLERCUBE:
        texture_type = Texture::TT_cube_map;
        break;
      case CG_SAMPLERBUF:
        texture_type = Texture::TT_buffer_texture;
        break;
      case CG_SAMPLERCUBEARRAY:
        texture_type = Texture::TT_cube_map_array;
        break;
      default:
        return nullptr;
      }
      return ::ShaderType::register_type(::ShaderType::SampledImage(texture_type));
    }

  default:
    break;
  }

  return nullptr;
}

/**
 * xyz
 */
void Shader::
cg_release_resources() {
  if (_cg_vprogram != 0) {
    cgDestroyProgram(_cg_vprogram);
    _cg_vprogram = 0;
  }
  if (_cg_fprogram != 0) {
    cgDestroyProgram(_cg_fprogram);
    _cg_fprogram = 0;
  }
  if (_cg_gprogram != 0) {
    cgDestroyProgram(_cg_gprogram);
    _cg_gprogram = 0;
  }
}

/**
 * xyz
 */
CGprogram Shader::
cg_compile_entry_point(const char *entry, const ShaderCaps &caps,
                       CGcontext context, ShaderType type) {
  CGprogram prog;
  CGerror err;
  const char *compiler_args[100];
  const string text = get_text(type);
  int nargs = 0;

  int active, ultimate;

  switch (type) {
  case ST_vertex:
    active   = caps._active_vprofile;
    ultimate = caps._ultimate_vprofile;
    break;

  case ST_fragment:
    active   = caps._active_fprofile;
    ultimate = caps._ultimate_fprofile;
    break;

  case ST_geometry:
    active   = caps._active_gprofile;
    ultimate = caps._ultimate_gprofile;
    break;

  case ST_tess_evaluation:
  case ST_tess_control:
    active   = caps._active_tprofile;
    ultimate = caps._ultimate_tprofile;
    break;

  case ST_none:
  default:
    active   = CG_PROFILE_UNKNOWN;
    ultimate = CG_PROFILE_UNKNOWN;
  };

  if (type == ST_fragment && caps._bug_list.count(SBUG_ati_draw_buffers)) {
    compiler_args[nargs++] = "-po";
    compiler_args[nargs++] = "ATI_draw_buffers";
  }

  string version_arg;
  if (!cg_glsl_version.empty() && active != CG_PROFILE_UNKNOWN &&
      cgGetProfileProperty((CGprofile) active, CG_IS_GLSL_PROFILE)) {

    version_arg = "version=";
    version_arg += cg_glsl_version;

    compiler_args[nargs++] = "-po";
    compiler_args[nargs++] = version_arg.c_str();
  }

  compiler_args[nargs] = 0;

  cgGetError();

  if ((active != (int)CG_PROFILE_UNKNOWN) && (active != ultimate)) {
    // Print out some debug information about what we're doing.
    if (shader_cat.is_debug()) {
      shader_cat.debug()
        << "Compiling Cg shader " << get_filename(type) << " with entry point " << entry
        << " and active profile " << cgGetProfileString((CGprofile) active) << "\n";

      if (nargs > 0) {
        shader_cat.debug() << "Using compiler arguments:";
        for (int i = 0; i < nargs; ++i) {
          shader_cat.debug(false) << " " << compiler_args[i];
        }
        shader_cat.debug(false) << "\n";
      }
    }

    // Compile the shader with the active profile.
    prog = cgCreateProgram(context, CG_SOURCE, text.c_str(),
                           (CGprofile)active, entry, (const char **)compiler_args);
    err = cgGetError();
    if (err == CG_NO_ERROR) {
      return prog;
    }
    if (prog != 0) {
      cgDestroyProgram(prog);
    }
    if (shader_cat.is_debug()) {
      shader_cat.debug()
        << "Compilation with active profile failed: " << cgGetErrorString(err) << "\n";
      if (err == CG_COMPILER_ERROR) {
        const char *listing = cgGetLastListing(context);
        if (listing != nullptr) {
          shader_cat.debug(false) << listing;
        }
      }
    }
  }

  if (shader_cat.is_debug()) {
    shader_cat.debug()
      << "Compiling Cg shader " << get_filename(type) << " with entry point " << entry
      << " and ultimate profile " << cgGetProfileString((CGprofile) ultimate) << "\n";
  }

  // The active profile failed, so recompile it with the ultimate profile.
  prog = cgCreateProgram(context, CG_SOURCE, text.c_str(),
                         (CGprofile)ultimate, entry, nullptr);

  // Extract the output listing.
  err = cgGetError();
  const char *listing = cgGetLastListing(context);

  if (err == CG_NO_ERROR && listing != nullptr && strlen(listing) > 1) {
    shader_cat.warning()
      << "Encountered warnings during compilation of " << get_filename(type)
      << ":\n" << listing;

  } else if (err == CG_COMPILER_ERROR) {
    shader_cat.error()
      << "Failed to compile Cg shader " << get_filename(type);
    if (listing != nullptr) {
      shader_cat.error(false) << ":\n" << listing;
    } else {
      shader_cat.error(false) << "!\n";
    }
  }

  if (err == CG_NO_ERROR) {
    return prog;
  }

  if (shader_cat.is_debug()) {
    shader_cat.debug()
      << "Compilation with ultimate profile failed: " << cgGetErrorString(err) << "\n";
  }

  if (prog != 0) {
    cgDestroyProgram(prog);
  }
  return 0;
}

/**
 * Compiles a Cg shader for a given set of capabilities.  If successful, the
 * shader is stored in the instance variables _cg_context, _cg_vprogram,
 * _cg_fprogram.
 */
bool Shader::
cg_compile_shader(const ShaderCaps &caps, CGcontext context) {
  _cg_last_caps = caps;

  if (!_text._separate || !_text._vertex.empty()) {
    _cg_vprogram = cg_compile_entry_point("vshader", caps, context, ST_vertex);
    if (_cg_vprogram == 0) {
      cg_release_resources();
      return false;
    }
    _cg_vprofile = cgGetProgramProfile(_cg_vprogram);
  }

  if (!_text._separate || !_text._fragment.empty()) {
    _cg_fprogram = cg_compile_entry_point("fshader", caps, context, ST_fragment);
    if (_cg_fprogram == 0) {
      cg_release_resources();
      return false;
    }
    _cg_fprofile = cgGetProgramProfile(_cg_fprogram);
  }

  if ((_text._separate && !_text._geometry.empty()) || (!_text._separate && _text._shared.find("gshader") != string::npos)) {
    _cg_gprogram = cg_compile_entry_point("gshader", caps, context, ST_geometry);
    if (_cg_gprogram == 0) {
      cg_release_resources();
      return false;
    }
    _cg_gprofile = cgGetProgramProfile(_cg_gprogram);
  }

  if (_cg_vprogram == 0 && _cg_fprogram == 0 && _cg_gprogram == 0) {
    shader_cat.error() << "Shader must at least have one program!\n";
    cg_release_resources();
    return false;
  }

  // This is present to work around a bug in the Cg compiler for Direct3D 9.
  // It generates "texld_sat" instructions that the result in an
  // D3DXERR_INVALIDDATA error when trying to load the shader, since the _sat
  // modifier may not be used on tex* instructions.
  if (_cg_fprofile == CG_PROFILE_PS_2_0 ||
      _cg_fprofile == CG_PROFILE_PS_2_X ||
      _cg_fprofile == CG_PROFILE_PS_3_0) {
    vector_string lines;
    tokenize(cgGetProgramString(_cg_fprogram, CG_COMPILED_PROGRAM), lines, "\n");

    ostringstream out;
    int num_modified = 0;

    for (size_t i = 0; i < lines.size(); ++i) {
      const string &line = lines[i];

      size_t space = line.find(' ');
      if (space == string::npos) {
        out << line << '\n';
        continue;
      }

      string instr = line.substr(0, space);

      // Look for a texld instruction with _sat modifier.
      if (instr.compare(0, 5, "texld") == 0 &&
          instr.compare(instr.size() - 4, 4, "_sat") == 0) {
        // Which destination register are we operating on?
        string reg = line.substr(space + 1, line.find(',', space) - space - 1);

        // Move the saturation operation to a separate instruction.
        instr.resize(instr.size() - 4);
        out << instr << ' ' << line.substr(space + 1) << '\n';
        out << "mov_sat " << reg << ", " << reg << '\n';
        ++num_modified;
      } else {
        out << line << '\n';
      }
    }

    if (num_modified > 0) {
      string result = out.str();
      CGprogram new_program;
      new_program = cgCreateProgram(context, CG_OBJECT, result.c_str(),
                                    (CGprofile)_cg_fprofile, "fshader",
                                    nullptr);
      if (new_program) {
        cgDestroyProgram(_cg_fprogram);
        _cg_fprogram = new_program;

        if (shader_cat.is_debug()) {
          shader_cat.debug()
            << "Replaced " << num_modified << " invalid texld_sat instruction"
            << ((num_modified == 1) ? "" : "s") << " in compiled shader\n";
        }
      } else {
        shader_cat.warning()
          << "Failed to load shader with fixed texld_sat instructions: "
          << cgGetErrorString(cgGetError()) << "\n";
      }
    }
  }

  // DEBUG: output the generated program
  if (shader_cat.is_debug()) {
    const char *vertex_program;
    const char *fragment_program;
    const char *geometry_program;

    if (_cg_vprogram != 0) {
      shader_cat.debug()
        << "Cg vertex profile: " << cgGetProfileString((CGprofile)_cg_vprofile) << "\n";
      vertex_program = cgGetProgramString(_cg_vprogram, CG_COMPILED_PROGRAM);
      shader_cat.spam() << vertex_program << "\n";
    }
    if (_cg_fprogram != 0) {
      shader_cat.debug()
        << "Cg fragment profile: " << cgGetProfileString((CGprofile)_cg_fprofile) << "\n";
      fragment_program = cgGetProgramString(_cg_fprogram, CG_COMPILED_PROGRAM);
      shader_cat.spam() << fragment_program << "\n";
    }
    if (_cg_gprogram != 0) {
      shader_cat.debug()
        << "Cg geometry profile: " << cgGetProfileString((CGprofile)_cg_gprofile) << "\n";
      geometry_program = cgGetProgramString(_cg_gprogram, CG_COMPILED_PROGRAM);
      shader_cat.spam() << geometry_program << "\n";
    }
  }

  return true;
}

/**
 *
 */
bool Shader::
cg_analyze_entry_point(CGprogram prog, ShaderType type) {
  bool success = true;

  CGparameter parameter = cgGetFirstParameter(prog, CG_PROGRAM);
  while (parameter) {
    if (cgIsParameterReferenced(parameter)) {
      const ::ShaderType *arg_type = cg_parameter_type(parameter);

      CGenum vbl = cgGetParameterVariability(parameter);
      CGtype base_type = cgGetParameterBaseType(parameter);

      if (cgGetParameterDirection(parameter) == CG_IN) {
        CPT(InternalName) name = InternalName::make(cgGetParameterName(parameter));

        if (vbl == CG_VARYING && type == ST_vertex) {
          success &= bind_vertex_input(name, arg_type, -1);
        }
        else if (vbl == CG_UNIFORM) {
          ShaderArgInfo p;
          p._id._name   = cgGetParameterName(parameter);
          p._id._type   = type;
          p._id._seqno  = -1;
          p._type       = arg_type;

          //NB. Cg does have a CG_DOUBLE type, but at least for the ARB
          // profiles and GLSL profiles it just maps to float.
          switch (base_type) {
          case CG_UINT:
          case CG_ULONG:
          case CG_USHORT:
          case CG_UCHAR:
          case CG_BOOL:
            p._numeric_type = SPT_uint;
            break;
          case CG_INT:
          case CG_LONG:
          case CG_SHORT:
          case CG_CHAR:
            p._numeric_type = SPT_int;
            break;
          default:
            p._numeric_type = SPT_float;
            break;
          }
          success &= compile_parameter(p);
        }
      }
    } else if (shader_cat.is_debug()) {
      shader_cat.debug()
        << "Parameter " << cgGetParameterName(parameter)
        << " is unreferenced within shader " << get_filename(type) << "\n";
    }

    parameter = cgGetNextParameter(parameter);
  }

  return success;
}

/**
 * This subroutine analyzes the parameters of a Cg shader.  The output is
 * stored in instance variables: _mat_spec, _var_spec, and _tex_spec.
 *
 * In order to do this, it is necessary to compile the shader.  It would be a
 * waste of CPU time to compile the shader, analyze the parameters, and then
 * discard the compiled shader.  This would force us to compile it again
 * later, when we need to build the ShaderContext.  Instead, we cache the
 * compiled Cg program in instance variables.  Later, a ShaderContext can pull
 * the compiled shader from these instance vars.
 *
 * To compile a shader, you need to first choose a profile.  There are two
 * contradictory objectives:
 *
 * 1. If you don't use the gsg's active profile, then the cached compiled
 * shader will not be useful to the ShaderContext.
 *
 * 2. If you use too weak a profile, then the shader may not compile.  So to
 * guarantee success, you should use the ultimate profile.
 *
 * To resolve this conflict, we try the active profile first, and if that
 * doesn't work, we try the ultimate profile.
 *
 */
bool Shader::
cg_analyze_shader(const ShaderCaps &caps) {

  // Make sure we have a context for analyzing the shader.
  if (_cg_context == 0) {
    _cg_context = cgCreateContext();
    if (_cg_context == 0) {
      shader_cat.error()
        << "Could not create a Cg context object: "
        << cgGetErrorString(cgGetError()) << "\n";
      return false;
    }
  }

  if (!cg_compile_shader(caps, _cg_context)) {
    return false;
  }

  if (_cg_fprogram != 0) {
     if (!cg_analyze_entry_point(_cg_fprogram, ST_fragment)) {
      cg_release_resources();
      clear_parameters();
      return false;
    }
  }

  if (_var_spec.size() != 0) {
    shader_cat.error() << "Cannot use vtx parameters in an fshader\n";
    cg_release_resources();
    clear_parameters();
    return false;
  }

  if (_cg_vprogram != 0) {
    if (!cg_analyze_entry_point(_cg_vprogram, ST_vertex)) {
      cg_release_resources();
      clear_parameters();
      return false;
    }
  }

  if (_cg_gprogram != 0) {
    if (!cg_analyze_entry_point(_cg_gprogram, ST_geometry)) {
      cg_release_resources();
      clear_parameters();
      return false;
    }
  }

  // Assign sequence numbers to all parameters.  GLCgShaderContext relies on
  // the fact that the varyings start at seqno 0.
  int seqno = 0;
  for (size_t i = 0; i < _var_spec.size(); ++i) {
    _var_spec[i]._id._seqno = seqno++;
  }
  for (size_t i = 0; i < _mat_spec.size(); ++i) {
    _mat_spec[i]._id._seqno = seqno++;
  }
  for (size_t i = 0; i < _tex_spec.size(); ++i) {
    _tex_spec[i]._id._seqno = seqno++;
  }

  for (size_t i = 0; i < _ptr_spec.size(); ++i) {
    _ptr_spec[i]._id._seqno = seqno++;
    _ptr_spec[i]._info._id = _ptr_spec[i]._id;
  }

  /*
  // The following code is present to work around a bug in the Cg compiler.
  // It does not generate correct code for shadow map lookups when using arbfp1.
  // This is a particularly onerous limitation, given that arbfp1 is the only
  // Cg target that works on radeons.  I suspect this is an intentional
  // omission on nvidia's part.  The following code fetches the output listing,
  // detects the error, repairs the code, and resumbits the repaired code to Cg.
  if ((_cg_fprofile == CG_PROFILE_ARBFP1) && (gsghint->_supports_shadow_filter)) {
    bool shadowunit[32];
    bool anyshadow = false;
    memset(shadowunit, 0, sizeof(shadowunit));
    vector_string lines;
    tokenize(cgGetProgramString(_cg_program[SHADER_type_frag],
                                CG_COMPILED_PROGRAM), lines, "\n");
    // figure out which texture units contain shadow maps.
    for (int lineno=0; lineno<(int)lines.size(); lineno++) {
      if (lines[lineno].compare(0,21,"#var sampler2DSHADOW ")) {
        continue;
      }
      vector_string fields;
      tokenize(lines[lineno], fields, ":");
      if (fields.size()!=5) {
        continue;
      }
      vector_string words;
      tokenize(trim(fields[2]), words, " ");
      if (words.size()!=2) {
        continue;
      }
      int unit = atoi(words[1].c_str());
      if ((unit < 0)||(unit >= 32)) {
        continue;
      }
      anyshadow = true;
      shadowunit[unit] = true;
    }
    // modify all TEX statements that use the relevant texture units.
    if (anyshadow) {
      for (int lineno=0; lineno<(int)lines.size(); lineno++) {
        if (lines[lineno].compare(0,4,"TEX ")) {
          continue;
        }
        vector_string fields;
        tokenize(lines[lineno], fields, ",");
        if ((fields.size()!=4)||(trim(fields[3]) != "2D;")) {
          continue;
        }
        vector_string texunitf;
        tokenize(trim(fields[2]), texunitf, "[]");
        if ((texunitf.size()!=3)||(texunitf[0] != "texture")||(texunitf[2]!="")) {
          continue;
        }
        int unit = atoi(texunitf[1].c_str());
        if ((unit < 0) || (unit >= 32) || (shadowunit[unit]==false)) {
          continue;
        }
        lines[lineno] = fields[0]+","+fields[1]+","+fields[2]+", SHADOW2D;";
      }
      string result = "!!ARBfp1.0\nOPTION ARB_fragment_program_shadow;\n";
      for (int lineno=1; lineno<(int)lines.size(); lineno++) {
        result += (lines[lineno] + "\n");
      }
      _cg_program[2] = _cg_program[SHADER_type_frag];
      _cg_program[SHADER_type_frag] =
        cgCreateProgram(_cg_context, CG_OBJECT, result.c_str(),
                        _cg_profile[SHADER_type_frag], "fshader", (const char**)NULL);
      cg_report_errors(s->get_name(), _cg_context);
      if (_cg_program[SHADER_type_frag]==0) {
        release_resources();
        return false;
      }
    }
  }
  */

  cg_release_resources();
  return true;
}

/**
 * This routine is used by the ShaderContext constructor to compile the
 * shader.  The CGprogram objects are turned over to the ShaderContext, we no
 * longer own them.
 */
bool Shader::
cg_compile_for(const ShaderCaps &caps, CGcontext context,
               CGprogram &combined_program, pvector<CGparameter> &map) {

  // Initialize the return values to empty.
  combined_program = 0;
  map.clear();

  // Make sure the shader is compiled for the target caps.  Most of the time,
  // it will already be - this is usually a no-op.

  _default_caps = caps;
  if (!cg_compile_shader(caps, context)) {
    return false;
  }

  // If the compile routine used the ultimate profile instead of the active
  // one, it means the active one isn't powerful enough to compile the shader.
  if (_cg_vprogram != 0 && _cg_vprofile != caps._active_vprofile) {
    shader_cat.error() << "Cg vertex program not supported by profile "
      << cgGetProfileString((CGprofile) caps._active_vprofile) << ": "
      << get_filename(ST_vertex) << ". Try choosing a different profile.\n";
    return false;
  }
  if (_cg_fprogram != 0 && _cg_fprofile != caps._active_fprofile) {
    shader_cat.error() << "Cg fragment program not supported by profile "
      << cgGetProfileString((CGprofile) caps._active_fprofile) << ": "
      << get_filename(ST_fragment) << ". Try choosing a different profile.\n";
    return false;
  }
  if (_cg_gprogram != 0 && _cg_gprofile != caps._active_gprofile) {
    shader_cat.error() << "Cg geometry program not supported by profile "
      << cgGetProfileString((CGprofile) caps._active_gprofile) << ": "
      << get_filename(ST_geometry) << ". Try choosing a different profile.\n";
    return false;
  }

  // Gather the programs we will be combining.
  pvector<CGprogram> programs;
  if (_cg_vprogram != 0) {
    programs.push_back(_cg_vprogram);
  }
  if (_cg_fprogram != 0) {
    programs.push_back(_cg_fprogram);
  }
  if (_cg_gprogram != 0) {
    programs.push_back(_cg_gprogram);
  }

  // Combine the programs.  This can be more optimal than loading them
  // individually, and it is even necessary for some profiles (particularly
  // GLSL profiles on non-NVIDIA GPUs).
  combined_program = cgCombinePrograms(programs.size(), &programs[0]);

  // Build a parameter map.
  size_t n_mat = _mat_spec.size();
  size_t n_tex = _tex_spec.size();
  size_t n_var = _var_spec.size();
  size_t n_ptr = _ptr_spec.size();

  map.resize(n_mat + n_tex + n_var + n_ptr);

  // This is a bit awkward, we have to go in and seperate out the combined
  // program, since all the parameter bindings have changed.
  CGprogram programs_by_type[ST_COUNT];
  for (int i = 0; i < cgGetNumProgramDomains(combined_program); ++i) {
    // Conveniently, the CGdomain enum overlaps with ShaderType.
    CGprogram program = cgGetProgramDomainProgram(combined_program, i);
    programs_by_type[cgGetProgramDomain(program)] = program;
  }

  for (size_t i = 0; i < n_mat; ++i) {
    const ShaderArgId &id = _mat_spec[i]._id;
    map[id._seqno] = cgGetNamedParameter(programs_by_type[id._type], id._name.c_str());

    if (shader_cat.is_debug()) {
      const char *resource = cgGetParameterResourceName(map[id._seqno]);
      if (resource != nullptr) {
        shader_cat.debug() << "Uniform parameter " << id._name
                           << " is bound to resource " << resource << "\n";
      }
    }
  }

  for (size_t i = 0; i < n_tex; ++i) {
    const ShaderArgId &id = _tex_spec[i]._id;
    CGparameter p = cgGetNamedParameter(programs_by_type[id._type], id._name.c_str());

    if (shader_cat.is_debug()) {
      const char *resource = cgGetParameterResourceName(p);
      if (resource != nullptr) {
        shader_cat.debug() << "Texture parameter " << id._name
                          << " is bound to resource " << resource << "\n";
      }
    }
    map[id._seqno] = p;
  }

  for (size_t i = 0; i < n_var; ++i) {
    const ShaderArgId &id = _var_spec[i]._id;
    CGparameter p = cgGetNamedParameter(programs_by_type[id._type], id._name.c_str());

    const char *resource = cgGetParameterResourceName(p);
    if (shader_cat.is_debug() && resource != nullptr) {
      if (cgGetParameterResource(p) == CG_GLSL_ATTRIB) {
        shader_cat.debug()
          << "Varying parameter " << id._name << " is bound to GLSL attribute "
          << resource << "\n";
      } else {
        shader_cat.debug()
          << "Varying parameter " << id._name << " is bound to resource "
          << resource << " (" << cgGetParameterResource(p)
          << ", index " << cgGetParameterResourceIndex(p) << ")\n";
      }
    }

    map[id._seqno] = p;
  }

  for (size_t i = 0; i < n_ptr; ++i) {
    const ShaderArgId &id = _ptr_spec[i]._id;
    map[id._seqno] = cgGetNamedParameter(programs_by_type[id._type], id._name.c_str());

    if (shader_cat.is_debug()) {
      const char *resource = cgGetParameterResourceName(map[id._seqno]);
      if (resource != nullptr) {
        shader_cat.debug() << "Uniform ptr parameter " << id._name
                           << " is bound to resource " << resource << "\n";
      }
    }
  }

  // Transfer ownership of the compiled shader.
  if (_cg_vprogram != 0) {
    cgDestroyProgram(_cg_vprogram);
    _cg_vprogram = 0;
  }
  if (_cg_fprogram != 0) {
    cgDestroyProgram(_cg_fprogram);
    _cg_fprogram = 0;
  }
  if (_cg_gprogram != 0) {
    cgDestroyProgram(_cg_gprogram);
    _cg_gprogram = 0;
  }

  _cg_last_caps.clear();

  return true;
}
#endif  // HAVE_CG

/**
 * Construct a Shader that will be filled in using fillin() or read() later.
 */
Shader::
Shader(ShaderLanguage lang) :
  _error_flag(false),
  _parse(0),
  _loaded(false),
  _language(lang),
  _mat_deps(0),
  _cache_compiled_shader(false)
{
#ifdef HAVE_CG
  _cg_vprogram = 0;
  _cg_fprogram = 0;
  _cg_gprogram = 0;
  _cg_vprofile = CG_PROFILE_UNKNOWN;
  _cg_fprofile = CG_PROFILE_UNKNOWN;
  _cg_gprofile = CG_PROFILE_UNKNOWN;
  if (_default_caps._ultimate_vprofile == 0 || _default_caps._ultimate_vprofile == CG_PROFILE_UNKNOWN) {
    if (basic_shaders_only) {
      _default_caps._active_vprofile = CG_PROFILE_ARBVP1;
      _default_caps._active_fprofile = CG_PROFILE_ARBFP1;
      _default_caps._active_gprofile = CG_PROFILE_UNKNOWN;
    } else {
      _default_caps._active_vprofile = CG_PROFILE_UNKNOWN;
      _default_caps._active_fprofile = CG_PROFILE_UNKNOWN;
      _default_caps._active_gprofile = CG_PROFILE_UNKNOWN;
    }
    _default_caps._ultimate_vprofile = cgGetProfile("glslv");
    _default_caps._ultimate_fprofile = cgGetProfile("glslf");
    _default_caps._ultimate_gprofile = cgGetProfile("glslg");
    if (_default_caps._ultimate_gprofile == CG_PROFILE_UNKNOWN) {
      _default_caps._ultimate_gprofile = cgGetProfile("gp4gp");
    }
  }
#endif
}

/**
 * Reads the shader from the given filename(s). Returns a boolean indicating
 * success or failure.
 */
bool Shader::
read(const ShaderFile &sfile, BamCacheRecord *record) {
  _text._separate = sfile._separate;

  if (sfile._separate) {
    if (_language == SL_none) {
      shader_cat.error()
        << "No shader language was specified!\n";
      return false;
    }

    // Read the various stages in order.
    if (!sfile._vertex.empty() &&
        !do_read_source(Stage::vertex, sfile._vertex, record)) {
      return false;
    }
    if (!sfile._fragment.empty() &&
        !do_read_source(Stage::fragment, sfile._fragment, record)) {
      return false;
    }
    if (!sfile._geometry.empty() &&
        !do_read_source(Stage::geometry, sfile._geometry, record)) {
      return false;
    }
    if (!sfile._tess_control.empty() &&
        !do_read_source(Stage::tess_control, sfile._tess_control, record)) {
      return false;
    }
    if (!sfile._tess_evaluation.empty() &&
        !do_read_source(Stage::tess_evaluation, sfile._tess_evaluation, record)) {
      return false;
    }
    if (!sfile._compute.empty() &&
        !do_read_source(Stage::compute, sfile._compute, record)) {
      return false;
    }
    _filename = sfile;

  } else if (_language == SL_Cg || _language == SL_none) {
    // For historical reasons, we have to open up this file early to determine
    // some things about it.
    Filename fn = sfile._shared;
    VirtualFileSystem *vfs = VirtualFileSystem::get_global_ptr();
    PT(VirtualFile) vf = vfs->find_file(fn, get_model_path());
    if (vf == nullptr) {
      shader_cat.error()
        << "Could not find shader file: " << fn << "\n";
      return false;
    }

    ShaderFile sbody;
    sbody._separate = false;
    if (!vf->read_file(sbody._shared, true)) {
      shader_cat.error()
        << "Could not read shader file: " << fn << "\n";
      return false;
    }

    _filename = sfile;
    if (!load(sbody, record)) {
      return false;
    }

    //FIXME
    //for (ShaderModule *module : _modules) {
    //  module->set_source_filename(fn);
    //}

  } else {
    shader_cat.error()
      << "GLSL shaders must have separate shader bodies!\n";
    return false;
  }

  if (!link()) {
    return false;
  }

  _prepare_shader_pcollector = PStatCollector(std::string("Draw:Prepare:Shader:") + _debug_name);

  _loaded = true;
  return true;
}

/**
 * Loads the shader from the given string(s). Returns a boolean indicating
 * success or failure.
 */
bool Shader::
load(const ShaderFile &sbody, BamCacheRecord *record) {
  _filename = ShaderFile("created-shader");
  _fullpath = Filename();
  _text._separate = sbody._separate;

  if (sbody._separate) {
    if (_language == SL_none) {
      shader_cat.error()
        << "No shader language was specified!\n";
      return false;
    }

    if (!sbody._vertex.empty() &&
        !do_load_source(Stage::vertex, sbody._vertex, record)) {
      return false;
    }
    if (!sbody._fragment.empty() &&
        !do_load_source(Stage::fragment, sbody._fragment, record)) {
      return false;
    }
    if (!sbody._geometry.empty() &&
        !do_load_source(Stage::geometry, sbody._geometry, record)) {
      return false;
    }
    if (!sbody._tess_control.empty() &&
        !do_load_source(Stage::tess_control, sbody._tess_control, record)) {
      return false;
    }
    if (!sbody._tess_evaluation.empty() &&
        !do_load_source(Stage::tess_evaluation, sbody._tess_evaluation, record)) {
      return false;
    }
    if (!sbody._compute.empty() &&
        !do_load_source(Stage::compute, sbody._compute, record)) {
      return false;
    }

  } else if (_language == SL_Cg || _language == SL_none) {
    if (_language == SL_none && !has_cg_header(sbody._shared)) {
      shader_cat.error()
        << "Unable to determine shader language of created-shader\n";
      return false;
    }
    _language = SL_Cg;

    if (!do_load_source(Stage::vertex, sbody._shared, record)) {
      return false;
    }
    if (!do_load_source(Stage::fragment, sbody._shared, record)) {
      return false;
    }
    if (sbody._shared.find("gshader") != string::npos &&
        !do_load_source(Stage::geometry, sbody._shared, record)) {
      return false;
    }

  } else {
    shader_cat.error()
      << "GLSL shaders must have separate shader bodies!\n";
    return false;
  }

  if (!link()) {
    shader_cat.error()
      << "Failed to link shader.\n";
    return false;
  }

  _debug_name = "created-shader";
  _prepare_shader_pcollector = PStatCollector("Draw:Prepare:Shader:created-shader");

  _loaded = true;
  return true;
}

/**
 * Reads the shader file from the given path into the given string.
 *
 * Returns false if there was an error with this shader bad enough to consider
 * it 'invalid'.
 */
bool Shader::
do_read_source(Stage stage, const Filename &fn, BamCacheRecord *record) {
  VirtualFileSystem *vfs = VirtualFileSystem::get_global_ptr();
  PT(VirtualFile) vf = vfs->find_file(fn, get_model_path());
  if (vf == nullptr) {
    shader_cat.error()
      << "Could not find shader file: " << fn << "\n";
    return false;
  }

  std::istream *in = vf->open_read_file(true);
  if (in == nullptr) {
    shader_cat.error()
      << "Could not open shader file for reading: " << fn << "\n";
    return false;
  }

  Filename fullpath = vf->get_filename();

  PT(BamCacheRecord) record_pt;
  if (record == nullptr) {
    BamCache *cache = BamCache::get_global_ptr();
    record_pt = cache->lookup(fullpath, "smo");
    record = record_pt.p();
  }

  if (record != nullptr) {
    record->add_dependent_file(vf);
  }

  shader_cat.info() << "Reading shader file: " << fn << "\n";
  if (!do_read_source(stage, *in, fn, record)) {
    vf->close_read_file(in);
    return false;
  }

  //_last_modified = std::max(_last_modified, vf->get_timestamp());
  //_source_files.push_back(fullpath);

  vf->close_read_file(in);

  if (!_debug_name.empty()) {
    _debug_name += '/';
  }
  _debug_name += fullpath.get_basename();

  return true;
}

/**
 * Loads the shader file from the given string into the given string,
 * performing any pre-processing on it that may be necessary.
 *
 * Returns false if there was an error with this shader bad enough to consider
 * it 'invalid'.
 */
bool Shader::
do_read_source(ShaderModule::Stage stage, std::istream &in,
               const Filename &source_filename, BamCacheRecord *record) {
  ShaderCompiler *compiler = get_compiler(_language);
  nassertr(compiler != nullptr, false);

  PT(ShaderModule) module = compiler->compile_now(stage, in, source_filename, record);
  if (!module) {
    return false;
  }

  if (_language == SL_Cg) {
#ifdef HAVE_CG
    ShaderCompilerCg *cg_compiler = DCAST(ShaderCompilerCg, compiler);
    cg_compiler->get_profile_from_header(_text._shared, _default_caps);

    if (!cg_analyze_shader(_default_caps)) {
      shader_cat.error()
        << "Shader encountered an error.\n";
      return false;
    }
#else
    shader_cat.error()
      << "Tried to load Cg shader, but no Cg support is enabled.\n";
    return false;
#endif
  }

  if (!source_filename.empty()) {
    module->set_source_filename(source_filename);
  }

  if (has_stage(stage)) {
    shader_cat.error()
      << "Shader already has a module with stage " << stage << ".\n";
    return false;
  }

  if (_module_mask > (1u << (uint32_t)stage)) {
    shader_cat.error()
      << "Shader modules must be loaded in increasing stage order.\n";
    return false;
  }

  // Link its inputs up with the previous stage.
  if (!_modules.empty()) {
    if (!module->link_inputs(_modules.back().get_read_pointer())) {
      shader_cat.error()
        << "Unable to match shader module interfaces.\n";
      return false;
    }
  }
  else if (stage == Stage::vertex) {
    // Bind vertex inputs right away.
    bool success = true;
    for (const ShaderModule::Variable &var : module->_inputs) {
      if (!bind_vertex_input(var.name, var.type, var.get_location())) {
        success = false;
      }
    }
    if (!success) {
      shader_cat.error()
        << "Failed to bind vertex inputs.\n";
      return false;
    }
  }

  _modules.push_back(std::move(module));
  _module_mask |= (1u << (uint32_t)stage);

  return true;
}

/**
 * Loads the shader file from the given string into the given string,
 * performing any pre-processing on it that may be necessary.
 *
 * Returns false if there was an error with this shader bad enough to consider
 * it 'invalid'.
 */
bool Shader::
do_load_source(ShaderModule::Stage stage, const std::string &source, BamCacheRecord *record) {
  std::istringstream in(source);
  return do_read_source(stage, in, Filename("created-shader"), record);
}

/**
 * Completes the binding between the different shader stages.
 */
bool Shader::
link() {
  // Go through all the modules to fetch the parameters.
  pmap<CPT_InternalName, const ShaderModule::Variable *> parameters;
  BitArray used_locations;

  for (COWPT(ShaderModule) &cow_module : _modules) {
    const ShaderModule *module = cow_module.get_read_pointer();
    pmap<int, int> remap;

    for (const ShaderModule::Variable &var : module->_parameters) {
      const auto result = parameters.insert({var.name, &var});
      const auto &it = result.first;

      if (!result.second) {
        // A variable by this name was already added by another stage.  Check
        // that it has the same type and location.
        const ShaderModule::Variable &other = *(it->second);
        if (other.type != var.type) {
          shader_cat.error()
            << "Parameter " << var.name << " in module " << *module
            << " is declared in another stage with a mismatching type!\n";
          return false;
        }

        if (it->second->get_location() != var.get_location()) {
          // Different location; need to remap this.
          remap[var.get_location()] = it->second->get_location();
        }
      } else if (var.has_location()) {
        if (used_locations.get_bit(var.get_location())) {
          // This location is already used.
          int location = used_locations.get_lowest_off_bit();
          used_locations.set_bit(location);
          remap[var.get_location()] = location;
        } else {
          used_locations.set_bit(var.get_location());
        }
      }
    }

    if (!remap.empty()) {
      PT(ShaderModule) module = cow_module.get_write_pointer();
      if (shader_cat.is_debug()) {
        shader_cat.debug()
          << "Remapping " << remap.size() << " locations for module " << *module << "\n";
      }
      module->remap_parameter_locations(remap);
    }
  }

  // Now bind all of the parameters.
  bool success = true;
  for (const auto &pair : parameters) {
    const ShaderModule::Variable &var = *pair.second;

    if (!bind_parameter(var.name, var.type, var.get_location())) {
      success = false;
    }
  }

  return success;
}

/**
 * Binds a vertex input parameter with the given type.
 */
bool Shader::
bind_vertex_input(const InternalName *name, const ::ShaderType *type, int location) {
  std::string name_str = name->get_name();

  Shader::ShaderVarSpec bind;
  bind._id._name = name_str;
  bind._id._seqno = location;
  bind._name = nullptr;
  bind._append_uv = -1;

  //FIXME: other types, matrices
  bind._elements = 1;
  bind._numeric_type = SPT_float;

  if (shader_cat.is_debug()) {
    shader_cat.debug()
      << "Binding vertex input " << name_str << " with type " << *type
      << " and location " << location << "\n";
  }

  // Check if it has a p3d_ prefix - if so, assign special meaning.
  if (_language == Shader::SL_GLSL && name_str.compare(0, 4, "p3d_") == 0) {
    // GLSL-style vertex input.
    if (name_str == "p3d_Vertex") {
      bind._name = InternalName::get_vertex();
    }
    else if (name_str == "p3d_Normal") {
      bind._name = InternalName::get_normal();
    }
    else if (name_str == "p3d_Color") {
      bind._name = InternalName::get_color();
    }
    else if (name_str.compare(4, 7, "Tangent") == 0) {
      bind._name = InternalName::get_tangent();
      if (name_str.size() > 11) {
        bind._append_uv = atoi(name_str.substr(11).c_str());
      }
    }
    else if (name_str.compare(4, 8, "Binormal") == 0) {
      bind._name = InternalName::get_binormal();
      if (name_str.size() > 12) {
        bind._append_uv = atoi(name_str.substr(12).c_str());
      }
    }
    else if (name_str.compare(4, 13, "MultiTexCoord") == 0 && name_str.size() > 17) {
      bind._name = InternalName::get_texcoord();
      bind._append_uv = atoi(name_str.substr(17).c_str());
    }
    else {
      shader_cat.error()
        << "Unrecognized built-in vertex input name '" << name_str << "'!\n";
      return false;
    }
  }
  else if (_language == Shader::SL_Cg && name_str.compare(0, 4, "vtx_") == 0) {
    // Cg-style vertex input.
    if (name_str == "vtx_position") {
      bind._name = InternalName::get_vertex();
      bind._append_uv = -1;
    }
    else if (name_str.substr(4, 8) == "texcoord") {
      bind._name = InternalName::get_texcoord();
      if (name_str.size() > 12) {
        bind._append_uv = atoi(name_str.c_str() + 12);
      }
    }
    else if (name_str.substr(4, 7) == "tangent") {
      bind._name = InternalName::get_tangent();
      if (name_str.size() > 11) {
        bind._append_uv = atoi(name_str.c_str() + 11);
      }
    }
    else if (name_str.substr(4, 8) == "binormal") {
      bind._name = InternalName::get_binormal();
      if (name_str.size() > 11) {
        bind._append_uv = atoi(name_str.c_str() + 11);
      }
    }
    else {
      bind._name = InternalName::make(name_str.substr(4));
    }
  }
  else {
    // Arbitrarily named attribute.
    bind._name = InternalName::make(name_str);
  }

  _var_spec.push_back(bind);
  return true;
}

/**
 * Binds a uniform parameter with the given type.
 */
bool Shader::
bind_parameter(const InternalName *name, const ::ShaderType *type, int location) {
  std::string name_str = name->get_name();

  ShaderArgId arg_id;
  arg_id._name = name_str;
  arg_id._seqno = location;

  // Split it at the underscores.
  vector_string pieces;
  tokenize(name_str, pieces, "_");
  nassertr(!pieces.empty(), false);

  // Check if it has a p3d_ prefix - if so, assign special meaning.
  if (pieces[0] == "p3d" && _language == SL_GLSL) {
    if (pieces[1] == "ModelViewProjectionMatrix") {
      ShaderMatSpec bind;
      bind._id = arg_id;
      bind._func = SMF_compose;
      bind._piece = SMP_whole;
      bind._arg[0] = nullptr;
      bind._arg[1] = nullptr;
      bind._part[0] = SMO_model_to_apiview;
      bind._part[1] = SMO_apiview_to_apiclip;
      cp_add_mat_spec(bind);
      return true;
    }
    if (pieces[1].compare(0, 7, "Texture") == 0) {
      ShaderTexSpec bind;
      bind._id = arg_id;
      bind._part = STO_stage_i;
      bind._name = 0;
      bind._desired_type = Texture::TT_2d_texture;

      string tail;
      bind._stage = string_to_int(name_str.substr(7), tail);
      if (!tail.empty()) {
        shader_cat.error()
          << "Error parsing shader input name: unexpected '"
          << tail << "' in '" << name_str << "'\n";
        return false;
      }

      _tex_spec.push_back(bind);
      return true;
    }
    if (pieces[1] == "ColorScale") {
      ShaderMatSpec bind;
      bind._id = arg_id;
      bind._func = SMF_first;
      bind._part[0] = SMO_attr_colorscale;
      bind._arg[0] = nullptr;
      bind._part[1] = SMO_identity;
      bind._arg[1] = nullptr;
      bind._piece = SMP_row3;
      cp_add_mat_spec(bind);
      return true;
    }
    if (pieces[1] == "TexAlphaOnly") {
      ShaderMatSpec bind;
      bind._id = arg_id;
      bind._func = SMF_first;
      bind._index = 0;
      bind._part[0] = SMO_tex_is_alpha_i;
      bind._arg[0] = nullptr;
      bind._part[1] = SMO_identity;
      bind._arg[1] = nullptr;
      bind._piece = SMP_row3;
      cp_add_mat_spec(bind);
      return true;
    }

    shader_cat.error() << "Unrecognized uniform name '" << name_str << "'!\n";
    return false;
  }

  // Check for mstrans, wstrans, vstrans, cstrans, mspos, wspos, vspos, cspos
  if (pieces[0].compare(1, std::string::npos, "strans") == 0 ||
      pieces[0].compare(1, std::string::npos, "spos") == 0) {
    pieces.push_back("to");

    switch (pieces[0][0]) {
    case 'm':
      pieces.push_back("model");
      break;
    case 'w':
      pieces.push_back("world");
      break;
    case 'v':
      pieces.push_back("view");
      break;
    case 'c':
      pieces.push_back("clip");
      break;
    default:
      return false;
    }
    if (pieces[0].compare(1, std::string::npos, "strans") == 0) {
      pieces[0] = "trans";
    } else {
      pieces[0] = "row3";
    }
  }
  else if (pieces[0].size() == 3 && // mat_modelproj et al
           (pieces[0] == "mat" || pieces[0] == "inv" ||
            pieces[0] == "tps" || pieces[0] == "itp")) {
    std::string trans = pieces[0];
    std::string matrix = pieces[1];
    pieces.clear();
    if (matrix == "modelview") {
      tokenize("trans_model_to_apiview", pieces, "_");
    } else if (matrix == "projection") {
      tokenize("trans_apiview_to_apiclip", pieces, "_");
    } else if (matrix == "modelproj") {
      tokenize("trans_model_to_apiclip", pieces, "_");
    } else {
      shader_cat.error()
        << "Unrecognized matrix name " << name_str << "\n";
      return false;
    }
    if (trans == "mat") {
      pieces[0] = "trans";
    } else if (trans == "inv") {
      string t = pieces[1];
      pieces[1] = pieces[3];
      pieces[3] = t;
    } else if (trans == "tps") {
      pieces[0] = "tpose";
    } else if (trans == "itp") {
      string t = pieces[1];
      pieces[1] = pieces[3];
      pieces[3] = t;
      pieces[0] = "tpose";
    }
  }

  // Implement the Cg-style transform-matrix generator.
  if (pieces[0] == "trans" ||
      pieces[0] == "tpose" ||
      pieces[0] == "row0" ||
      pieces[0] == "row1" ||
      pieces[0] == "row2" ||
      pieces[0] == "row3" ||
      pieces[0] == "col0" ||
      pieces[0] == "col1" ||
      pieces[0] == "col2" ||
      pieces[0] == "col3") {

    ShaderMatSpec bind;
    bind._id = arg_id;
    bind._func = SMF_compose;

    //int next = 1;
    pieces.push_back("");

    if (pieces[0] == "trans" || pieces[0] == "tpose") {
      if (const ::ShaderType::Matrix *matrix = type->as_matrix()) {
        if (matrix->get_num_columns() >= 4 || matrix->get_num_columns() >= 4) {
          bind._piece = (pieces[0][2] == 'p') ? SMP_transpose : SMP_whole;
        } else {
          bind._piece = (pieces[0][2] == 'p') ? SMP_transpose3x3 : SMP_upper3x3;
        }
      } else {
        shader_cat.error()
          << "Shader input " << name_str << " should be a matrix type\n";
        return false;
      }
    }

    //if (!cp_parse_coord_sys(p, pieces, next, bind, true)) {
    //  return false;
    //}
    //if (!cp_parse_delimiter(p, pieces, next)) {
    //  return false;
    //}
    //if (!cp_parse_coord_sys(p, pieces, next, bind, false)) {
    //  return false;
    //}
    //if (!cp_parse_eol(p, pieces, next)) {
    //  return false;
    //}

    cp_add_mat_spec(bind);
    return true;
  }

  if (_language == SL_Cg && pieces[0] == "k") {
    name_str = name_str.substr(2);
    name = InternalName::make(name_str);
  }

  // If we get here, it's not a specially recognized input, but just a regular
  // user-defined input.
  if (const ::ShaderType::Array *array = type->as_array()) {
    // A uniform array.
    const ::ShaderType *element_type = array->get_element_type();

    ShaderPtrSpec bind;
    bind._id = arg_id;

    if (const ::ShaderType::Matrix *matrix = element_type->as_matrix()) {
      size_t num_rows = matrix->get_num_rows();
      if (matrix->get_num_columns() != num_rows) {
        shader_cat.error()
          << "Uniform parameter '" << name_str << "' has unsupported non-square matrix type\n";
        return false;
      }

      bind._dim[1] = num_rows * num_rows;
      bind._type = SPT_float;
    }
    else if (const ::ShaderType::Vector *vector = element_type->as_vector()) {
      bind._dim[1] = 4;
      bind._type = SPT_float;
    }
    else if (const ::ShaderType::Scalar *scalar = element_type->as_scalar()) {
      bind._dim[1] = 1;
      bind._type = SPT_float;
    }

    bind._arg = name;
    bind._dim[0] = array->get_num_elements();
    bind._dep[0] = SSD_general | SSD_shaderinputs | SSD_frame;
    bind._dep[1] = SSD_NONE;
    _ptr_spec.push_back(bind);
    return true;
  }
  else if (const ::ShaderType::SampledImage *sampler = type->as_sampled_image()) {
    ShaderTexSpec bind;
    bind._id = arg_id;
    bind._part = STO_named_input;
    bind._name = name;
    bind._desired_type = sampler->get_texture_type();
    bind._stage = 0;
    _tex_spec.push_back(bind);
    return true;
  }
  else if (const ::ShaderType::Image *image = type->as_image()) {
    ShaderImgSpec bind;
    bind._id = arg_id;
    bind._name = name;
    bind._desired_type = image->get_texture_type();
    bind._writable = image->is_writable();
    _img_spec.push_back(bind);
    return true;
  }
  else if (const ::ShaderType::Matrix *matrix = type->as_matrix()) {
    if (matrix->get_num_columns() == 3 && matrix->get_num_rows() == 3) {
      ShaderMatSpec bind;
      bind._id = arg_id;
      bind._piece = SMP_upper3x3;
      bind._func = SMF_first;
      bind._part[0] = SMO_mat_constant_x;
      bind._arg[0] = name;
      bind._part[1] = SMO_identity;
      bind._arg[1] = nullptr;
      cp_add_mat_spec(bind);
      return true;
    }
    else if (matrix->get_num_columns() == 4 && matrix->get_num_rows() == 4) {
      ShaderMatSpec bind;
      bind._id = arg_id;
      bind._piece = SMP_whole;
      bind._func = SMF_first;
      bind._part[1] = SMO_identity;
      bind._arg[1] = nullptr;

      if (name->get_parent() != InternalName::get_root()) {
        // It might be something like an attribute of a shader input, like a
        // light parameter.  It might also just be a custom struct
        // parameter.  We can't know yet, sadly.
        if (name->get_basename() == "shadowMatrix") {
          // Special exception for shadowMatrix, which is deprecated,
          // because it includes the model transformation.  It is far more
          // efficient to do that in the shader instead.
          static bool warned = false;
          if (!warned) {
            warned = true;
            shader_cat.warning()
              << "light.shadowMatrix inputs are deprecated; use "
                 "shadowViewMatrix instead, which transforms from view "
                 "space instead of model space.\n";
          }
          bind._func = SMF_compose;
          bind._part[0] = SMO_model_to_apiview;
          bind._arg[0] = nullptr;
          bind._part[1] = SMO_mat_constant_x_attrib;
          bind._arg[1] = name->get_parent()->append("shadowViewMatrix");
        } else {
          bind._part[0] = SMO_mat_constant_x_attrib;
          bind._arg[0] = name;
        }
      } else {
        bind._part[0] = SMO_mat_constant_x;
        bind._arg[0] = name;
      }
      cp_add_mat_spec(bind);
      return true;
    }
  }
  else if (type == ::ShaderType::float_type) {
    if (name->get_parent() != InternalName::get_root()) {
      // It might be something like an attribute of a shader input, like a
      // light parameter.  It might also just be a custom struct
      // parameter.  We can't know yet, sadly.
      ShaderMatSpec bind;
      bind._id = arg_id;
      bind._piece = SMP_row3x1;
      bind._func = SMF_first;
      bind._part[0] = SMO_vec_constant_x_attrib;
      bind._arg[0] = name;
      // We need SSD_view_transform since some attributes (eg. light
      // position) have to be transformed to view space.
      bind._part[1] = SMO_identity;
      bind._arg[1] = nullptr;
      cp_add_mat_spec(bind);
      return true;
    }
    else {
      ShaderPtrSpec bind;
      bind._id = arg_id;
      bind._arg = name;
      bind._dim[0] = 1;
      bind._dim[1] = 1;
      bind._type = SPT_float;
      _ptr_spec.push_back(bind);
      return true;
    }
  }

  shader_cat.error()
    << "Uniform parameter '" << name_str << "' has unsupported type "
    << *type << "\n";
  return false;
}

/**
 * Checks whether the shader or any of its dependent files were modified on
 * disk.
 */
bool Shader::
check_modified() const {
  for (COWPT(ShaderModule) const &cow_module : _modules) {
    const ShaderModule *module = cow_module.get_read_pointer();

    if (module->_record != nullptr && !module->_record->dependents_unchanged()) {
      return true;
    }
  }
  return false;
}

/**
 * Find a ShaderCompiler in the global registry that makes the supplied language
 */
ShaderCompiler *Shader::
get_compiler(ShaderLanguage lang) const {
  ShaderCompilerRegistry *registry = ShaderCompilerRegistry::get_global_ptr();
  return registry->get_compiler_from_language(lang);
}

/**
 * Delete the compiled code, if it exists.
 */
Shader::
~Shader() {
  release_all();
  // Note: don't try to erase ourselves from the table.  It currently keeps a
  // reference forever, and so the only place where this constructor is called
  // is in the destructor of the table itself.
  /*if (_loaded) {
    _load_table.erase(_filename);
  } else {
    _make_table.erase(_text);
  }*/
}

/**
 * Loads the shader with the given filename.
 */
PT(Shader) Shader::
load(const Filename &file, ShaderLanguage lang) {
  ShaderFile sfile(file);
  ShaderTable::const_iterator i = _load_table.find(sfile);
  if (i != _load_table.end() && (lang == SL_none || lang == i->second->_language)) {
    // But check that someone hasn't modified it in the meantime.
    if (i->second->check_modified()) {
      shader_cat.info()
        << "Shader " << file << " was modified on disk, reloading.\n";
    } else {
      if (shader_cat.is_debug()) {
        shader_cat.debug()
          << "Shader " << file << " was found in shader cache.\n";
      }
      return i->second;
    }
  }

  PT(Shader) shader = new Shader(lang);
  if (!shader->read(sfile)) {
    return nullptr;
  }

  _load_table[sfile] = shader;

  /*if (cache_generated_shaders) {
    ShaderTable::const_iterator i = _make_table.find(shader->_text);
    if (i != _make_table.end() && (lang == SL_none || lang == i->second->_language)) {
      return i->second;
    }
    _make_table[shader->_text] = shader;
  }*/
  return shader;
}

/**
 * This variant of Shader::load loads all shader programs separately.
 */
PT(Shader) Shader::
load(ShaderLanguage lang, const Filename &vertex,
     const Filename &fragment, const Filename &geometry,
     const Filename &tess_control, const Filename &tess_evaluation) {
  ShaderFile sfile(vertex, fragment, geometry, tess_control, tess_evaluation);
  ShaderTable::const_iterator i = _load_table.find(sfile);
  if (i != _load_table.end() && (lang == SL_none || lang == i->second->_language)) {
    // But check that someone hasn't modified it in the meantime.
    if (i->second->check_modified()) {
      shader_cat.info()
        << "Shader was modified on disk, reloading.\n";
    } else {
      if (shader_cat.is_debug()) {
        shader_cat.debug()
          << "Shader was found in shader cache.\n";
      }
      return i->second;
    }
  }

  PT(Shader) shader = new Shader(lang);
  if (!shader->read(sfile)) {
    return nullptr;
  }

  _load_table[sfile] = shader;

  /*if (cache_generated_shaders) {
    ShaderTable::const_iterator i = _make_table.find(shader->_text);
    if (i != _make_table.end() && (lang == SL_none || lang == i->second->_language)) {
      return i->second;
    }
    _make_table[shader->_text] = shader;
  }*/
  return shader;
}

/**
 * Loads a compute shader.
 */
PT(Shader) Shader::
load_compute(ShaderLanguage lang, const Filename &fn) {
  if (lang != SL_GLSL) {
    shader_cat.error()
      << "Only GLSL compute shaders are currently supported.\n";
    return nullptr;
  }

  Filename fullpath(fn);
  VirtualFileSystem *vfs = VirtualFileSystem::get_global_ptr();
  if (!vfs->resolve_filename(fullpath, get_model_path())) {
    shader_cat.error()
      << "Could not find compute shader file: " << fn << "\n";
    return nullptr;
  }

  ShaderFile sfile;
  sfile._separate = true;
  sfile._compute = fn;

  ShaderTable::const_iterator i = _load_table.find(sfile);
  if (i != _load_table.end() && (lang == SL_none || lang == i->second->_language)) {
    // But check that someone hasn't modified it in the meantime.
    if (i->second->check_modified()) {
      shader_cat.info()
        << "Compute shader " << fn << " was modified on disk, reloading.\n";
    } else {
      if (shader_cat.is_debug()) {
        shader_cat.debug()
          << "Compute shader " << fn << " was found in shader cache.\n";
      }
      return i->second;
    }
  }

  BamCache *cache = BamCache::get_global_ptr();
  PT(BamCacheRecord) record = cache->lookup(fullpath, "sho");
  if (record != nullptr) {
    if (record->has_data()) {
      PT(Shader) shader = DCAST(Shader, record->get_data());
      if (shader->_module_mask == (1 << (int)Stage::compute)) {
        shader_cat.info()
          << "Compute shader " << fn << " was found in disk cache.\n";
        return shader;
      }
    }
  }

  PT(Shader) shader = new Shader(lang);
  if (!shader->do_read_source(Stage::compute, fullpath, record)) {
    return nullptr;
  }

  _load_table[sfile] = shader;

  /*if (cache_generated_shaders) {
    ShaderTable::const_iterator i = _make_table.find(shader->_text);
    if (i != _make_table.end() && (lang == SL_none || lang == i->second->_language)) {
      return i->second;
    }
    _make_table[shader->_text] = shader;
  }*/

  // It makes little sense to cache the shader before compilation, so we keep
  // the record for when we have the compiled the shader.
  //shader->_record = std::move(record);
  shader->_cache_compiled_shader = BamCache::get_global_ptr()->get_cache_compiled_shaders();
  shader->_fullpath = std::move(fullpath);
  return shader;
}

/**
 * Loads the shader, using the string as shader body.
 */
PT(Shader) Shader::
make(string body, ShaderLanguage lang) {
  if (lang == SL_GLSL) {
    shader_cat.error()
      << "GLSL shaders must have separate shader bodies!\n";
    return nullptr;

  } else if (lang == SL_none) {
    shader_cat.warning()
      << "Shader::make() now requires an explicit shader language.  Assuming Cg.\n";
    lang = SL_Cg;
  }
#ifndef HAVE_CG
  if (lang == SL_Cg) {
    shader_cat.error() << "Support for Cg shaders is not enabled.\n";
    return nullptr;
  }
#endif

  ShaderFile sbody(move(body));

  if (cache_generated_shaders) {
    ShaderTable::const_iterator i = _make_table.find(sbody);
    if (i != _make_table.end() && (lang == SL_none || lang == i->second->_language)) {
      // But check that someone hasn't modified its includes in the meantime.
      if (!i->second->check_modified()) {
        return i->second;
      }
    }
  }

  PT(Shader) shader = new Shader(lang);
  if (!shader->load(sbody)) {
    return nullptr;
  }

  /*if (cache_generated_shaders) {
    ShaderTable::const_iterator i = _make_table.find(shader->_text);
    if (i != _make_table.end() && (lang == SL_none || lang == i->second->_language)) {
      shader = i->second;
    } else {
      _make_table[shader->_text] = shader;
    }
    _make_table[std::move(sbody)] = shader;
  }*/

  if (dump_generated_shaders) {
    ostringstream fns;
    int index = _shaders_generated ++;
    fns << "genshader" << index;
    string fn = fns.str();
    shader_cat.warning() << "Dumping shader: " << fn << "\n";

    pofstream s;
    s.open(fn.c_str(), std::ios::out | std::ios::trunc);
    s << shader->get_text();
    s.close();
  }
  return shader;
}

/**
 * Loads the shader, using the strings as shader bodies.
 */
PT(Shader) Shader::
make(ShaderLanguage lang, string vertex, string fragment, string geometry,
     string tess_control, string tess_evaluation) {
#ifndef HAVE_CG
  if (lang == SL_Cg) {
    shader_cat.error() << "Support for Cg shaders is not enabled.\n";
    return nullptr;
  }
#endif
  if (lang == SL_none) {
    shader_cat.error()
      << "Shader::make() requires an explicit shader language.\n";
    return nullptr;
  }

  ShaderFile sbody(move(vertex), move(fragment), move(geometry),
                   move(tess_control), move(tess_evaluation));

  if (cache_generated_shaders) {
    ShaderTable::const_iterator i = _make_table.find(sbody);
    if (i != _make_table.end() && (lang == SL_none || lang == i->second->_language)) {
      // But check that someone hasn't modified its includes in the meantime.
      if (!i->second->check_modified()) {
        return i->second;
      }
    }
  }

  PT(Shader) shader = new Shader(lang);
  shader->_filename = ShaderFile("created-shader");
  if (!shader->load(sbody)) {
    return nullptr;
  }

  /*if (cache_generated_shaders) {
    ShaderTable::const_iterator i = _make_table.find(shader->_text);
    if (i != _make_table.end() && (lang == SL_none || lang == i->second->_language)) {
      shader = i->second;
    } else {
      _make_table[shader->_text] = shader;
    }
    _make_table[std::move(sbody)] = shader;
  }*/

  return shader;
}

/**
 * Loads the compute shader from the given string.
 */
PT(Shader) Shader::
make_compute(ShaderLanguage lang, string body) {
  if (lang != SL_GLSL) {
    shader_cat.error()
      << "Only GLSL compute shaders are currently supported.\n";
    return nullptr;
  }

  ShaderFile sbody;
  sbody._separate = true;
  sbody._compute = move(body);

  if (cache_generated_shaders) {
    ShaderTable::const_iterator i = _make_table.find(sbody);
    if (i != _make_table.end() && (lang == SL_none || lang == i->second->_language)) {
      // But check that someone hasn't modified its includes in the meantime.
      if (!i->second->check_modified()) {
        return i->second;
      }
    }
  }

  PT(Shader) shader = new Shader(lang);
  shader->_filename = ShaderFile("created-shader");
  if (!shader->load(sbody)) {
    return nullptr;
  }

  /*if (cache_generated_shaders) {
    ShaderTable::const_iterator i = _make_table.find(shader->_text);
    if (i != _make_table.end() && (lang == SL_none || lang == i->second->_language)) {
      shader = i->second;
    } else {
      _make_table[shader->_text] = shader;
    }
    _make_table[std::move(sbody)] = shader;
  }*/

  return shader;
}

/**
 * Indicates that the shader should be enqueued to be prepared in the
 * indicated prepared_objects at the beginning of the next frame.  This will
 * ensure the texture is already loaded into texture memory if it is expected
 * to be rendered soon.
 *
 * Use this function instead of prepare_now() to preload textures from a user
 * interface standpoint.
 */
PT(AsyncFuture) Shader::
prepare(PreparedGraphicsObjects *prepared_objects) {
  return prepared_objects->enqueue_shader_future(this);
}

/**
 * Returns true if the shader has already been prepared or enqueued for
 * preparation on the indicated GSG, false otherwise.
 */
bool Shader::
is_prepared(PreparedGraphicsObjects *prepared_objects) const {
  Contexts::const_iterator ci;
  ci = _contexts.find(prepared_objects);
  if (ci != _contexts.end()) {
    return true;
  }
  return prepared_objects->is_shader_queued(this);
}

/**
 * Frees the texture context only on the indicated object, if it exists there.
 * Returns true if it was released, false if it had not been prepared.
 */
bool Shader::
release(PreparedGraphicsObjects *prepared_objects) {
  Contexts::iterator ci;
  ci = _contexts.find(prepared_objects);
  if (ci != _contexts.end()) {
    ShaderContext *sc = (*ci).second;
    if (sc != nullptr) {
      prepared_objects->release_shader(sc);
    } else {
      _contexts.erase(ci);
    }
    return true;
  }

  // Maybe it wasn't prepared yet, but it's about to be.
  return prepared_objects->dequeue_shader(this);
}

/**
 * Creates a context for the shader on the particular GSG, if it does not
 * already exist.  Returns the new (or old) ShaderContext.  This assumes that
 * the GraphicsStateGuardian is the currently active rendering context and
 * that it is ready to accept new textures.  If this is not necessarily the
 * case, you should use prepare() instead.
 *
 * Normally, this is not called directly except by the GraphicsStateGuardian;
 * a shader does not need to be explicitly prepared by the user before it may
 * be rendered.
 */
ShaderContext *Shader::
prepare_now(PreparedGraphicsObjects *prepared_objects,
            GraphicsStateGuardianBase *gsg) {
  Contexts::const_iterator ci;
  ci = _contexts.find(prepared_objects);
  if (ci != _contexts.end()) {
    return (*ci).second;
  }

  ShaderContext *tc = prepared_objects->prepare_shader_now(this, gsg);
  _contexts[prepared_objects] = tc;

  return tc;
}

/**
 * Removes the indicated PreparedGraphicsObjects table from the Shader's
 * table, without actually releasing the texture.  This is intended to be
 * called only from PreparedGraphicsObjects::release_texture(); it should
 * never be called by user code.
 */
void Shader::
clear_prepared(PreparedGraphicsObjects *prepared_objects) {
  Contexts::iterator ci;
  ci = _contexts.find(prepared_objects);
  if (ci != _contexts.end()) {
    _contexts.erase(ci);
  } else {
    // If this assertion fails, clear_prepared() was given a prepared_objects
    // which the texture didn't know about.
    nassert_raise("unknown PreparedGraphicsObjects");
  }
}

/**
 * Frees the context allocated on all objects for which the texture has been
 * declared.  Returns the number of contexts which have been freed.
 */
int Shader::
release_all() {
  // We have to traverse a copy of the _contexts list, because the
  // PreparedGraphicsObjects object will call clear_prepared() in response to
  // each release_texture(), and we don't want to be modifying the _contexts
  // list while we're traversing it.
  Contexts temp = _contexts;
  int num_freed = (int)_contexts.size();

  Contexts::const_iterator ci;
  for (ci = temp.begin(); ci != temp.end(); ++ci) {
    PreparedGraphicsObjects *prepared_objects = (*ci).first;
    ShaderContext *sc = (*ci).second;
    if (sc != nullptr) {
      prepared_objects->release_shader(sc);
    }
  }

  // There might still be some outstanding contexts in the map, if there were
  // any NULL pointers there.  Eliminate them.
  _contexts.clear();

  return num_freed;
}

/**
 *
 */
void Shader::ShaderCaps::
clear() {
  _supports_glsl = false;

#ifdef HAVE_CG
  _active_vprofile = CG_PROFILE_UNKNOWN;
  _active_fprofile = CG_PROFILE_UNKNOWN;
  _active_gprofile = CG_PROFILE_UNKNOWN;
  _ultimate_vprofile = CG_PROFILE_UNKNOWN;
  _ultimate_fprofile = CG_PROFILE_UNKNOWN;
  _ultimate_gprofile = CG_PROFILE_UNKNOWN;
#endif
}

/**
 * Tells the BamReader how to create objects of type Shader.
 */
void Shader::
register_with_read_factory() {
  BamReader::get_factory()->register_factory(get_class_type(), make_from_bam);
}

/**
 * Writes the contents of this object to the datagram for shipping out to a
 * Bam file.
 */
void Shader::
write_datagram(BamWriter *manager, Datagram &dg) {
  dg.add_uint8(_language);
  dg.add_bool(_loaded);
  _filename.write_datagram(dg);
  _text.write_datagram(dg);

  dg.add_uint32(_compiled_format);
  dg.add_string(_compiled_binary);
}

/**
 * This function is called by the BamReader's factory when a new object of
 * type Shader is encountered in the Bam file.  It should create the Shader
 * and extract its information from the file.
 */
TypedWritable *Shader::
make_from_bam(const FactoryParams &params) {
  Shader *attrib = new Shader(SL_none);
  DatagramIterator scan;
  BamReader *manager;

  parse_params(params, scan, manager);
  attrib->fillin(scan, manager);
  return attrib;
}

/**
 * This internal function is called by make_from_bam to read in all of the
 * relevant data from the BamFile for the new Shader.
 */
void Shader::
fillin(DatagramIterator &scan, BamReader *manager) {
  _language = (ShaderLanguage) scan.get_uint8();
  _loaded = scan.get_bool();
  _filename.read_datagram(scan);
  _text.read_datagram(scan);

  _compiled_format = scan.get_uint32();
  _compiled_binary = scan.get_string();
}
