#include "core/render/batcher.hpp"

#include <glad/glad.h>

#include <cstdio>
#include <cstring>

namespace core::render {
namespace {

struct Glyph {
  std::uint8_t rows[7]; // 5 bits per row, bit 4 = leftmost column
};

// 5x7 bitmap font: ASCII 32..96. Lowercase maps to uppercase.
// clang-format off
constexpr Glyph kFont[65] = {
  /*   */ {{ 0, 0, 0, 0, 0, 0, 0}},
  /* ! */ {{ 4, 4, 4, 4, 4, 0, 4}},
  /* " */ {{10,10, 0, 0, 0, 0, 0}},
  /* # */ {{10,31,10,10,31,10, 0}},
  /* $ */ {{ 6,15,20,14, 5,30,12}},
  /* % */ {{24,25, 2, 4, 9, 3, 0}},
  /* & */ {{12,18,20, 8,21,18,13}},
  /* ' */ {{ 4, 4, 0, 0, 0, 0, 0}},
  /* ( */ {{ 2, 4, 8, 8, 8, 4, 2}},
  /* ) */ {{ 8, 4, 2, 2, 2, 4, 8}},
  /* * */ {{ 0,21,14,31,14,21, 0}},
  /* + */ {{ 0, 4, 4,31, 4, 4, 0}},
  /* , */ {{ 0, 0, 0, 0,12, 4, 8}},
  /* - */ {{ 0, 0, 0,31, 0, 0, 0}},
  /* . */ {{ 0, 0, 0, 0, 0,12,12}},
  /* / */ {{ 1, 2, 4, 4, 8,16, 0}},
  /* 0 */ {{14,17,19,21,25,17,14}},
  /* 1 */ {{ 4,12, 4, 4, 4, 4,14}},
  /* 2 */ {{14,17, 1, 2, 8,16,31}},
  /* 3 */ {{31, 2, 8, 2, 1,17,14}},
  /* 4 */ {{ 2, 6,10,18,31, 2, 2}},
  /* 5 */ {{31,16,30, 1, 1,17,14}},
  /* 6 */ {{ 6, 8,16,30,17,17,14}},
  /* 7 */ {{31, 1, 2, 4, 8, 8, 8}},
  /* 8 */ {{14,17,17,14,17,17,14}},
  /* 9 */ {{14,17,17,31, 1, 2,12}},
  /* : */ {{ 0,12,12, 0,12,12, 0}},
  /* ; */ {{ 0,12,12, 0,12, 4, 8}},
  /* < */ {{ 2, 4, 8,16, 8, 4, 2}},
  /* = */ {{ 0, 0,31, 0,31, 0, 0}},
  /* > */ {{ 8, 4, 2, 1, 2, 4, 8}},
  /* ? */ {{14,17, 1, 2, 4, 0, 4}},
  /* @ */ {{14,17,27,21,27,16,14}},
  /* A */ {{14,17,17,31,17,17,17}},
  /* B */ {{30,17,17,30,17,17,30}},
  /* C */ {{14,17,16,16,16,17,14}},
  /* D */ {{28,18,17,17,17,18,28}},
  /* E */ {{31,16,16,30,16,16,31}},
  /* F */ {{31,16,16,30,16,16,16}},
  /* G */ {{14,17,16,23,17,17,15}},
  /* H */ {{17,17,17,31,17,17,17}},
  /* I */ {{14, 4, 4, 4, 4, 4,14}},
  /* J */ {{ 7, 2, 2, 2, 2,18,12}},
  /* K */ {{17,18,20,24,20,18,17}},
  /* L */ {{16,16,16,16,16,16,31}},
  /* M */ {{17,27,21,21,17,17,17}},
  /* N */ {{17,17,25,21,19,17,17}},
  /* O */ {{14,17,17,17,17,17,14}},
  /* P */ {{30,17,17,30,16,16,16}},
  /* Q */ {{14,17,17,17,21,18,13}},
  /* R */ {{30,17,17,30,20,18,17}},
  /* S */ {{15,16,16,14, 1, 1,30}},
  /* T */ {{31, 4, 4, 4, 4, 4, 4}},
  /* U */ {{17,17,17,17,17,17,14}},
  /* V */ {{17,17,17,17,17,10, 4}},
  /* W */ {{17,17,17,21,21,21,10}},
  /* X */ {{17,17,10, 4,10,17,17}},
  /* Y */ {{17,17,10, 4, 4, 4, 4}},
  /* Z */ {{31, 1, 2, 4, 8,16,31}},
  /* [ */ {{14, 8, 8, 8, 8, 8,14}},
  /* \ */ {{16, 8, 4, 4, 2, 1, 0}},
  /* ] */ {{14, 2, 2, 2, 2, 2,14}},
  /* ^ */ {{ 4,10,17, 0, 0, 0, 0}},
  /* _ */ {{ 0, 0, 0, 0, 0, 0,31}},
};
// clang-format on

const char* kVertexShader = R"(#version 330 core
layout(location = 0) in vec2 aCorner;
layout(location = 1) in vec2 aCenter;
layout(location = 2) in vec2 aHalf;
layout(location = 3) in vec4 aColor;
layout(location = 4) in float aShape;
uniform vec2 uViewHalf;
out vec2 vUv;
out vec2 vWorldHalf;
out vec4 vColor;
flat out float vShape;
void main() {
  vUv = aCorner;
  vWorldHalf = aHalf * uViewHalf;
  vColor = aColor;
  vShape = aShape;
  gl_Position = vec4(aCenter + aCorner * aHalf, 0.0, 1.0);
}
)";

const char* kFragmentShader = R"(#version 330 core
in vec2 vUv;
in vec2 vWorldHalf;
in vec4 vColor;
flat in float vShape;
out vec4 frag;
void main() {
  if (vShape > 0.5) {
    vec2 p = vUv * vWorldHalf;
    float r = vWorldHalf.x;
    if (dot(p, p) > r * r) discard;
  }
  frag = vColor;
}
)";

unsigned compile(GLenum type, const char* src) {
  const unsigned sh = glCreateShader(type);
  glShaderSource(sh, 1, &src, nullptr);
  glCompileShader(sh);
  GLint ok = 0;
  glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[2048];
    glGetShaderInfoLog(sh, sizeof(log), nullptr, log);
    std::fprintf(stderr, "shader compile error:\n%s\n", log);
    glDeleteShader(sh);
    return 0;
  }
  return sh;
}

const Glyph& glyphFor(char c) {
  unsigned char u = static_cast<unsigned char>(c);
  if (u >= 'a' && u <= 'z') {
    u = static_cast<unsigned char>(u - 'a' + 'A');
  }
  if (u < 32 || u > 96) {
    u = '?';
  }
  return kFont[u - 32];
}

} // namespace

bool fontSupports(std::string_view s) {
  for (const char c : s) {
    unsigned char u = static_cast<unsigned char>(c);
    if (u >= 'a' && u <= 'z') {
      u = static_cast<unsigned char>(u - 'a' + 'A');
    }
    if (u < 32 || u > 96) return false;
  }
  return true;
}

bool Batcher::init() {
  const unsigned vs = compile(GL_VERTEX_SHADER, kVertexShader);
  const unsigned fs = compile(GL_FRAGMENT_SHADER, kFragmentShader);
  if (vs == 0 || fs == 0) {
    return false;
  }
  program_ = glCreateProgram();
  glAttachShader(program_, vs);
  glAttachShader(program_, fs);
  glLinkProgram(program_);
  glDeleteShader(vs);
  glDeleteShader(fs);

  GLint ok = 0;
  glGetProgramiv(program_, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[2048];
    glGetProgramInfoLog(program_, sizeof(log), nullptr, log);
    std::fprintf(stderr, "program link error:\n%s\n", log);
    return false;
  }
  uViewHalf_ = glGetUniformLocation(program_, "uViewHalf");

  constexpr float quad[8] = {-1.0F, -1.0F, 1.0F, -1.0F, -1.0F, 1.0F, 1.0F, 1.0F};

  glGenVertexArrays(1, &vao_);
  glBindVertexArray(vao_);

  glGenBuffers(1, &quadVbo_);
  glBindBuffer(GL_ARRAY_BUFFER, quadVbo_);
  glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
  glEnableVertexAttribArray(0);

  glGenBuffers(1, &instVbo_);
  glBindBuffer(GL_ARRAY_BUFFER, instVbo_);
  constexpr GLsizei stride = 9 * sizeof(float);
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride,
                        reinterpret_cast<void*>(0 * sizeof(float)));
  glEnableVertexAttribArray(1);
  glVertexAttribDivisor(1, 1);
  glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride,
                        reinterpret_cast<void*>(2 * sizeof(float)));
  glEnableVertexAttribArray(2);
  glVertexAttribDivisor(2, 1);
  glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, stride,
                        reinterpret_cast<void*>(4 * sizeof(float)));
  glEnableVertexAttribArray(3);
  glVertexAttribDivisor(3, 1);
  glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, stride,
                        reinterpret_cast<void*>(8 * sizeof(float)));
  glEnableVertexAttribArray(4);
  glVertexAttribDivisor(4, 1);

  glBindVertexArray(0);

  data_.reserve(16384 * 9);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glDisable(GL_DEPTH_TEST);
  return true;
}

void Batcher::shutdown() {
  if (instVbo_ != 0) glDeleteBuffers(1, &instVbo_);
  if (quadVbo_ != 0) glDeleteBuffers(1, &quadVbo_);
  if (vao_ != 0) glDeleteVertexArrays(1, &vao_);
  if (program_ != 0) glDeleteProgram(program_);
  instVbo_ = quadVbo_ = vao_ = program_ = 0;
}

void Batcher::beginFrame(int fbWidth, int fbHeight, Color clearColor) {
  fbW_ = fbWidth > 0 ? fbWidth : 1;
  fbH_ = fbHeight > 0 ? fbHeight : 1;
  data_.clear();
  glViewport(0, 0, fbW_, fbH_);
  glClearColor(clearColor.r, clearColor.g, clearColor.b, clearColor.a);
  glClear(GL_COLOR_BUFFER_BIT);
}

void Batcher::setWorldView(float centerX, float centerY, float zoom) {
  cx_ = centerX;
  cy_ = centerY;
  zoom_ = zoom;
  yDown_ = false;
}

void Batcher::setScreenView() {
  cx_ = static_cast<float>(fbW_) * 0.5F;
  cy_ = static_cast<float>(fbH_) * 0.5F;
  zoom_ = 1.0F;
  yDown_ = true;
}

void Batcher::push(float x, float y, float w, float h, float shape, Color c) {
  const float clipX = (x - cx_) * zoom_ / (static_cast<float>(fbW_) * 0.5F);
  const float unitY = yDown_ ? (cy_ - y) : (y - cy_);
  const float clipY = unitY * zoom_ / (static_cast<float>(fbH_) * 0.5F);
  const float halfClipX = (w * 0.5F) * zoom_ / (static_cast<float>(fbW_) * 0.5F);
  const float halfClipY = (h * 0.5F) * zoom_ / (static_cast<float>(fbH_) * 0.5F);

  float* d = data_.data(); // push_back below keeps this valid via growth check
  (void)d;
  const float vals[9] = {clipX, clipY, halfClipX, halfClipY,
                         c.r,   c.g,   c.b,       c.a, shape};
  data_.insert(data_.end(), vals, vals + 9);
}

void Batcher::rect(float x, float y, float w, float h, Color c) {
  push(x, y, w, h, 0.0F, c);
}

void Batcher::rectTopLeft(float x, float y, float w, float h, Color c) {
  push(x + w * 0.5F, y + h * 0.5F, w, h, 0.0F, c);
}

void Batcher::circle(float x, float y, float radius, Color c) {
  push(x, y, radius * 2.0F, radius * 2.0F, 1.0F, c);
}

void Batcher::text(float x, float y, float scale, Color c, std::string_view str) {
  float penX = x;
  for (const char ch : str) {
    if (ch == '\n') {
      penX = x;
      y += 8.0F * scale;
      continue;
    }
    const Glyph& g = glyphFor(ch);
    for (int row = 0; row < 7; ++row) {
      const std::uint8_t bits = g.rows[row];
      for (int col = 0; col < 5; ++col) {
        if (bits & (1u << (4 - col))) {
          rectTopLeft(penX + static_cast<float>(col) * scale,
                      y + static_cast<float>(row) * scale, scale, scale, c);
        }
      }
    }
    penX += 6.0F * scale;
  }
}

float Batcher::textWidth(float scale, std::string_view str) const {
  return static_cast<float>(str.size()) * 6.0F * scale;
}

void Batcher::flush() {
  if (data_.empty()) {
    return;
  }
  const GLsizeiptr bytes = static_cast<GLsizeiptr>(data_.size() * sizeof(float));
  glBindBuffer(GL_ARRAY_BUFFER, instVbo_);
  glBufferData(GL_ARRAY_BUFFER, bytes, nullptr, GL_STREAM_DRAW); // orphan
  glBufferSubData(GL_ARRAY_BUFFER, 0, bytes, data_.data());

  glUseProgram(program_);
  glUniform2f(uViewHalf_, static_cast<float>(fbW_) / (2.0F * zoom_),
              static_cast<float>(fbH_) / (2.0F * zoom_));
  glBindVertexArray(vao_);
  glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4,
                        static_cast<GLsizei>(data_.size() / 9));
  glBindVertexArray(0);

  data_.clear();
}

} // namespace core::render
