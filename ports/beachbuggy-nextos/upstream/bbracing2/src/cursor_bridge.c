#include "cursor_bridge.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <GLES2/gl2.h>
#include "nxgl_gles2.h"

#include "util.h"

#define CURSOR_HIDE_NS (UINT64_C(2500) * UINT64_C(1000000))
#define CURSOR_DEADZONE 0.22f
#define CURSOR_ACTIVATE_ZONE 0.27f
#define CURSOR_RESPONSE_HZ 14.0f
#define CURSOR_MAX_DT 0.050f
#define CURSOR_FINGER_ID ((SDL_FingerID)0x4e584242)
#define CURSOR_TOUCH_ID ((SDL_TouchID)0x4e58)

enum CursorClickSource {
  CURSOR_CLICK_R3 = 1u << 0,
  CURSOR_CLICK_R2 = 1u << 1,
};

typedef struct CursorState {
  Sint16 raw_x;
  Sint16 raw_y;
  float x;
  float y;
  float velocity_x;
  float velocity_y;
  Uint64 last_frame_ns;
  Uint64 visible_until_ns;
  uint32_t click_sources;
  uint32_t button_down_sources;
  bool r2_click_down;
  bool enabled;
  bool initialized;
  SDL_Window *window;
  GLuint program;
  GLint position_location;
  GLint color_location;
  bool renderer_failed;
} CursorState;

static CursorState g_cursor = {
  .x = 0.5f,
  .y = 0.5f,
  .enabled = true,
};

static bool cursor_trace_enabled(void) {
  static int enabled = -1;
  if (enabled < 0) {
    const char *value = getenv("BB_CURSOR_TRACE");
    enabled = value && *value && *value != '0';
  }
  return enabled != 0;
}

static void cursor_init_once(void) {
  if (g_cursor.initialized) return;
  g_cursor.initialized = true;
  const char *value = getenv("BB_CURSOR");
  if (value && (!strcmp(value, "0") || !strcmp(value, "false") ||
                !strcmp(value, "off"))) {
    g_cursor.enabled = false;
  }
}

static float axis_normalized(Sint16 value) {
  if (value < 0) return (float)value / 32768.0f;
  return (float)value / 32767.0f;
}

static bool cursor_visible(Uint64 now) {
  return g_cursor.enabled && g_cursor.visible_until_ns != 0 &&
         now < g_cursor.visible_until_ns;
}

static bool cursor_pressed(void) {
  return g_cursor.click_sources != 0;
}

static void push_finger_event(SDL_EventType type) {
  SDL_Event event;
  memset(&event, 0, sizeof(event));
  event.tfinger.type = type;
  event.tfinger.timestamp = SDL_GetTicksNS();
  event.tfinger.touchID = CURSOR_TOUCH_ID;
  event.tfinger.fingerID = CURSOR_FINGER_ID;
  event.tfinger.x = g_cursor.x;
  event.tfinger.y = g_cursor.y;
  event.tfinger.dx = 0.0f;
  event.tfinger.dy = 0.0f;
  /* The validated Linux direct-touch path reports pressure 1 on DOWN and UP. */
  event.tfinger.pressure = 1.0f;
  event.tfinger.windowID = g_cursor.window ? SDL_GetWindowID(g_cursor.window) : 0;
  if (!SDL_PushEvent(&event)) {
    logPrintf("[cursor] SDL_PushEvent(0x%x) failed: %s\n", type,
              SDL_GetError());
  } else if (cursor_trace_enabled()) {
    logPrintf("[cursor] finger %s x=%.3f y=%.3f\n",
              type == SDL_EVENT_FINGER_DOWN ? "DOWN" : "UP",
              g_cursor.x, g_cursor.y);
  }
}

void cursor_bridge_track_axis(SDL_GamepadAxis axis, Sint16 value) {
  cursor_init_once();
  if (!g_cursor.enabled) return;
  if (axis == SDL_GAMEPAD_AXIS_RIGHTX) {
    g_cursor.raw_x = value;
  } else if (axis == SDL_GAMEPAD_AXIS_RIGHTY) {
    g_cursor.raw_y = value;
  } else if (axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) {
    const bool was_pressed = g_cursor.r2_click_down;
    const bool pressed = g_cursor.r2_click_down ? value > 4096 : value >= 8192;
    g_cursor.r2_click_down = pressed;
    if (pressed == was_pressed) return;
    const uint32_t before = g_cursor.click_sources;
    if (pressed) {
      if (!(before & CURSOR_CLICK_R2) && cursor_visible(SDL_GetTicksNS()))
        g_cursor.click_sources |= CURSOR_CLICK_R2;
    } else {
      g_cursor.click_sources &= ~CURSOR_CLICK_R2;
    }
    if (before == 0 && g_cursor.click_sources != 0) {
      push_finger_event(SDL_EVENT_FINGER_DOWN);
    } else if (before != 0 && g_cursor.click_sources == 0) {
      push_finger_event(SDL_EVENT_FINGER_UP);
    }
  }
}

void cursor_bridge_track_button(SDL_GamepadButton button, bool pressed) {
  cursor_init_once();
  if (!g_cursor.enabled) return;

  uint32_t source = 0;
  if (button == SDL_GAMEPAD_BUTTON_RIGHT_STICK) {
    source = CURSOR_CLICK_R3;
  } else {
    return;
  }

  const Uint64 now = SDL_GetTicksNS();
  const bool was_pressed = (g_cursor.button_down_sources & source) != 0;
  if (pressed == was_pressed) return;
  if (pressed) g_cursor.button_down_sources |= source;
  else g_cursor.button_down_sources &= ~source;

  const uint32_t before = g_cursor.click_sources;
  if (pressed) {
    /* R3 remains native unless the player deliberately activated the fallback
     * with the right stick. This keeps gameplay actions intact. */
    if (!cursor_visible(now)) return;
    g_cursor.click_sources |= source;
    g_cursor.visible_until_ns = now + CURSOR_HIDE_NS;
  } else {
    if (!(before & source)) return;
    g_cursor.click_sources &= ~source;
    g_cursor.visible_until_ns = now + CURSOR_HIDE_NS;
  }
  if (before == 0 && g_cursor.click_sources != 0) {
    push_finger_event(SDL_EVENT_FINGER_DOWN);
  } else if (before != 0 && g_cursor.click_sources == 0) {
    push_finger_event(SDL_EVENT_FINGER_UP);
  }
}

void cursor_bridge_handle_event(const SDL_Event *event) {
  if (!event) return;
  if (event->type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
    cursor_bridge_track_axis((SDL_GamepadAxis)event->gaxis.axis,
                             event->gaxis.value);
  } else if (event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ||
             event->type == SDL_EVENT_GAMEPAD_BUTTON_UP) {
    cursor_bridge_track_button((SDL_GamepadButton)event->gbutton.button,
                               event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
  } else if (event->type == SDL_EVENT_GAMEPAD_REMOVED) {
    g_cursor.raw_x = 0;
    g_cursor.raw_y = 0;
    g_cursor.velocity_x = 0.0f;
    g_cursor.velocity_y = 0.0f;
    if (cursor_pressed()) {
      push_finger_event(SDL_EVENT_FINGER_UP);
      g_cursor.click_sources = 0;
    }
    g_cursor.button_down_sources = 0;
    g_cursor.r2_click_down = false;
  }
}

static GLuint compile_shader(GLenum type, const char *source) {
  GLuint shader = glCreateShader(type);
  if (!shader) return 0;
  glShaderSource(shader, 1, &source, NULL);
  glCompileShader(shader);
  GLint ok = GL_FALSE;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char message[512] = {0};
    glGetShaderInfoLog(shader, sizeof(message), NULL, message);
    logPrintf("[cursor] shader compile failed: %s\n", message);
    glDeleteShader(shader);
    return 0;
  }
  return shader;
}

static bool create_renderer(void) {
  static const char vertex_source[] =
      "attribute vec2 a_position;\n"
      "uniform vec4 u_color;\n"
      "varying vec4 v_color;\n"
      "void main() { gl_Position = vec4(a_position, 0.0, 1.0);"
      " v_color = u_color; }\n";
  static const char fragment_source[] =
      "precision mediump float;\n"
      "varying vec4 v_color;\n"
      "void main() { gl_FragColor = v_color; }\n";

  GLuint vertex = compile_shader(GL_VERTEX_SHADER, vertex_source);
  GLuint fragment = compile_shader(GL_FRAGMENT_SHADER, fragment_source);
  if (!vertex || !fragment) {
    if (vertex) glDeleteShader(vertex);
    if (fragment) glDeleteShader(fragment);
    return false;
  }

  GLuint program = glCreateProgram();
  glAttachShader(program, vertex);
  glAttachShader(program, fragment);
  glBindAttribLocation(program, 0, "a_position");
  glLinkProgram(program);
  glDeleteShader(vertex);
  glDeleteShader(fragment);
  GLint ok = GL_FALSE;
  glGetProgramiv(program, GL_LINK_STATUS, &ok);
  if (!ok) {
    char message[512] = {0};
    glGetProgramInfoLog(program, sizeof(message), NULL, message);
    logPrintf("[cursor] program link failed: %s\n", message);
    glDeleteProgram(program);
    return false;
  }

  g_cursor.program = program;
  g_cursor.position_location = glGetAttribLocation(program, "a_position");
  g_cursor.color_location = glGetUniformLocation(program, "u_color");
  if (g_cursor.position_location < 0 || g_cursor.color_location < 0) {
    glDeleteProgram(program);
    g_cursor.program = 0;
    return false;
  }
  return true;
}

typedef struct GLState {
  GLint program;
  GLint framebuffer;
  GLint array_buffer;
  GLint element_array_buffer;
  GLint viewport[4];
  GLint scissor_box[4];
  GLint blend_src_rgb;
  GLint blend_dst_rgb;
  GLint blend_src_alpha;
  GLint blend_dst_alpha;
  GLint blend_equation_rgb;
  GLint blend_equation_alpha;
  GLboolean blend;
  GLboolean depth_test;
  GLboolean cull_face;
  GLboolean scissor_test;
  GLboolean stencil_test;
  GLboolean color_mask[4];
  GLboolean depth_mask;
  GLint attrib_enabled;
  GLint attrib_size;
  GLint attrib_type;
  GLint attrib_normalized;
  GLint attrib_stride;
  GLint attrib_buffer;
  void *attrib_pointer;
} GLState;

static void save_gl_state(GLState *state, GLuint attribute) {
  glGetIntegerv(GL_CURRENT_PROGRAM, &state->program);
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &state->framebuffer);
  glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &state->array_buffer);
  glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &state->element_array_buffer);
  glGetIntegerv(GL_VIEWPORT, state->viewport);
  glGetIntegerv(GL_SCISSOR_BOX, state->scissor_box);
  glGetIntegerv(GL_BLEND_SRC_RGB, &state->blend_src_rgb);
  glGetIntegerv(GL_BLEND_DST_RGB, &state->blend_dst_rgb);
  glGetIntegerv(GL_BLEND_SRC_ALPHA, &state->blend_src_alpha);
  glGetIntegerv(GL_BLEND_DST_ALPHA, &state->blend_dst_alpha);
  glGetIntegerv(GL_BLEND_EQUATION_RGB, &state->blend_equation_rgb);
  glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &state->blend_equation_alpha);
  state->blend = glIsEnabled(GL_BLEND);
  state->depth_test = glIsEnabled(GL_DEPTH_TEST);
  state->cull_face = glIsEnabled(GL_CULL_FACE);
  state->scissor_test = glIsEnabled(GL_SCISSOR_TEST);
  state->stencil_test = glIsEnabled(GL_STENCIL_TEST);
  glGetBooleanv(GL_COLOR_WRITEMASK, state->color_mask);
  glGetBooleanv(GL_DEPTH_WRITEMASK, &state->depth_mask);
  glGetVertexAttribiv(attribute, GL_VERTEX_ATTRIB_ARRAY_ENABLED,
                      &state->attrib_enabled);
  glGetVertexAttribiv(attribute, GL_VERTEX_ATTRIB_ARRAY_SIZE,
                      &state->attrib_size);
  glGetVertexAttribiv(attribute, GL_VERTEX_ATTRIB_ARRAY_TYPE,
                      &state->attrib_type);
  glGetVertexAttribiv(attribute, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED,
                      &state->attrib_normalized);
  glGetVertexAttribiv(attribute, GL_VERTEX_ATTRIB_ARRAY_STRIDE,
                      &state->attrib_stride);
  glGetVertexAttribiv(attribute, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING,
                      &state->attrib_buffer);
  glGetVertexAttribPointerv(attribute, GL_VERTEX_ATTRIB_ARRAY_POINTER,
                           &state->attrib_pointer);
}

static void set_capability(GLenum capability, GLboolean enabled) {
  if (enabled) glEnable(capability);
  else glDisable(capability);
}

static void restore_gl_state(const GLState *state, GLuint attribute) {
  glUseProgram((GLuint)state->program);
  glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)state->framebuffer);
  glBindBuffer(GL_ARRAY_BUFFER, (GLuint)state->attrib_buffer);
  glVertexAttribPointer(attribute, state->attrib_size, state->attrib_type,
                        state->attrib_normalized, state->attrib_stride,
                        state->attrib_pointer);
  if (state->attrib_enabled) glEnableVertexAttribArray(attribute);
  else glDisableVertexAttribArray(attribute);
  glBindBuffer(GL_ARRAY_BUFFER, (GLuint)state->array_buffer);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, (GLuint)state->element_array_buffer);
  glViewport(state->viewport[0], state->viewport[1], state->viewport[2],
             state->viewport[3]);
  glScissor(state->scissor_box[0], state->scissor_box[1],
            state->scissor_box[2], state->scissor_box[3]);
  glBlendFuncSeparate(state->blend_src_rgb, state->blend_dst_rgb,
                      state->blend_src_alpha, state->blend_dst_alpha);
  glBlendEquationSeparate(state->blend_equation_rgb,
                          state->blend_equation_alpha);
  set_capability(GL_BLEND, state->blend);
  set_capability(GL_DEPTH_TEST, state->depth_test);
  set_capability(GL_CULL_FACE, state->cull_face);
  set_capability(GL_SCISSOR_TEST, state->scissor_test);
  set_capability(GL_STENCIL_TEST, state->stencil_test);
  glColorMask(state->color_mask[0], state->color_mask[1],
              state->color_mask[2], state->color_mask[3]);
  glDepthMask(state->depth_mask);
}

static void make_arrow_vertices(GLfloat *vertices, float px, float py,
                                float scale, float offset_x, float offset_y,
                                int width, int height) {
  static const float shape[18] = {
      0.0f, 0.0f,  1.0f, 30.0f, 29.0f, 20.0f,
      9.0f, 18.0f, 16.5f, 17.0f, 23.0f, 35.0f,
      9.0f, 18.0f, 23.0f, 35.0f, 16.0f, 40.0f,
  };
  for (int i = 0; i < 9; ++i) {
    const float sx = px + offset_x + shape[i * 2] * scale;
    const float sy = py + offset_y + shape[i * 2 + 1] * scale;
    vertices[i * 2] = sx * 2.0f / (float)width - 1.0f;
    vertices[i * 2 + 1] = 1.0f - sy * 2.0f / (float)height;
  }
}

static void draw_arrow_pass(float px, float py, float scale, float offset_x,
                            float offset_y, int width, int height,
                            float red, float green, float blue, float alpha) {
  GLfloat vertices[18];
  make_arrow_vertices(vertices, px, py, scale, offset_x, offset_y, width,
                      height);
  glUniform4f(g_cursor.color_location, red, green, blue, alpha);
  glVertexAttribPointer((GLuint)g_cursor.position_location, 2, GL_FLOAT,
                        GL_FALSE, 0, vertices);
  glDrawArrays(GL_TRIANGLES, 0, 9);
}

static void draw_cursor(SDL_Window *window) {
  int width = 0;
  int height = 0;
  SDL_GetWindowSizeInPixels(window, &width, &height);
  if (width <= 0 || height <= 0) return;

  if (!g_cursor.program && !g_cursor.renderer_failed) {
    if (!create_renderer()) {
      g_cursor.renderer_failed = true;
      logPrintf("[cursor] renderer disabled after initialization failure\n");
      return;
    }
    logPrintf("[cursor] GLES2 arrow renderer ready (%dx%d)\n", width, height);
  }
  if (!g_cursor.program) return;

  GLState state;
  save_gl_state(&state, (GLuint)g_cursor.position_location);

  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glViewport(0, 0, width, height);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glDisable(GL_STENCIL_TEST);
  glDepthMask(GL_FALSE);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glEnable(GL_BLEND);
  /* Preserve opaque framebuffer alpha for the Amlogic OSD compositor. */
  glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
  glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
  glUseProgram(g_cursor.program);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
  glEnableVertexAttribArray((GLuint)g_cursor.position_location);

  const float px = g_cursor.x * (float)width;
  const float py = g_cursor.y * (float)height;
  draw_arrow_pass(px, py, 1.10f, 3.0f, 4.0f, width, height,
                  0.0f, 0.0f, 0.0f, 0.42f);
  draw_arrow_pass(px, py, 1.12f, -1.8f, -1.8f, width, height,
                  0.03f, 0.04f, 0.06f, 0.96f);
  if (cursor_pressed()) {
    draw_arrow_pass(px, py, 1.0f, 0.0f, 0.0f, width, height,
                    1.0f, 0.78f, 0.18f, 1.0f);
  } else {
    draw_arrow_pass(px, py, 1.0f, 0.0f, 0.0f, width, height,
                    0.94f, 0.97f, 1.0f, 1.0f);
  }

  restore_gl_state(&state, (GLuint)g_cursor.position_location);
}

void cursor_bridge_frame(SDL_Window *window) {
  cursor_init_once();
  if (!g_cursor.enabled || !window) return;
  g_cursor.window = window;

  const Uint64 now = SDL_GetTicksNS();
  float dt = 1.0f / 60.0f;
  if (g_cursor.last_frame_ns != 0 && now > g_cursor.last_frame_ns) {
    dt = (float)(now - g_cursor.last_frame_ns) / 1000000000.0f;
  }
  g_cursor.last_frame_ns = now;
  if (dt > CURSOR_MAX_DT) dt = CURSOR_MAX_DT;
  if (dt < 1.0f / 240.0f) dt = 1.0f / 240.0f;

  const float raw_x = axis_normalized(g_cursor.raw_x);
  const float raw_y = axis_normalized(g_cursor.raw_y);
  const float magnitude = sqrtf(raw_x * raw_x + raw_y * raw_y);
  float target_x = 0.0f;
  float target_y = 0.0f;
  if (magnitude > CURSOR_DEADZONE) {
    const float unit_x = raw_x / magnitude;
    const float unit_y = raw_y / magnitude;
    float response = (magnitude - CURSOR_DEADZONE) /
                     (1.0f - CURSOR_DEADZONE);
    if (response > 1.0f) response = 1.0f;
    /* Progressive response: precise near center, one screen/second at full. */
    const float speed = 0.16f * response + 0.94f * response * response;
    target_x = unit_x * speed;
    target_y = unit_y * speed;
    if (magnitude > CURSOR_ACTIVATE_ZONE) {
      g_cursor.visible_until_ns = now + CURSOR_HIDE_NS;
    }
  }

  const float smoothing = 1.0f - expf(-CURSOR_RESPONSE_HZ * dt);
  g_cursor.velocity_x += (target_x - g_cursor.velocity_x) * smoothing;
  g_cursor.velocity_y += (target_y - g_cursor.velocity_y) * smoothing;
  g_cursor.x += g_cursor.velocity_x * dt;
  g_cursor.y += g_cursor.velocity_y * dt;
  if (g_cursor.x < 0.0f) g_cursor.x = 0.0f;
  else if (g_cursor.x > 1.0f) g_cursor.x = 1.0f;
  if (g_cursor.y < 0.0f) g_cursor.y = 0.0f;
  else if (g_cursor.y > 1.0f) g_cursor.y = 1.0f;

  if (cursor_pressed()) g_cursor.visible_until_ns = now + CURSOR_HIDE_NS;
  if (cursor_visible(now)) draw_cursor(window);
}
