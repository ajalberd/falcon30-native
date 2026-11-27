#pragma once

#include <SDL3/SDL_opengl.h>
#include <string>

namespace falcon {

// Loads a PNG file into an OpenGL 2D RGBA8 texture (returns 0 on failure).
GLuint load_png_texture_gl(const std::string& path, int& out_w, int& out_h);

} // namespace falcon
