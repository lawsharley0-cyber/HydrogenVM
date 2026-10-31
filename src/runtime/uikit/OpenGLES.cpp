// HydrogenVM — OpenGL ES 1.1 & 2.0, EAGLContext, EAGLSharegroup, and CAEAGLLayer.
//
// Provides a complete OpenGL ES 1.1 (fixed-function + OES extensions incl.
// OES_framebuffer_object and OES_draw_texture) and OpenGL ES 2.0 (shaders,
// programs, uniforms, vertex attributes, VBOs, FBOs, textures) state tracker
// and software rasterizer wired into CAEAGLLayer via LayerData::externalContents.
#include "runtime/gl/HostGL.hpp"
#include "UIKit.hpp"
#include "runtime/graphics/Pvrtc.hpp"
#include "runtime/graphics/Shader.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace Hydrogen::Runtime::UIKit {

using Foundation::autoreleased;
using Foundation::makeString;
using Foundation::utf8;

namespace {

// ================================================================ GL Enums ===

enum : uint32_t {
    GL_NO_ERROR = 0,
    GL_ZERO = 0,
    GL_ONE = 1,
    GL_POINTS = 0x0000,
    GL_LINES = 0x0001,
    GL_LINE_LOOP = 0x0002,
    GL_LINE_STRIP = 0x0003,
    GL_TRIANGLES = 0x0004,
    GL_TRIANGLE_STRIP = 0x0005,
    GL_TRIANGLE_FAN = 0x0006,

    GL_DEPTH_BUFFER_BIT = 0x00000100,
    GL_STENCIL_BUFFER_BIT = 0x00000400,
    GL_COLOR_BUFFER_BIT = 0x00004000,

    GL_NEVER = 0x0200,
    GL_LESS = 0x0201,
    GL_EQUAL = 0x0202,
    GL_LEQUAL = 0x0203,
    GL_GREATER = 0x0204,
    GL_NOTEQUAL = 0x0205,
    GL_GEQUAL = 0x0206,
    GL_ALWAYS = 0x0207,

    GL_SRC_COLOR = 0x0300,
    GL_ONE_MINUS_SRC_COLOR = 0x0301,
    GL_SRC_ALPHA = 0x0302,
    GL_ONE_MINUS_SRC_ALPHA = 0x0303,
    GL_DST_ALPHA = 0x0304,
    GL_ONE_MINUS_DST_ALPHA = 0x0305,
    GL_DST_COLOR = 0x0306,
    GL_ONE_MINUS_DST_COLOR = 0x0307,
    GL_SRC_ALPHA_SATURATE = 0x0308,

    GL_FRONT = 0x0404,
    GL_BACK = 0x0405,
    GL_FRONT_AND_BACK = 0x0408,

    GL_INVALID_ENUM = 0x0500,
    GL_INVALID_VALUE = 0x0501,
    GL_INVALID_OPERATION = 0x0502,
    GL_OUT_OF_MEMORY = 0x0505,
    GL_INVALID_FRAMEBUFFER_OPERATION = 0x0506,

    GL_CW = 0x0900,
    GL_CCW = 0x0901,

    GL_CULL_FACE = 0x0B44,
    GL_DEPTH_TEST = 0x0B71,
    GL_STENCIL_TEST = 0x0B90,
    GL_ALPHA_TEST = 0x0BC0,
    GL_BLEND = 0x0BE2,
    GL_SCISSOR_TEST = 0x0C11,
    GL_TEXTURE_2D = 0x0DE1,

    GL_UNPACK_ALIGNMENT = 0x0CF5,
    GL_PACK_ALIGNMENT = 0x0D05,
    GL_MAX_TEXTURE_SIZE = 0x0D33,
    GL_MAX_VIEWPORT_DIMS = 0x0D3A,
    GL_SUBPIXEL_BITS = 0x0D50,
    GL_RED_BITS = 0x0D52,
    GL_GREEN_BITS = 0x0D53,
    GL_BLUE_BITS = 0x0D54,
    GL_ALPHA_BITS = 0x0D55,
    GL_DEPTH_BITS = 0x0D56,
    GL_STENCIL_BITS = 0x0D57,
    GL_VIEWPORT = 0x0BA2,
    GL_SCISSOR_BOX = 0x0C10,
    GL_COLOR_CLEAR_VALUE = 0x0C22,
    GL_MODELVIEW_MATRIX = 0x0BA6,
    GL_PROJECTION_MATRIX = 0x0BA7,
    GL_TEXTURE_MATRIX = 0x0BA8,

    GL_BYTE = 0x1400,
    GL_UNSIGNED_BYTE = 0x1401,
    GL_SHORT = 0x1402,
    GL_UNSIGNED_SHORT = 0x1403,
    GL_INT = 0x1404,
    GL_UNSIGNED_INT = 0x1405,
    GL_FLOAT = 0x1406,
    GL_FIXED = 0x140C,

    GL_MODELVIEW = 0x1700,
    GL_PROJECTION = 0x1701,
    GL_TEXTURE = 0x1702,

    GL_ALPHA = 0x1906,
    GL_RGB = 0x1907,
    GL_RGBA = 0x1908,
    GL_LUMINANCE = 0x1909,
    GL_LUMINANCE_ALPHA = 0x190A,
    GL_BGRA_EXT = 0x80E1,

    GL_VENDOR = 0x1F00,
    GL_RENDERER = 0x1F01,
    GL_VERSION = 0x1F02,
    GL_EXTENSIONS = 0x1F03,
    GL_SHADING_LANGUAGE_VERSION = 0x8B8C,

    GL_MODULATE = 0x2100,
    GL_DECAL = 0x2101,
    GL_REPLACE = 0x1E01,
    GL_ADD = 0x0104,

    GL_NEAREST = 0x2600,
    GL_LINEAR = 0x2601,
    GL_NEAREST_MIPMAP_NEAREST = 0x2700,
    GL_LINEAR_MIPMAP_NEAREST = 0x2701,
    GL_NEAREST_MIPMAP_LINEAR = 0x2702,
    GL_LINEAR_MIPMAP_LINEAR = 0x2703,
    GL_TEXTURE_MAG_FILTER = 0x2800,
    GL_TEXTURE_MIN_FILTER = 0x2801,
    GL_TEXTURE_WRAP_S = 0x2802,
    GL_TEXTURE_WRAP_T = 0x2803,
    GL_REPEAT = 0x2901,
    GL_CLAMP_TO_EDGE = 0x812F,
    GL_MIRRORED_REPEAT = 0x8370,

    GL_UNSIGNED_SHORT_4_4_4_4 = 0x8033,
    GL_UNSIGNED_SHORT_5_5_5_1 = 0x8034,
    GL_UNSIGNED_SHORT_5_6_5 = 0x8363,

    GL_VERTEX_ARRAY = 0x8074,
    GL_NORMAL_ARRAY = 0x8075,
    GL_COLOR_ARRAY = 0x8076,
    GL_TEXTURE_COORD_ARRAY = 0x8078,
    GL_POINT_SIZE_ARRAY_OES = 0x8B9C,

    GL_FUNC_ADD = 0x8006,
    GL_FUNC_SUBTRACT = 0x800A,
    GL_FUNC_REVERSE_SUBTRACT = 0x800B,

    GL_TEXTURE0 = 0x84C0,
    GL_ACTIVE_TEXTURE = 0x84E0,
    GL_MAX_TEXTURE_UNITS = 0x84E2,
    GL_MAX_VERTEX_ATTRIBS = 0x8869,
    GL_MAX_TEXTURE_IMAGE_UNITS = 0x8872,
    GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS = 0x8B4D,
    GL_MAX_VERTEX_UNIFORM_VECTORS = 0x8DFB,
    GL_MAX_VARYING_VECTORS = 0x8DFC,
    GL_MAX_FRAGMENT_UNIFORM_VECTORS = 0x8DFD,
    GL_MAX_RENDERBUFFER_SIZE = 0x84E8,

    GL_ARRAY_BUFFER = 0x8892,
    GL_ELEMENT_ARRAY_BUFFER = 0x8893,
    GL_ARRAY_BUFFER_BINDING = 0x8894,
    GL_ELEMENT_ARRAY_BUFFER_BINDING = 0x8895,
    GL_BUFFER_SIZE = 0x8764,
    GL_BUFFER_USAGE = 0x8765,
    GL_STATIC_DRAW = 0x88E4,

    GL_FRAGMENT_SHADER = 0x8B30,
    GL_VERTEX_SHADER = 0x8B31,
    GL_FLOAT_VEC2 = 0x8B50,
    GL_FLOAT_VEC3 = 0x8B51,
    GL_FLOAT_VEC4 = 0x8B52,
    GL_INT_VEC2 = 0x8B53,
    GL_INT_VEC3 = 0x8B54,
    GL_INT_VEC4 = 0x8B55,
    GL_BOOL = 0x8B56,
    GL_BOOL_VEC2 = 0x8B57,
    GL_BOOL_VEC3 = 0x8B58,
    GL_BOOL_VEC4 = 0x8B59,
    GL_FLOAT_MAT2 = 0x8B5A,
    GL_FLOAT_MAT3 = 0x8B5B,
    GL_FLOAT_MAT4 = 0x8B5C,
    GL_SAMPLER_2D = 0x8B5E,
    GL_SAMPLER_CUBE = 0x8B60,
    GL_COMPILE_STATUS = 0x8B81,
    GL_LINK_STATUS = 0x8B82,
    GL_VALIDATE_STATUS = 0x8B83,
    GL_INFO_LOG_LENGTH = 0x8B84,
    GL_ATTACHED_SHADERS = 0x8B85,
    GL_ACTIVE_UNIFORMS = 0x8B86,
    GL_ACTIVE_UNIFORM_MAX_LENGTH = 0x8B87,
    GL_SHADER_SOURCE_LENGTH = 0x8B88,
    GL_ACTIVE_ATTRIBUTES = 0x8B89,
    GL_ACTIVE_ATTRIBUTE_MAX_LENGTH = 0x8B8A,
    GL_CURRENT_PROGRAM = 0x8B8D,

    GL_FRAMEBUFFER = 0x8D40,
    GL_RENDERBUFFER = 0x8D41,
    GL_RENDERBUFFER_WIDTH = 0x8D42,
    GL_RENDERBUFFER_HEIGHT = 0x8D43,
    GL_RENDERBUFFER_INTERNAL_FORMAT = 0x8D44,
    GL_RENDERBUFFER_RED_SIZE = 0x8D50,
    GL_RENDERBUFFER_GREEN_SIZE = 0x8D51,
    GL_RENDERBUFFER_BLUE_SIZE = 0x8D52,
    GL_RENDERBUFFER_ALPHA_SIZE = 0x8D53,
    GL_RENDERBUFFER_DEPTH_SIZE = 0x8D54,
    GL_RENDERBUFFER_STENCIL_SIZE = 0x8D55,
    GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE = 0x8CD0,
    GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME = 0x8CD1,
    GL_COLOR_ATTACHMENT0 = 0x8CE0,
    GL_DEPTH_ATTACHMENT = 0x8D00,
    GL_STENCIL_ATTACHMENT = 0x8D20,
    GL_FRAMEBUFFER_COMPLETE = 0x8CD5,
    GL_FRAMEBUFFER_BINDING = 0x8CA6,
    GL_RENDERBUFFER_BINDING = 0x8CA7,
    GL_RGBA4 = 0x8056,
    GL_RGB5_A1 = 0x8057,
    GL_RGB565 = 0x8D62,
    GL_DEPTH_COMPONENT16 = 0x81A5,
    GL_DEPTH_COMPONENT24_OES = 0x81A6,
    GL_STENCIL_INDEX8 = 0x8D48,
    GL_DEPTH24_STENCIL8_OES = 0x88F0,
    GL_INVERT = 0x150A,
    GL_KEEP = 0x1E00,
    GL_INCR = 0x1E02,
    GL_DECR = 0x1E03,
    GL_INCR_WRAP = 0x8507,
    GL_DECR_WRAP = 0x8508,
    GL_RGBA8_OES = 0x8058,
    GL_RGB8_OES = 0x8051,
    GL_TEXTURE_CROP_RECT_OES = 0x8B9D,
};

// =============================================================== Math / 4x4 ===

struct Vec4 {
    float x = 0, y = 0, z = 0, w = 1;
    float& operator[](int i) { return (&x)[i]; }
    float operator[](int i) const { return (&x)[i]; }
};

// Column-major 4x4 matrix (OpenGL standard: m[col*4 + row]).
struct Mat4 {
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    static Mat4 identity() { return {}; }
    Mat4 operator*(const Mat4& b) const {
        Mat4 r{};
        for (int c = 0; c < 4; ++c) {
            for (int row = 0; row < 4; ++row) {
                r.m[c * 4 + row] = m[0 * 4 + row] * b.m[c * 4 + 0] +
                                   m[1 * 4 + row] * b.m[c * 4 + 1] +
                                   m[2 * 4 + row] * b.m[c * 4 + 2] +
                                   m[3 * 4 + row] * b.m[c * 4 + 3];
            }
        }
        return r;
    }
    Vec4 apply(const Vec4& v) const {
        return {
            m[0] * v.x + m[4] * v.y + m[8]  * v.z + m[12] * v.w,
            m[1] * v.x + m[5] * v.y + m[9]  * v.z + m[13] * v.w,
            m[2] * v.x + m[6] * v.y + m[10] * v.z + m[14] * v.w,
            m[3] * v.x + m[7] * v.y + m[11] * v.z + m[15] * v.w
        };
    }
};

// =================================================== GL Objects & Resources ===

struct EAGLLayerData : ObjC::HostObject {
    id drawableProperties = 0;   // retained NSDictionary
    G::ImagePtr frontBuffer;     // most recently presented frame (top-left premultiplied RGBA)
};

struct Renderbuffer {
    uint32_t id = 0;
    int width = 0, height = 0;
    uint32_t internalFormat = GL_RGBA8_OES;
    std::vector<uint8_t> rgba;   // bottom-left origin, straight RGBA (width * height * 4)
    std::vector<float> depth;    // width * height
    std::vector<uint8_t> stencil;
    ObjC::id drawableLayer = 0;  // CAEAGLLayer bound via renderbufferStorage:fromDrawable:
};

struct Framebuffer {
    uint32_t id = 0;
    uint32_t colorAttachmentType = 0;   // GL_RENDERBUFFER or GL_TEXTURE_2D
    uint32_t colorAttachmentId = 0;
    uint32_t depthAttachmentType = 0;
    uint32_t depthAttachmentId = 0;
    uint32_t stencilAttachmentType = 0;
    uint32_t stencilAttachmentId = 0;
};

struct BufferObject {
    uint32_t id = 0;
    uint32_t usage = GL_STATIC_DRAW;
    std::vector<uint8_t> data;
    uint32_t mappedGuestAddr = 0;
};

struct Texture2D {
    uint32_t id = 0;
    int width = 0, height = 0;
    uint32_t format = GL_RGBA;
    std::vector<uint8_t> rgba;   // straight RGBA, width * height * 4
    uint32_t minFilter = GL_LINEAR;
    uint32_t magFilter = GL_LINEAR;
    uint32_t wrapS = GL_REPEAT;
    uint32_t wrapT = GL_REPEAT;
    int cropRect[4] = {0, 0, 0, 0}; // GL_TEXTURE_CROP_RECT_OES: u, v, w, h

    // Texel index along one axis, wrapped as OpenGL does (in texel space).
    static int wrapTexel(int i, int n, uint32_t mode) {
        if (mode == GL_CLAMP_TO_EDGE) return std::clamp(i, 0, n - 1);
        if (mode == GL_MIRRORED_REPEAT) {
            const int period = 2 * n;
            int m = i % period;
            if (m < 0) m += period;
            return m < n ? m : period - 1 - m;
        }
        int m = i % n;
        return m < 0 ? m + n : m;
    }
    const uint8_t* texel(int x, int y) const { return &rgba[(size_t(y) * size_t(width) + size_t(x)) * 4]; }

    // GL_NEAREST picks texel floor(u * width); GL_LINEAR blends the four texels
    // around u * width - 0.5. (No mipmaps: minification uses the same filter.)
    Vec4 sample(float u, float v) const {
        if (width <= 0 || height <= 0 || rgba.empty()) return {1, 1, 1, 1};
        if (!std::isfinite(u) || !std::isfinite(v)) u = v = 0;
        const float tx = u * float(width), ty = v * float(height);
        if (magFilter == GL_NEAREST) {
            const uint8_t* p = texel(wrapTexel(int(std::floor(tx)), width, wrapS), wrapTexel(int(std::floor(ty)), height, wrapT));
            return {p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, p[3] / 255.0f};
        }
        const float fx = tx - 0.5f, fy = ty - 0.5f;
        const float x0f = std::floor(fx), y0f = std::floor(fy);
        const float ax = fx - x0f, ay = fy - y0f;
        const int x0 = wrapTexel(int(x0f), width, wrapS), x1 = wrapTexel(int(x0f) + 1, width, wrapS);
        const int y0 = wrapTexel(int(y0f), height, wrapT), y1 = wrapTexel(int(y0f) + 1, height, wrapT);
        const uint8_t *a = texel(x0, y0), *b = texel(x1, y0), *c = texel(x0, y1), *d = texel(x1, y1);
        const float w00 = (1 - ax) * (1 - ay), w10 = ax * (1 - ay), w01 = (1 - ax) * ay, w11 = ax * ay;
        constexpr float k = 1.0f / 255.0f;
        return {(a[0] * w00 + b[0] * w10 + c[0] * w01 + d[0] * w11) * k, (a[1] * w00 + b[1] * w10 + c[1] * w01 + d[1] * w11) * k,
                (a[2] * w00 + b[2] * w10 + c[2] * w01 + d[2] * w11) * k, (a[3] * w00 + b[3] * w10 + c[3] * w01 + d[3] * w11) * k};
    }
};

struct ShaderObject {
    uint32_t id = 0;
    uint32_t type = GL_VERTEX_SHADER;
    std::string source;
    bool compiled = false;
    std::string infoLog;
    Shader::Program executable;
};

struct ShaderVar {
    std::string name;
    uint32_t type = GL_FLOAT_VEC4;
    int size = 1;
    int location = 0;
    bool array = false;
};

struct UniformValue {
    uint32_t type = GL_FLOAT_VEC4;
    float f[16] = {};
    int i[4] = {0, 0, 0, 0};
    int span = 1; // elements that glUniform*v may write starting at this location
};

struct ProgramObject {
    struct Binding { int slot, location, width; bool sampler; bool memory = false; };
    struct Varying { int vertexSlot, fragmentSlot, width, offset; };
    uint32_t id = 0;
    std::vector<uint32_t> shaders;
    bool linked = false;
    bool pendingDelete = false;
    std::string infoLog;
    std::map<std::string, int> boundAttribLocations;
    std::vector<ShaderVar> attribs;
    std::vector<ShaderVar> uniforms;
    std::unordered_map<int, UniformValue> uniformValues;
    Shader::Program vertex, fragment;
    std::vector<Binding> vertexInputs, vertexUniforms, fragmentUniforms;
    std::vector<Varying> varyings;
    int varyingWidth = 0;
    bool budgetLogged = false;
};

struct SharegroupData : ObjC::HostObject {
    uint32_t nextBuffer = 1, nextTexture = 1, nextRenderbuffer = 1, nextFramebuffer = 1, nextShader = 1, nextProgram = 1;
    std::unordered_map<uint32_t, BufferObject> buffers;
    std::unordered_map<uint32_t, Texture2D> textures;
    std::unordered_map<uint32_t, Renderbuffer> renderbuffers;
    std::unordered_map<uint32_t, Framebuffer> framebuffers;
    std::unordered_map<uint32_t, ShaderObject> shaders;
    std::mutex programMu; // shared contexts may create, bind and draw concurrently
    std::unordered_map<uint32_t, ProgramObject> programs;
};

struct ArrayPointer {
    bool enabled = false;
    int size = 4;
    uint32_t type = GL_FLOAT;
    bool normalized = false;
    int stride = 0;
    uint32_t ptr = 0;       // guest pointer or buffer offset
    uint32_t buffer = 0;    // bound GL_ARRAY_BUFFER at the time of *Pointer call
};

struct VaoState {
    uint32_t elementArrayBuffer = 0;
    ArrayPointer vertexPtr;
    ArrayPointer colorPtr;
    ArrayPointer normalPtr;
    ArrayPointer texCoordPtr[8];
    ArrayPointer pointSizePtr;
    ArrayPointer attribPtr[16];
    
    VaoState() {
        for (int i = 0; i < 16; ++i) attribPtr[i] = ArrayPointer();
    }
};

struct ContextData : ObjC::HostObject {
    uint32_t api = 2;
    ObjC::id sharegroup = 0;   // retained EAGLSharegroup
    uint32_t lastError = GL_NO_ERROR;

    // Bindings
    uint32_t arrayBuffer = 0;
    uint32_t framebuffer = 0;        // draw framebuffer (GL_FRAMEBUFFER / GL_DRAW_FRAMEBUFFER_APPLE)
    uint32_t readFramebuffer = 0;    // GL_READ_FRAMEBUFFER_APPLE (multisample resolve source)
    uint32_t renderbuffer = 0;
    uint32_t activeTexture = 0;           // 0..7
    uint32_t clientActiveTexture = 0;     // 0..7
    uint32_t boundTextures[8] = {};
    uint32_t currentProgram = 0;
    uint32_t boundVao = 0;
    VaoState defaultVao;
    std::unordered_map<uint32_t, VaoState> vaos;

    // Viewport & Scissor
    int viewport[4] = {0, 0, 320, 480};
    int scissor[4] = {0, 0, 320, 480};
    float depthRange[2] = {0.0f, 1.0f};

    // Clear state
    float clearColor[4] = {0, 0, 0, 0};
    float clearDepth = 1.0f;
    int clearStencil = 0;

    // Capabilities & Raster state
    bool blend = false;
    bool depthTest = false;
    bool scissorTest = false;
    bool cullFace = false;
    bool alphaTest = false;
    bool stencilTest = false;
    bool texture2DEnabled[8] = {};
    bool depthMask = true;
    bool colorMask[4] = {true, true, true, true};

    uint32_t blendSrcRGB = GL_ONE, blendDstRGB = GL_ZERO;
    uint32_t blendSrcAlpha = GL_ONE, blendDstAlpha = GL_ZERO;
    uint32_t blendEqRGB = GL_FUNC_ADD, blendEqAlpha = GL_FUNC_ADD;
    float blendColor[4] = {0, 0, 0, 0};
    uint32_t depthFunc = GL_LESS;
    struct StencilFace {
        uint32_t func = GL_ALWAYS;
        int ref = 0;
        uint32_t mask = 0xFF, writeMask = 0xFF;
        uint32_t fail = GL_KEEP, zfail = GL_KEEP, zpass = GL_KEEP;
    } stencilFront, stencilBack;
    uint32_t cullFaceMode = GL_BACK;
    uint32_t frontFace = GL_CCW;
    uint32_t alphaFunc = GL_ALWAYS;
    float alphaRef = 0.0f;
    int unpackAlignment = 4, packAlignment = 4;
    float lineWidth = 1.0f, pointSize = 1.0f;

    // GLES 1.1 Fixed-Function state
    uint32_t matrixMode = GL_MODELVIEW;
    std::vector<Mat4> modelviewStack{Mat4::identity()};
    std::vector<Mat4> projectionStack{Mat4::identity()};
    std::vector<Mat4> textureStack{Mat4::identity()};
    Vec4 currentColor{1, 1, 1, 1};
    Vec4 currentNormal{0, 0, 1, 0};
    Vec4 currentTexCoord[8];
    uint32_t texEnvMode[8] = {GL_MODULATE, GL_MODULATE, GL_MODULATE, GL_MODULATE, GL_MODULATE, GL_MODULATE, GL_MODULATE, GL_MODULATE};

    // GLES 1.1 Client Arrays

    // GLES 2.0 Vertex Attributes (16 slots)
    Vec4 genericAttrib[16];

    // Default FBO (0) fallback renderbuffer
    Renderbuffer defaultRb;

    ContextData() {
        for (int i = 0; i < 16; ++i) genericAttrib[i] = {0, 0, 0, 1};
        defaultRb.width = 320;
        defaultRb.height = 480;
        defaultRb.rgba.assign(320 * 480 * 4, 0);
        defaultRb.depth.assign(320 * 480, 1.0f);
        defaultRb.stencil.assign(320 * 480, 0);
    }

    std::vector<Mat4>& currentMatrixStack() {
        if (matrixMode == GL_PROJECTION) return projectionStack;
        if (matrixMode == GL_TEXTURE) return textureStack;
        return modelviewStack;
    }
};

inline VaoState& currentVao(ContextData* c) {
    return c->boundVao ? c->vaos[c->boundVao] : c->defaultVao;
}

struct GLState {
    explicit GLState(Process&) {}
    Class eaglLayer = 0, eaglContext = 0, eaglSharegroup = 0;
    std::mutex mu;
    std::unordered_map<uint32_t, ObjC::id> currentByThread; // GuestThread::id -> EAGLContext id
    ObjC::id lastActiveContext = 0;
};

GLState& GLS(Process& p) { return p.service<GLState>(); }
GLState& GLS(Ctx& x) { return GLS(x.proc); }

ContextData* currentCtx(Ctx& x) {
    auto& st = GLS(x);
    uint32_t tid = x.proc.currentThread().id;
    ObjC::id cid = 0;
    {
        std::lock_guard<std::mutex> lk(st.mu);
        auto it = st.currentByThread.find(tid);
        if (it != st.currentByThread.end()) {
            cid = it->second;
        } else if (!st.currentByThread.empty()) {
            cid = st.currentByThread.begin()->second;
        } else if (st.lastActiveContext != 0) {
            cid = st.lastActiveContext;
        }
    }
    return cid ? rt(x).hostIf<ContextData>(cid) : nullptr;
}

SharegroupData* currentSg(Ctx& x, ContextData* c) {
    if (!c || !c->sharegroup) return nullptr;
    return rt(x).hostIf<SharegroupData>(c->sharegroup);
}

// Obtain the active color, depth and stencil targets for the current context.
struct RenderTarget {
    uint8_t* rgba = nullptr;
    float* depth = nullptr;
    uint8_t* stencil = nullptr;
    int width = 0, height = 0;
    bool isTexture = false;
};

RenderTarget activeTarget(Ctx& x, ContextData& c, SharegroupData& sg) {
    if (c.framebuffer != 0) {
        auto fit = sg.framebuffers.find(c.framebuffer);
        if (fit != sg.framebuffers.end()) {
            const Framebuffer& fb = fit->second;
            RenderTarget rtOut{};
            auto includeSize = [&](const Renderbuffer& rb) {
                rtOut.width = rtOut.width > 0 ? std::min(rtOut.width, rb.width) : rb.width;
                rtOut.height = rtOut.height > 0 ? std::min(rtOut.height, rb.height) : rb.height;
            };
            if (fb.colorAttachmentType == GL_RENDERBUFFER && fb.colorAttachmentId != 0) {
                auto rbit = sg.renderbuffers.find(fb.colorAttachmentId);
                if (rbit != sg.renderbuffers.end() && !rbit->second.rgba.empty()) {
                    Renderbuffer& rb = rbit->second;
                    rtOut.rgba = rb.rgba.data();
                    includeSize(rb);
                    // Colour storage is not an implicit depth attachment.
                }
            } else if (fb.colorAttachmentType == GL_TEXTURE_2D && fb.colorAttachmentId != 0) {
                auto txit = sg.textures.find(fb.colorAttachmentId);
                if (txit != sg.textures.end()) {
                    Texture2D& tx = txit->second;
                    rtOut.rgba = tx.rgba.data();
                    rtOut.width = tx.width;
                    rtOut.height = tx.height;
                    rtOut.isTexture = true;
                }
            }
            auto attach = [&](uint32_t type, uint32_t id, bool depth) {
                if (type != GL_RENDERBUFFER || id == 0) return;
                auto it = sg.renderbuffers.find(id);
                if (it == sg.renderbuffers.end()) return;
                Renderbuffer& rb = it->second;
                const size_t count = size_t(rb.width) * size_t(rb.height);
                if (depth && rb.depth.size() == count) rtOut.depth = rb.depth.data();
                if (rb.stencil.size() == count) rtOut.stencil = rb.stencil.data();
                if ((depth && rtOut.depth) || rtOut.stencil) includeSize(rb);
            };
            attach(fb.depthAttachmentType, fb.depthAttachmentId, true);
            attach(fb.stencilAttachmentType, fb.stencilAttachmentId, false);
            if ((rtOut.rgba || rtOut.depth || rtOut.stencil) && rtOut.width > 0 && rtOut.height > 0) return rtOut;
        }
    }
    if (c.renderbuffer != 0) {
        auto rbit = sg.renderbuffers.find(c.renderbuffer);
        if (rbit != sg.renderbuffers.end() && rbit->second.width > 0 && rbit->second.height > 0 && !rbit->second.rgba.empty()) {
            Renderbuffer& rb = rbit->second;
            return {rb.rgba.data(), rb.depth.data(), rb.stencil.data(), rb.width, rb.height, false};
        }
    }
    return {c.defaultRb.rgba.data(), c.defaultRb.depth.data(), c.defaultRb.stencil.data(), c.defaultRb.width, c.defaultRb.height, false};
}

// =========================================================== CAEAGLLayer ===

id eaglLayer_init(Ctx& x, id self, SEL s) {
    const id me = id(rt(x).sendSuper(x.cpu, self, ui(x).layer, s));
    if (me) {
        EAGLLayerData& ed = rt(x).host<EAGLLayerData>(me);
        const id kRet = makeString(x, "kEAGLDrawablePropertyRetainedBacking");
        const id kFmt = makeString(x, "kEAGLDrawablePropertyColorFormat");
        const id vFmt = makeString(x, "kEAGLColorFormatRGBA8");
        ed.drawableProperties = Foundation::makeDict(x, {{kRet, Foundation::makeBool(x, false)}, {kFmt, vFmt}}, true);
        LayerData& ld = layerData(x, me);
        ld.externalContents = [& p = x.proc, me]() -> G::ImagePtr {
            if (auto* d = ObjC::rt(p).hostIf<EAGLLayerData>(me)) return d->frontBuffer;
            return nullptr;
        };
    }
    return me;
}

id eaglLayer_initWithCoder(Ctx& x, id self, SEL, id) {
    return eaglLayer_init(x, self, rt(x).selInit);
}

void eaglLayer_dealloc(Ctx& x, id self, SEL s) {
    if (HostGL::enabled(x.proc)) HostGL::layerGone(x.proc, self);
    if (auto* d = rt(x).hostIf<EAGLLayerData>(self)) {
        if (d->drawableProperties) rt(x).release(x.cpu, d->drawableProperties);
        d->drawableProperties = 0;
        d->frontBuffer.reset();
    }
    rt(x).sendSuper(x.cpu, self, ui(x).layer, s);
}

id eaglLayer_props(Ctx& x, id self, SEL) {
    return rt(x).host<EAGLLayerData>(self).drawableProperties;
}

void eaglLayer_setProps(Ctx& x, id self, SEL, id dict) {
    auto& d = rt(x).host<EAGLLayerData>(self);
    const id copy = dict ? id(rt(x).send(x.cpu, dict, "copy")) : 0;
    if (d.drawableProperties) rt(x).release(x.cpu, d.drawableProperties);
    d.drawableProperties = copy;
}

// ================================================= EAGLContext / Sharegroup ===

id sg_init(Ctx& x, id self, SEL) {
    rt(x).host<SharegroupData>(self);
    return self;
}

id ctx_initWithAPI(Ctx& x, id self, SEL, uint32_t api) {
    return id(rt(x).send(x.cpu, self, "initWithAPI:sharegroup:", {api, 0}));
}

id ctx_initWithAPISharegroup(Ctx& x, id self, SEL, uint32_t api, id sg) {
    if (api < 1 || api > 3) return 0;
    ContextData& d = rt(x).host<ContextData>(self);
    d.api = api;
    if (!sg) {
        sg = id(rt(x).send(x.cpu, id(rt(x).send(x.cpu, GLS(x).eaglSharegroup, rt(x).selAlloc)), rt(x).selInit));
        d.sharegroup = sg;
    } else {
        d.sharegroup = rt(x).retain(x.cpu, sg);
    }
    if (HostGL::enabled(x.proc) && !HostGL::contextCreate(x.proc, self, int(api), d.sharegroup))
        x.proc.logf(LogLevel::Warning, "GPU: the host driver has no OpenGL ES %u context; this context renders in software", api);
    return self;
}

void ctx_dealloc(Ctx& x, id self, SEL) {
    if (HostGL::enabled(x.proc)) HostGL::contextDestroy(x.proc, self);
    if (auto* d = rt(x).hostIf<ContextData>(self)) {
        if (d->sharegroup) rt(x).release(x.cpu, d->sharegroup);
        d->sharegroup = 0;
    }
    auto& st = GLS(x);
    {
        std::lock_guard<std::mutex> lk(st.mu);
        for (auto it = st.currentByThread.begin(); it != st.currentByThread.end();) {
            if (it->second == self) it = st.currentByThread.erase(it);
            else ++it;
        }
    }
    rt(x).disposeInstance(x.cpu, self);
}

bool ctx_setCurrent(Ctx& x, id, SEL, id ctx) {
    auto& st = GLS(x);
    const uint32_t tid = x.proc.currentThread().id;
    const bool host = HostGL::enabled(x.proc);
    std::lock_guard<std::mutex> lk(st.mu);
    if (ctx && !host) {
        for (const auto& [t, c] : st.currentByThread) {
            if (t != tid && c == ctx) {
                x.proc.logf(LogLevel::Warning, "EAGLContext setCurrentContext: fails because ctx 0x%x is in use on thread %u", ctx, t);
                return false;
            }
        }
    }
    // HostGL may give a second thread its own context in the same sharegroup.
    // Only bind after the software affinity check, and report that successful
    // shared bind to the guest instead of leaving its thread without a context.
    if (host) HostGL::contextMakeCurrent(x.proc, ctx);
    if (ctx) {
        st.currentByThread[tid] = ctx;
        st.lastActiveContext = ctx;
    } else {
        st.currentByThread.erase(tid);
    }
    return true;
}

id ctx_current(Ctx& x, id, SEL) {
    auto& st = GLS(x);
    const uint32_t tid = x.proc.currentThread().id;
    std::lock_guard<std::mutex> lk(st.mu);
    auto it = st.currentByThread.find(tid);
    if (it != st.currentByThread.end()) return it->second;
    // On the GPU path the answer must be this thread's own: engines check
    // `if ([EAGLContext currentContext] != ctx) [EAGLContext setCurrentContext:ctx];`
    // on their render thread, and reporting another thread's context made them
    // skip that call, so their GL went to a stand-in context whose framebuffers
    // they never created (Army of Darkness: black screen, incomplete framebuffer).
    if (HostGL::enabled(x.proc)) return 0;
    if (!st.currentByThread.empty()) return st.currentByThread.begin()->second;
    return st.lastActiveContext;
}

uint32_t ctx_api(Ctx& x, id self, SEL) { return rt(x).host<ContextData>(self).api; }
id ctx_sharegroup(Ctx& x, id self, SEL) { return rt(x).host<ContextData>(self).sharegroup; }

bool ctx_renderbufferStorage(Ctx& x, id self, SEL, uint32_t target, id drawable) {
    (void)target;
    if (HostGL::isHostContext(x.proc, self)) {
        int w = 0, h = 0;
        if (drawable) {
            LayerData& ld = layerData(x, drawable);
            const float scale = ld.contentsScale > 0 ? ld.contentsScale : ui(x).scale;
            const float bw = ld.bounds.w > 0 ? ld.bounds.w : ui(x).screenW;
            const float bh = ld.bounds.h > 0 ? ld.bounds.h : ui(x).screenH;
            w = std::max(1, int(std::round(bw * scale)));
            h = std::max(1, int(std::round(bh * scale)));
        }
        return HostGL::renderbufferStorageFromDrawable(x.proc, self, drawable, w, h);
    }
    ContextData& c = rt(x).host<ContextData>(self);
    SharegroupData* sg = currentSg(x, &c);
    if (!sg || c.renderbuffer == 0) return false;
    Renderbuffer& rb = sg->renderbuffers[c.renderbuffer];
    rb.id = c.renderbuffer;
    if (!drawable) {
        rb.width = rb.height = 0;
        rb.rgba.clear();
        rb.depth.clear();
        rb.stencil.clear();
        rb.drawableLayer = 0;
        return true;
    }
    LayerData& ld = layerData(x, drawable);
    const float scale = ld.contentsScale > 0 ? ld.contentsScale : ui(x).scale;
    float bw = ld.bounds.w > 0 ? ld.bounds.w : ui(x).screenW;
    float bh = ld.bounds.h > 0 ? ld.bounds.h : ui(x).screenH;
    rb.width = std::max(1, int(std::round(bw * scale)));
    rb.height = std::max(1, int(std::round(bh * scale)));
    rb.internalFormat = GL_RGBA8_OES;
    rb.rgba.assign(size_t(rb.width) * size_t(rb.height) * 4, 0);
    rb.depth.assign(size_t(rb.width) * size_t(rb.height), 1.0f);
    rb.drawableLayer = drawable;
    c.viewport[0] = 0; c.viewport[1] = 0; c.viewport[2] = rb.width; c.viewport[3] = rb.height;
    c.scissor[0] = 0;  c.scissor[1] = 0;  c.scissor[2] = rb.width;  c.scissor[3] = rb.height;
    return true;
}

static std::atomic<int> totalDrawCalls{0};

void noteLayerPresented(Ctx& x, id layer) {
    if (!layer) return;
    LayerData& ld = layerData(x, layer);
    auto& u = ui(x);
    if (std::find(u.eaglLayers.begin(), u.eaglLayers.end(), layer) == u.eaglLayers.end())
        u.eaglLayers.push_back(layer);
    if (layerOnScreen(x, layer)) {
        ld.everOnScreen = true;
        ld.pendingOffscreen = 0;
    } else {
        if (ld.everOnScreen) {
            x.proc.perf().offscreenPresents++;
        } else {
            ld.pendingOffscreen++;
        }
    }
}

void publishFrame(Ctx& x, id layer, const uint8_t* rgbaBottomLeft, int width, int height);

bool ctx_presentRenderbuffer(Ctx& x, id self, SEL, uint32_t target) {
    (void)target;
    if (HostGL::isHostContext(x.proc, self)) {
        uint32_t layer = 0;
        int w = 0, h = 0;
        bool direct = false;
        std::vector<uint8_t> pixels;
        if (!HostGL::presentRenderbuffer(x.proc, self, layer, w, h, pixels, direct)) return false;
        if (!direct) publishFrame(x, layer, pixels.data(), w, h);
        else {
            noteLayerPresented(x, layer);
            // The host shows GL frames itself. The UIKit overlay only needs a new
            // composite when the layer first switches to host presentation (or when
            // UIKit content changes, which marks the composite dirty on its own).
            LayerData& ld = layerData(x, layer);
            if (!ld.hostPresented) {
                ld.hostPresented = true;
                setNeedsComposite(x);
            }
            x.proc.perf().notePresented();
        }
        return true;
    }
    ContextData& c = rt(x).host<ContextData>(self);
    SharegroupData* sg = currentSg(x, &c);
    if (!sg) return false;
    Renderbuffer* pRb = nullptr;
    if (c.renderbuffer != 0) {
        auto it = sg->renderbuffers.find(c.renderbuffer);
        if (it != sg->renderbuffers.end() && it->second.drawableLayer) pRb = &it->second;
    }
    if (!pRb && c.framebuffer != 0) {
        auto fit = sg->framebuffers.find(c.framebuffer);
        if (fit != sg->framebuffers.end() && fit->second.colorAttachmentType == GL_RENDERBUFFER) {
            auto it = sg->renderbuffers.find(fit->second.colorAttachmentId);
            if (it != sg->renderbuffers.end() && it->second.drawableLayer) pRb = &it->second;
        }
    }
    if (!pRb) {
        for (auto& [id, r] : sg->renderbuffers) {
            if (r.drawableLayer && r.width > 0 && r.height > 0 && !r.rgba.empty()) {
                pRb = &r;
                break;
            }
        }
    }
    if (!pRb || !pRb->drawableLayer || pRb->width <= 0 || pRb->height <= 0 || pRb->rgba.empty()) return false;
    Renderbuffer& rb = *pRb;

    // If an offscreen framebuffer was rendered and is still active, blit into the drawable renderbuffer.
    if (c.framebuffer != 0) {
        const RenderTarget rtOut = activeTarget(x, c, *sg);
        if (rtOut.rgba && rtOut.rgba != rb.rgba.data() && rtOut.width > 0 && rtOut.height > 0) {
            if (rtOut.width == rb.width && rtOut.height == rb.height) {
                std::memcpy(rb.rgba.data(), rtOut.rgba, size_t(rb.width) * size_t(rb.height) * 4);
            } else {
                for (int y = 0; y < rb.height; ++y) {
                    const int sy = int(int64_t(y) * rtOut.height / rb.height);
                    for (int xx = 0; xx < rb.width; ++xx) {
                        const int sx = int(int64_t(xx) * rtOut.width / rb.width);
                        std::memcpy(rb.rgba.data() + (size_t(y) * rb.width + xx) * 4,
                                    rtOut.rgba + (size_t(sy) * rtOut.width + sx) * 4, 4);
                    }
                }
            }
        }
    } else {
        auto hasContent = [](const uint8_t* p, size_t sz) {
            if (!p || sz < 16) return false;
            const uint64_t* u = reinterpret_cast<const uint64_t*>(p);
            const size_t n = sz / 8;
            for (size_t i = 0; i < n; ++i) if (u[i] != 0) return true;
            return false;
        };
        if (!hasContent(rb.rgba.data(), rb.rgba.size())) {
            for (const auto& [fbid, fb] : sg->framebuffers) {
                if (fb.colorAttachmentType == GL_TEXTURE_2D && fb.colorAttachmentId != 0) {
                    auto tit = sg->textures.find(fb.colorAttachmentId);
                    if (tit != sg->textures.end()) {
                        const auto& lev = tit->second;
                        if (lev.width > 0 && lev.height > 0 && hasContent(lev.rgba.data(), lev.rgba.size())) {
                            for (int y = 0; y < rb.height; ++y) {
                                const int sy = int(int64_t(y) * lev.height / rb.height);
                                for (int xx = 0; xx < rb.width; ++xx) {
                                    const int sx = int(int64_t(xx) * lev.width / rb.width);
                                    std::memcpy(rb.rgba.data() + (size_t(y) * rb.width + xx) * 4,
                                                lev.rgba.data() + (size_t(sy) * lev.width + sx) * 4, 4);
                                }
                            }
                            break;
                        }
                    }
                }
            }
        }
    }

    publishFrame(x, rb.drawableLayer, rb.rgba.data(), rb.width, rb.height);
    return true;
}

// Hand a presented frame (straight RGBA, GL bottom-left row order) to the compositor.
void publishFrame(Ctx& x, id layer, const uint8_t* rgbaBottomLeft, int width, int height) {
    struct Rb { int width, height; const uint8_t* data; id drawableLayer; } rb{width, height, rgbaBottomLeft, layer};
    noteLayerPresented(x, layer);
    auto img = std::make_shared<G::Image>();
    img->width = rb.width;
    img->height = rb.height;
    img->rgba.resize(size_t(rb.width) * size_t(rb.height) * 4);
    // Flip vertically (OpenGL bottom-left -> UIKit top-left) and premultiply alpha for compositor.
    // An opaque layer (what GL games use) ignores the renderbuffer's alpha, as Core Animation does:
    // blending leaves alpha < 1 in places, which must not let the views behind show through.
    const bool opaqueLayer = layerData(x, rb.drawableLayer).opaque;
    // Debugging aid: HVM_DUMP_RB=<file> keeps overwriting <file> with the latest presented renderbuffer (raw RGBA, GL row order).
    if (const char* f = std::getenv("HVM_DUMP_RB")) {
        if (std::FILE* fp = std::fopen(f, "wb")) {
            const int32_t wh[2] = {rb.width, rb.height};
            std::fwrite(wh, 4, 2, fp);
            std::fwrite(rb.data, 1, size_t(rb.width) * size_t(rb.height) * 4, fp);
            std::fclose(fp);
        }
    }
    for (int y = 0; y < rb.height; ++y) {
        const uint8_t* srcRow = rb.data + size_t(rb.height - 1 - y) * size_t(rb.width) * 4;
        uint8_t* dstRow = &img->rgba[size_t(y) * size_t(rb.width) * 4];
        for (int px = 0; px < rb.width; ++px) {
            const uint8_t r = srcRow[px * 4 + 0];
            const uint8_t g = srcRow[px * 4 + 1];
            const uint8_t b = srcRow[px * 4 + 2];
            const uint8_t a = opaqueLayer ? 255 : srcRow[px * 4 + 3];
            if (a == 255) {
                dstRow[px * 4 + 0] = r;
                dstRow[px * 4 + 1] = g;
                dstRow[px * 4 + 2] = b;
                dstRow[px * 4 + 3] = 255;
            } else {
                dstRow[px * 4 + 0] = uint8_t((uint32_t(r) * a + 127) / 255);
                dstRow[px * 4 + 1] = uint8_t((uint32_t(g) * a + 127) / 255);
                dstRow[px * 4 + 2] = uint8_t((uint32_t(b) * a + 127) / 255);
                dstRow[px * 4 + 3] = a;
            }
        }
    }
    EAGLLayerData& ed = rt(x).host<EAGLLayerData>(rb.drawableLayer);
    ed.frontBuffer = img;
    LayerData& ld = layerData(x, rb.drawableLayer);
    if (!ld.externalContents) {
        const ObjC::id layerId = rb.drawableLayer;
        ld.externalContents = [&p = x.proc, layerId]() -> G::ImagePtr {
            if (auto* d = ObjC::rt(p).hostIf<EAGLLayerData>(layerId)) return d->frontBuffer;
            return nullptr;
        };
    }
    setNeedsComposite(x);
}

// =========================================================== GL C Functions ===

uint32_t gl_getError(Ctx& x) {
    ContextData* c = currentCtx(x);
    if (!c) return GL_INVALID_OPERATION;
    const uint32_t e = c->lastError;
    c->lastError = GL_NO_ERROR;
    return e;
}

GuestPtr gl_getString(Ctx& x, uint32_t name) {
    ContextData* c = currentCtx(x);
    const uint32_t api = c ? c->api : 2;
    switch (name) {
        case GL_VENDOR: return x.proc.internCString("Apple Inc.");
        case GL_RENDERER: return x.proc.internCString("Apple A6 GPU (HydrogenVM)");
        case GL_VERSION: return x.proc.internCString(api == 1 ? "OpenGL ES-CM 1.1 Apple A6" : "OpenGL ES 2.0 Apple A6");
        case GL_SHADING_LANGUAGE_VERSION: return x.proc.internCString("OpenGL ES GLSL ES 1.00");
        case GL_EXTENSIONS:
            return x.proc.internCString(
                "GL_OES_framebuffer_object GL_OES_rgb8_rgba8 GL_OES_texture_npot "
                "GL_EXT_texture_format_BGRA8888 GL_OES_vertex_array_object "
                "GL_OES_mapbuffer GL_OES_draw_texture GL_OES_point_sprite "
                "GL_OES_point_size_array GL_IMG_texture_compression_pvrtc "
                "GL_APPLE_framebuffer_multisample GL_EXT_discard_framebuffer "
                "GL_OES_depth24 GL_OES_stencil8 GL_OES_packed_depth_stencil "
                "GL_OES_standard_derivatives");
        default: return 0;
    }
}

void setCap(ContextData& c, uint32_t cap, bool en) {
    switch (cap) {
        case GL_BLEND: c.blend = en; break;
        case GL_DEPTH_TEST: c.depthTest = en; break;
        case GL_SCISSOR_TEST: c.scissorTest = en; break;
        case GL_CULL_FACE: c.cullFace = en; break;
        case GL_ALPHA_TEST: c.alphaTest = en; break;
        case GL_STENCIL_TEST: c.stencilTest = en; break;
        case GL_TEXTURE_2D: c.texture2DEnabled[c.activeTexture & 7] = en; break;
        default: break;
    }
}

void gl_enable(Ctx& x, uint32_t cap) { if (auto* c = currentCtx(x)) setCap(*c, cap, true); }
void gl_disable(Ctx& x, uint32_t cap) { if (auto* c = currentCtx(x)) setCap(*c, cap, false); }
bool gl_isEnabled(Ctx& x, uint32_t cap) {
    auto* c = currentCtx(x);
    if (!c) return false;
    switch (cap) {
        case GL_BLEND: return c->blend;
        case GL_DEPTH_TEST: return c->depthTest;
        case GL_SCISSOR_TEST: return c->scissorTest;
        case GL_CULL_FACE: return c->cullFace;
        case GL_ALPHA_TEST: return c->alphaTest;
        case GL_STENCIL_TEST: return c->stencilTest;
        case GL_TEXTURE_2D: return c->texture2DEnabled[c->activeTexture & 7];
        case GL_VERTEX_ARRAY: return currentVao(c).vertexPtr.enabled;
        case GL_COLOR_ARRAY: return currentVao(c).colorPtr.enabled;
        case GL_NORMAL_ARRAY: return currentVao(c).normalPtr.enabled;
        case GL_TEXTURE_COORD_ARRAY: return currentVao(c).texCoordPtr[c->clientActiveTexture & 7].enabled;
        case GL_POINT_SIZE_ARRAY_OES: return currentVao(c).pointSizePtr.enabled;
        default: return false;
    }
}

void gl_viewport(Ctx& x, int32_t vx, int32_t vy, int32_t vw, int32_t vh) {
    if (auto* c = currentCtx(x)) { c->viewport[0] = vx; c->viewport[1] = vy; c->viewport[2] = vw; c->viewport[3] = vh; }
}
void gl_scissor(Ctx& x, int32_t sx, int32_t sy, int32_t sw, int32_t sh) {
    if (auto* c = currentCtx(x)) { c->scissor[0] = sx; c->scissor[1] = sy; c->scissor[2] = sw; c->scissor[3] = sh; }
}
void gl_clearColor(Ctx& x, float r, float g, float b, float a) {
    if (auto* c = currentCtx(x)) {
        c->clearColor[0] = std::clamp(r, 0.0f, 1.0f);
        c->clearColor[1] = std::clamp(g, 0.0f, 1.0f);
        c->clearColor[2] = std::clamp(b, 0.0f, 1.0f);
        c->clearColor[3] = std::clamp(a, 0.0f, 1.0f);
    }
}
void gl_clearColorx(Ctx& x, int32_t r, int32_t g, int32_t b, int32_t a) {
    gl_clearColor(x, r / 65536.0f, g / 65536.0f, b / 65536.0f, a / 65536.0f);
}
void gl_clearDepthf(Ctx& x, float d) { if (auto* c = currentCtx(x)) c->clearDepth = std::clamp(d, 0.0f, 1.0f); }
void gl_clearDepthx(Ctx& x, int32_t d) { gl_clearDepthf(x, d / 65536.0f); }
void gl_clearStencil(Ctx& x, int32_t s) { if (auto* c = currentCtx(x)) c->clearStencil = s; }

void gl_clear(Ctx& x, uint32_t mask) {
    ContextData* c = currentCtx(x);
    SharegroupData* sg = currentSg(x, c);
    if (!c || !sg) return;
    RenderTarget rt = activeTarget(x, *c, *sg);
    if ((!rt.rgba && !rt.depth && !rt.stencil) || rt.width <= 0 || rt.height <= 0) return;

    int x0 = 0, y0 = 0, x1 = rt.width, y1 = rt.height;
    if (c->scissorTest) {
        x0 = std::clamp(c->scissor[0], 0, rt.width);
        y0 = std::clamp(c->scissor[1], 0, rt.height);
        x1 = std::clamp(c->scissor[0] + c->scissor[2], 0, rt.width);
        y1 = std::clamp(c->scissor[1] + c->scissor[3], 0, rt.height);
    }
    if (x1 <= x0 || y1 <= y0) return;

    if ((mask & GL_COLOR_BUFFER_BIT) && rt.rgba) {
        const uint8_t cr = uint8_t(c->clearColor[0] * 255.0f + 0.5f);
        const uint8_t cg = uint8_t(c->clearColor[1] * 255.0f + 0.5f);
        const uint8_t cb = uint8_t(c->clearColor[2] * 255.0f + 0.5f);
        const uint8_t ca = uint8_t(c->clearColor[3] * 255.0f + 0.5f);
        for (int y = y0; y < y1; ++y) {
            uint8_t* row = &rt.rgba[size_t(y) * size_t(rt.width) * 4];
            for (int px = x0; px < x1; ++px) {
                if (c->colorMask[0]) row[px * 4 + 0] = cr;
                if (c->colorMask[1]) row[px * 4 + 1] = cg;
                if (c->colorMask[2]) row[px * 4 + 2] = cb;
                if (c->colorMask[3]) row[px * 4 + 3] = ca;
            }
        }
    }
    if ((mask & GL_DEPTH_BUFFER_BIT) && rt.depth && c->depthMask) {
        for (int y = y0; y < y1; ++y) {
            float* drow = &rt.depth[size_t(y) * size_t(rt.width)];
            for (int px = x0; px < x1; ++px) drow[px] = c->clearDepth;
        }
    }
    if ((mask & GL_STENCIL_BUFFER_BIT) && rt.stencil) {
        const uint8_t clear = uint8_t(c->clearStencil) & uint8_t(c->stencilFront.writeMask);
        const uint8_t preserve = uint8_t(~c->stencilFront.writeMask);
        for (int y = y0; y < y1; ++y) {
            uint8_t* row = &rt.stencil[size_t(y) * size_t(rt.width)];
            for (int px = x0; px < x1; ++px) row[px] = clear | (row[px] & preserve);
        }
    }
}

void gl_stencilFunc(Ctx& x, uint32_t func, int32_t ref, uint32_t mask) {
    if (auto* c = currentCtx(x)) {
        c->stencilFront.func = c->stencilBack.func = func;
        c->stencilFront.ref = c->stencilBack.ref = ref;
        c->stencilFront.mask = c->stencilBack.mask = mask;
    }
}
void gl_stencilFuncSeparate(Ctx& x, uint32_t face, uint32_t func, int32_t ref, uint32_t mask) {
    if (auto* c = currentCtx(x)) {
        if (face == GL_FRONT || face == GL_FRONT_AND_BACK) {
            c->stencilFront.func = func; c->stencilFront.ref = ref; c->stencilFront.mask = mask;
        }
        if (face == GL_BACK || face == GL_FRONT_AND_BACK) {
            c->stencilBack.func = func; c->stencilBack.ref = ref; c->stencilBack.mask = mask;
        }
    }
}
void gl_stencilOp(Ctx& x, uint32_t fail, uint32_t zfail, uint32_t zpass) {
    if (auto* c = currentCtx(x)) {
        c->stencilFront.fail = c->stencilBack.fail = fail;
        c->stencilFront.zfail = c->stencilBack.zfail = zfail;
        c->stencilFront.zpass = c->stencilBack.zpass = zpass;
    }
}
void gl_stencilOpSeparate(Ctx& x, uint32_t face, uint32_t fail, uint32_t zfail, uint32_t zpass) {
    if (auto* c = currentCtx(x)) {
        if (face == GL_FRONT || face == GL_FRONT_AND_BACK) {
            c->stencilFront.fail = fail; c->stencilFront.zfail = zfail; c->stencilFront.zpass = zpass;
        }
        if (face == GL_BACK || face == GL_FRONT_AND_BACK) {
            c->stencilBack.fail = fail; c->stencilBack.zfail = zfail; c->stencilBack.zpass = zpass;
        }
    }
}
void gl_stencilMask(Ctx& x, uint32_t mask) {
    if (auto* c = currentCtx(x)) c->stencilFront.writeMask = c->stencilBack.writeMask = mask;
}
void gl_stencilMaskSeparate(Ctx& x, uint32_t face, uint32_t mask) {
    if (auto* c = currentCtx(x)) {
        if (face == GL_FRONT || face == GL_FRONT_AND_BACK) c->stencilFront.writeMask = mask;
        if (face == GL_BACK || face == GL_FRONT_AND_BACK) c->stencilBack.writeMask = mask;
    }
}

void gl_blendFunc(Ctx& x, uint32_t sfactor, uint32_t dfactor) {
    if (auto* c = currentCtx(x)) { c->blendSrcRGB = c->blendSrcAlpha = sfactor; c->blendDstRGB = c->blendDstAlpha = dfactor; }
}
void gl_blendFuncSeparate(Ctx& x, uint32_t srcRGB, uint32_t dstRGB, uint32_t srcA, uint32_t dstA) {
    if (auto* c = currentCtx(x)) { c->blendSrcRGB = srcRGB; c->blendDstRGB = dstRGB; c->blendSrcAlpha = srcA; c->blendDstAlpha = dstA; }
}
void gl_blendEquation(Ctx& x, uint32_t mode) { if (auto* c = currentCtx(x)) c->blendEqRGB = c->blendEqAlpha = mode; }
void gl_blendEquationSeparate(Ctx& x, uint32_t modeRGB, uint32_t modeA) {
    if (auto* c = currentCtx(x)) { c->blendEqRGB = modeRGB; c->blendEqAlpha = modeA; }
}
void gl_blendColor(Ctx& x, float r, float g, float b, float a) {
    if (auto* c = currentCtx(x)) {
        c->blendColor[0] = std::clamp(r, 0.0f, 1.0f);
        c->blendColor[1] = std::clamp(g, 0.0f, 1.0f);
        c->blendColor[2] = std::clamp(b, 0.0f, 1.0f);
        c->blendColor[3] = std::clamp(a, 0.0f, 1.0f);
    }
}
void gl_depthFunc(Ctx& x, uint32_t func) { if (auto* c = currentCtx(x)) c->depthFunc = func; }
void gl_depthMask(Ctx& x, uint32_t flag) { if (auto* c = currentCtx(x)) c->depthMask = (flag & 0xFF) != 0; }
void gl_depthRangef(Ctx& x, float zNear, float zFar) {
    if (auto* c = currentCtx(x)) { c->depthRange[0] = std::clamp(zNear, 0.0f, 1.0f); c->depthRange[1] = std::clamp(zFar, 0.0f, 1.0f); }
}
void gl_depthRangex(Ctx& x, int32_t zNear, int32_t zFar) { gl_depthRangef(x, zNear / 65536.0f, zFar / 65536.0f); }
void gl_colorMask(Ctx& x, uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    if (auto* c = currentCtx(x)) { c->colorMask[0] = r & 0xFF; c->colorMask[1] = g & 0xFF; c->colorMask[2] = b & 0xFF; c->colorMask[3] = a & 0xFF; }
}
void gl_cullFace(Ctx& x, uint32_t mode) { if (auto* c = currentCtx(x)) c->cullFaceMode = mode; }
void gl_frontFace(Ctx& x, uint32_t mode) { if (auto* c = currentCtx(x)) c->frontFace = mode; }
void gl_alphaFunc(Ctx& x, uint32_t func, float ref) { if (auto* c = currentCtx(x)) { c->alphaFunc = func; c->alphaRef = std::clamp(ref, 0.0f, 1.0f); } }
void gl_alphaFuncx(Ctx& x, uint32_t func, int32_t ref) { gl_alphaFunc(x, func, ref / 65536.0f); }
void gl_pixelStorei(Ctx& x, uint32_t pname, int32_t param) {
    if (auto* c = currentCtx(x)) {
        if (pname == GL_UNPACK_ALIGNMENT) c->unpackAlignment = std::max(1, param);
        else if (pname == GL_PACK_ALIGNMENT) c->packAlignment = std::max(1, param);
    }
}
void gl_lineWidth(Ctx& x, float w) { if (auto* c = currentCtx(x)) c->lineWidth = std::max(1.0f, w); }
void gl_lineWidthx(Ctx& x, int32_t w) { gl_lineWidth(x, w / 65536.0f); }
void gl_pointSize(Ctx& x, float s) { if (auto* c = currentCtx(x)) c->pointSize = std::max(1.0f, s); }
void gl_pointSizex(Ctx& x, int32_t s) { gl_pointSize(x, s / 65536.0f); }
void gl_noop(Ctx&) {}
void gl_noop1(Ctx&, uint32_t) {}
void gl_noop2(Ctx&, uint32_t, uint32_t) {}
void gl_noop3(Ctx&, uint32_t, uint32_t, uint32_t) {}

void gl_getIntegerv(Ctx& x, uint32_t pname, GuestPtr params) {
    if (!params) return;
    ContextData* c = currentCtx(x);
    auto wr = [&](int32_t v, int idx = 0) { x.mem.write<int32_t>(params + uint32_t(idx * 4), v); };
    switch (pname) {
        case GL_MAX_TEXTURE_SIZE: case GL_MAX_RENDERBUFFER_SIZE: wr(4096); break;
        case GL_MAX_VIEWPORT_DIMS: wr(4096, 0); wr(4096, 1); break;
        case GL_MAX_TEXTURE_UNITS: case GL_MAX_TEXTURE_IMAGE_UNITS: case GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS: wr(8); break;
        case GL_MAX_VERTEX_ATTRIBS: wr(16); break;
        case GL_MAX_VERTEX_UNIFORM_VECTORS: wr(256); break;
        case GL_MAX_VARYING_VECTORS: wr(16); break;
        case GL_MAX_FRAGMENT_UNIFORM_VECTORS: wr(64); break;
        case GL_SUBPIXEL_BITS: wr(4); break;
        case GL_RED_BITS: case GL_GREEN_BITS: case GL_BLUE_BITS: case GL_ALPHA_BITS: wr(8); break;
        case GL_DEPTH_BITS: wr(24); break;
        case GL_STENCIL_BITS: wr(8); break;
        case GL_VIEWPORT: if (c) for (int i = 0; i < 4; ++i) wr(c->viewport[i], i); break;
        case GL_SCISSOR_BOX: if (c) for (int i = 0; i < 4; ++i) wr(c->scissor[i], i); break;
        case GL_FRAMEBUFFER_BINDING: wr(c ? int32_t(c->framebuffer) : 0); break;
        case 0x8CAA: wr(c ? int32_t(c->readFramebuffer) : 0); break;   // GL_READ_FRAMEBUFFER_BINDING_APPLE
        case GL_RENDERBUFFER_BINDING: wr(c ? int32_t(c->renderbuffer) : 0); break;
        case GL_ARRAY_BUFFER_BINDING: wr(c ? int32_t(c->arrayBuffer) : 0); break;
        case GL_ELEMENT_ARRAY_BUFFER_BINDING: wr(c ? int32_t(currentVao(c).elementArrayBuffer) : 0); break;
        case GL_CURRENT_PROGRAM: wr(c ? int32_t(c->currentProgram) : 0); break;
        case GL_ACTIVE_TEXTURE: wr(c ? int32_t(GL_TEXTURE0 + c->activeTexture) : int32_t(GL_TEXTURE0)); break;
        case GL_UNPACK_ALIGNMENT: wr(c ? c->unpackAlignment : 4); break;
        case GL_PACK_ALIGNMENT: wr(c ? c->packAlignment : 4); break;
        case 0x86A2: wr(4); break;                                         // GL_NUM_COMPRESSED_TEXTURE_FORMATS
        case 0x86A3: for (int i = 0; i < 4; ++i) wr(0x8C00 + i, i); break; // GL_COMPRESSED_TEXTURE_FORMATS (PVRTC)
        case 0x8DF9: wr(0); break;                                         // GL_NUM_SHADER_BINARY_FORMATS
        case 0x0D31: wr(8); break;                                         // GL_MAX_LIGHTS
        case 0x0D32: wr(6); break;                                         // GL_MAX_CLIP_PLANES
        case 0x0D36: wr(16); break;                                        // GL_MAX_MODELVIEW_STACK_DEPTH
        case 0x0D38: case 0x0D39: wr(2); break;                            // projection / texture stack depth
        case 0x851C: wr(4096); break;                                      // GL_MAX_CUBE_MAP_TEXTURE_SIZE
        case 0x8D57: wr(4); break;                                         // GL_MAX_SAMPLES_APPLE
        case 0x8B9A: wr(0x1401); break;                                    // IMPLEMENTATION_COLOR_READ_TYPE
        case 0x8B9B: wr(0x1908); break;                                    // IMPLEMENTATION_COLOR_READ_FORMAT
        case 0x846D: case 0x846E: case 0x0B12: wr(1, 0); wr(64, 1); break;  // point size / line width ranges
        case 0x0BA0: wr(c ? int32_t(c->matrixMode) : 0x1700); break;       // GL_MATRIX_MODE
        case 0x8069: wr(c ? int32_t(c->boundTextures[c->activeTexture]) : 0); break;   // GL_TEXTURE_BINDING_2D
        case 0x84E1: wr(c ? int32_t(GL_TEXTURE0 + c->clientActiveTexture) : int32_t(GL_TEXTURE0)); break;   // CLIENT_ACTIVE_TEXTURE
        case 0x0BE2: wr(c && c->blend ? 1 : 0); break;                     // GL_BLEND
        case 0x0BE1: case 0x80CB: wr(c ? int32_t(c->blendSrcRGB) : 1); break;   // BLEND_SRC / SRC_RGB
        case 0x0BE0: case 0x80C8: wr(c ? int32_t(c->blendDstRGB) : 0); break;   // BLEND_DST / DST_RGB
        case 0x80C9: wr(c ? int32_t(c->blendSrcAlpha) : 1); break;
        case 0x80CA: wr(c ? int32_t(c->blendDstAlpha) : 0); break;
        default: wr(0); break;
    }
}

void gl_getFloatv(Ctx& x, uint32_t pname, GuestPtr params) {
    if (!params) return;
    ContextData* c = currentCtx(x);
    auto wr = [&](float v, int idx = 0) { x.mem.write<float>(params + uint32_t(idx * 4), v); };
    switch (pname) {
        case GL_COLOR_CLEAR_VALUE: if (c) for (int i = 0; i < 4; ++i) wr(c->clearColor[i], i); break;
        case GL_MODELVIEW_MATRIX: if (c) for (int i = 0; i < 16; ++i) wr(c->modelviewStack.back().m[i], i); break;
        case GL_PROJECTION_MATRIX: if (c) for (int i = 0; i < 16; ++i) wr(c->projectionStack.back().m[i], i); break;
        case GL_TEXTURE_MATRIX: if (c) for (int i = 0; i < 16; ++i) wr(c->textureStack.back().m[i], i); break;
        case GL_VIEWPORT: if (c) for (int i = 0; i < 4; ++i) wr(float(c->viewport[i]), i); break;
        default: {
            int32_t iv = 0;
            const uint32_t tmp = x.proc.heap().allocZeroed(16);
            gl_getIntegerv(x, pname, tmp);
            iv = x.mem.read<int32_t>(tmp);
            x.proc.heap().free(tmp);
            wr(float(iv));
            break;
        }
    }
}

void gl_getBooleanv(Ctx& x, uint32_t pname, GuestPtr params) {
    if (!params) return;
    const uint32_t tmp = x.proc.heap().allocZeroed(16);
    gl_getIntegerv(x, pname, tmp);
    x.mem.write<uint8_t>(params, x.mem.read<int32_t>(tmp) != 0 ? 1 : 0);
    x.proc.heap().free(tmp);
}

void gl_readPixels(Ctx& x, int32_t rx, int32_t ry, int32_t rw, int32_t rh, uint32_t format, uint32_t type, GuestPtr pixels) {
    ContextData* c = currentCtx(x);
    SharegroupData* sg = currentSg(x, c);
    if (!c || !sg || !pixels || rw <= 0 || rh <= 0) return;
    (void)format; (void)type;
    RenderTarget rt = activeTarget(x, *c, *sg);
    if (!rt.rgba) return;
    for (int y = 0; y < rh; ++y) {
        const int sy = ry + y;
        for (int px = 0; px < rw; ++px) {
            const int sx = rx + px;
            const uint32_t dstAddr = pixels + uint32_t((y * rw + px) * 4);
            if (sx >= 0 && sx < rt.width && sy >= 0 && sy < rt.height) {
                const uint8_t* p = &rt.rgba[(size_t(sy) * size_t(rt.width) + size_t(sx)) * 4];
                x.mem.copyIn(dstAddr, p, 4);
            } else {
                x.mem.write<uint32_t>(dstAddr, 0);
            }
        }
    }
}

// ================================================= FBOs & Renderbuffers ===

void gl_genRenderbuffers(Ctx& x, int32_t n, GuestPtr ids) {
    ContextData* c = currentCtx(x);
    SharegroupData* sg = currentSg(x, c);
    if (!sg || !ids || n <= 0) return;
    for (int32_t i = 0; i < n; ++i) {
        const uint32_t id = sg->nextRenderbuffer++;
        sg->renderbuffers[id].id = id;
        x.mem.write<uint32_t>(ids + uint32_t(i * 4), id);
    }
}
void gl_deleteRenderbuffers(Ctx& x, int32_t n, GuestPtr ids) {
    ContextData* c = currentCtx(x);
    SharegroupData* sg = currentSg(x, c);
    if (!sg || !ids || n <= 0) return;
    for (int32_t i = 0; i < n; ++i) {
        const uint32_t id = x.mem.read<uint32_t>(ids + uint32_t(i * 4));
        if (id) {
            sg->renderbuffers.erase(id);
            if (c->renderbuffer == id) c->renderbuffer = 0;
        }
    }
}
void gl_bindRenderbuffer(Ctx& x, uint32_t target, uint32_t rb) {
    (void)target;
    ContextData* c = currentCtx(x);
    SharegroupData* sg = currentSg(x, c);
    if (!c || !sg) return;
    c->renderbuffer = rb;
    if (rb != 0) sg->renderbuffers[rb].id = rb;
}
bool gl_isRenderbuffer(Ctx& x, uint32_t rb) {
    ContextData* c = currentCtx(x);
    SharegroupData* sg = currentSg(x, c);
    return sg && rb != 0 && sg->renderbuffers.count(rb) > 0;
}
void gl_renderbufferStorage(Ctx& x, uint32_t target, uint32_t internalformat, int32_t width, int32_t height) {
    (void)target;
    ContextData* c = currentCtx(x);
    SharegroupData* sg = currentSg(x, c);
    if (!c || !sg || c->renderbuffer == 0 || width < 0 || height < 0) return;
    Renderbuffer& rb = sg->renderbuffers[c->renderbuffer];
    rb.width = width;
    rb.height = height;
    rb.internalFormat = internalformat;
    rb.rgba.clear();
    rb.depth.clear();
    rb.stencil.clear();
    const size_t count = size_t(width) * size_t(height);
    switch (internalformat) {
        case GL_RGBA4: case GL_RGB5_A1: case GL_RGB565: case GL_RGBA8_OES: case GL_RGB8_OES:
            rb.rgba.assign(count * 4, 0);
            break;
        case GL_DEPTH_COMPONENT16: case GL_DEPTH_COMPONENT24_OES:
            rb.depth.assign(count, 1.0f);
            break;
        case GL_STENCIL_INDEX8:
            rb.stencil.assign(count, 0);
            break;
        case GL_DEPTH24_STENCIL8_OES:
            rb.depth.assign(count, 1.0f);
            rb.stencil.assign(count, 0);
            break;
        default:
            c->lastError = GL_INVALID_ENUM;
            break;
    }
}
void gl_renderbufferStorageMultisample(Ctx& x, uint32_t target, int32_t, uint32_t internalformat, int32_t width, int32_t height) {
    gl_renderbufferStorage(x, target, internalformat, width, height);
}
void gl_getRenderbufferParameteriv(Ctx& x, uint32_t target, uint32_t pname, GuestPtr params) {
    (void)target;
    ContextData* c = currentCtx(x);
    SharegroupData* sg = currentSg(x, c);
    if (!c || !sg || !params || c->renderbuffer == 0) return;
    const Renderbuffer& rb = sg->renderbuffers[c->renderbuffer];
    int32_t v = 0;
    switch (pname) {
        case GL_RENDERBUFFER_WIDTH: v = rb.width; break;
        case GL_RENDERBUFFER_HEIGHT: v = rb.height; break;
        case GL_RENDERBUFFER_INTERNAL_FORMAT: v = int32_t(rb.internalFormat); break;
        case GL_RENDERBUFFER_RED_SIZE:
        case GL_RENDERBUFFER_GREEN_SIZE:
        case GL_RENDERBUFFER_BLUE_SIZE:
            if (rb.rgba.empty()) v = 0;
            else if (rb.internalFormat == GL_RGB565) v = pname == GL_RENDERBUFFER_GREEN_SIZE ? 6 : 5;
            else if (rb.internalFormat == GL_RGBA4) v = 4;
            else if (rb.internalFormat == GL_RGB5_A1) v = 5;
            else v = 8;
            break;
        case GL_RENDERBUFFER_ALPHA_SIZE:
            v = rb.internalFormat == GL_RGBA8_OES ? 8 : (rb.internalFormat == GL_RGBA4 ? 4 : (rb.internalFormat == GL_RGB5_A1 ? 1 : 0));
            break;
        case GL_RENDERBUFFER_DEPTH_SIZE:
            v = rb.internalFormat == GL_DEPTH_COMPONENT16 ? 16 :
                (rb.internalFormat == GL_DEPTH_COMPONENT24_OES || rb.internalFormat == GL_DEPTH24_STENCIL8_OES ? 24 : 0);
            break;
        case GL_RENDERBUFFER_STENCIL_SIZE:
            v = rb.internalFormat == GL_STENCIL_INDEX8 || rb.internalFormat == GL_DEPTH24_STENCIL8_OES ? 8 : 0;
            break;
        default: break;
    }
    x.mem.write<int32_t>(params, v);
}

void gl_genFramebuffers(Ctx& x, int32_t n, GuestPtr ids) {
    ContextData* c = currentCtx(x);
    SharegroupData* sg = currentSg(x, c);
    if (!sg || !ids || n <= 0) return;
    for (int32_t i = 0; i < n; ++i) {
        const uint32_t id = sg->nextFramebuffer++;
        sg->framebuffers[id].id = id;
        x.mem.write<uint32_t>(ids + uint32_t(i * 4), id);
    }
}
void gl_deleteFramebuffers(Ctx& x, int32_t n, GuestPtr ids) {
    ContextData* c = currentCtx(x);
    SharegroupData* sg = currentSg(x, c);
    if (!sg || !ids || n <= 0) return;
    for (int32_t i = 0; i < n; ++i) {
        const uint32_t id = x.mem.read<uint32_t>(ids + uint32_t(i * 4));
        if (id) {
            sg->framebuffers.erase(id);
            if (c->framebuffer == id) c->framebuffer = 0;
        }
    }
}
void gl_bindFramebuffer(Ctx& x, uint32_t target, uint32_t fb) {
    ContextData* c = currentCtx(x);
    SharegroupData* sg = currentSg(x, c);
    if (!c || !sg) return;
    if (target == 0x8CA8 /* GL_READ_FRAMEBUFFER_APPLE */) c->readFramebuffer = fb;
    else {
        c->framebuffer = fb;
        if (target != 0x8CA9 /* GL_DRAW_FRAMEBUFFER_APPLE */) c->readFramebuffer = fb;
    }
    if (fb != 0) sg->framebuffers[fb].id = fb;
}
void gl_blitFramebuffer(Ctx& x, int32_t srcX0, int32_t srcY0, int32_t srcX1, int32_t srcY1,
                         int32_t dstX0, int32_t dstY0, int32_t dstX1, int32_t dstY1,
                         uint32_t mask, uint32_t filter) {
    (void)mask; (void)filter;
    ContextData* c = currentCtx(x);
    SharegroupData* sg = currentSg(x, c);
    if (!c || !sg) return;
    const uint32_t draw = c->framebuffer;
    c->framebuffer = c->readFramebuffer;
    const RenderTarget src = activeTarget(x, *c, *sg);
    c->framebuffer = draw;
    const RenderTarget dst = activeTarget(x, *c, *sg);
    if (!src.rgba || !dst.rgba || src.rgba == dst.rgba || src.width <= 0 || dst.width <= 0) return;

    const int sw = std::abs(srcX1 - srcX0);
    const int sh = std::abs(srcY1 - srcY0);
    const int dw = std::abs(dstX1 - dstX0);
    const int dh = std::abs(dstY1 - dstY0);
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;

    const int minSx = std::min(srcX0, srcX1);
    const int minSy = std::min(srcY0, srcY1);
    const int minDx = std::min(dstX0, dstX1);
    const int minDy = std::min(dstY0, dstY1);

    if (sw == dw && sh == dh && minSx >= 0 && minSy >= 0 && minSx + sw <= src.width &&
        minDx >= 0 && minDy >= 0 && minDx + dw <= dst.width) {
        for (int y = 0; y < dh; ++y) {
            std::memcpy(dst.rgba + (size_t(minDy + y) * dst.width + minDx) * 4,
                        src.rgba + (size_t(minSy + y) * src.width + minSx) * 4,
                        size_t(dw) * 4);
        }
        return;
    }

    for (int y = 0; y < dh; ++y) {
        const int dy = minDy + y;
        if (dy < 0 || dy >= dst.height) continue;
        const int sy = minSy + int(int64_t(y) * sh / dh);
        if (sy < 0 || sy >= src.height) continue;
        for (int xx = 0; xx < dw; ++xx) {
            const int dx = minDx + xx;
            if (dx < 0 || dx >= dst.width) continue;
            const int sx = minSx + int(int64_t(xx) * sw / dw);
            if (sx < 0 || sx >= src.width) continue;
            std::memcpy(dst.rgba + (size_t(dy) * dst.width + dx) * 4,
                        src.rgba + (size_t(sy) * src.width + sx) * 4, 4);
        }
    }
}

// APPLE_framebuffer_multisample: multisample buffers are ordinary buffers here (no MSAA),
// and resolving copies the read framebuffer's colour into the draw framebuffer.
void gl_renderbufferStorageMultisample(Ctx& x, uint32_t target, int32_t, uint32_t internalformat, int32_t width, int32_t height);
void gl_resolveMultisample(Ctx& x) {
    ContextData* c = currentCtx(x);
    SharegroupData* sg = currentSg(x, c);
    if (!c || !sg || c->readFramebuffer == c->framebuffer) return;
    const uint32_t draw = c->framebuffer;
    c->framebuffer = c->readFramebuffer;
    const RenderTarget src = activeTarget(x, *c, *sg);
    c->framebuffer = draw;
    const RenderTarget dst = activeTarget(x, *c, *sg);
    if (!src.rgba || !dst.rgba || src.rgba == dst.rgba || src.width <= 0 || dst.width <= 0) return;
    if (src.width == dst.width && src.height == dst.height) {
        std::memcpy(dst.rgba, src.rgba, size_t(dst.width) * size_t(dst.height) * 4);
        return;
    }
    for (int y = 0; y < dst.height; ++y) {
        const int sy = int(int64_t(y) * src.height / dst.height);
        for (int xx = 0; xx < dst.width; ++xx) {
            const int sx = int(int64_t(xx) * src.width / dst.width);
            std::memcpy(dst.rgba + (size_t(y) * dst.width + xx) * 4, src.rgba + (size_t(sy) * src.width + sx) * 4, 4);
        }
    }
}
bool gl_isFramebuffer(Ctx& x, uint32_t fb) {
    ContextData* c = currentCtx(x);
    SharegroupData* sg = currentSg(x, c);
    return sg && fb != 0 && sg->framebuffers.count(fb) > 0;
}
uint32_t gl_checkFramebufferStatus(Ctx&, uint32_t) { return GL_FRAMEBUFFER_COMPLETE; }
void gl_framebufferRenderbuffer(Ctx& x, uint32_t target, uint32_t attachment, uint32_t renderbuffertarget, uint32_t rb) {
    (void)target; (void)renderbuffertarget;
    ContextData* c = currentCtx(x);
    SharegroupData* sg = currentSg(x, c);
    if (!c || !sg || c->framebuffer == 0) return;
    Framebuffer& fb = sg->framebuffers[c->framebuffer];
    if (attachment == GL_COLOR_ATTACHMENT0) { fb.colorAttachmentType = GL_RENDERBUFFER; fb.colorAttachmentId = rb; }
    else if (attachment == GL_DEPTH_ATTACHMENT) { fb.depthAttachmentType = GL_RENDERBUFFER; fb.depthAttachmentId = rb; }
    else if (attachment == GL_STENCIL_ATTACHMENT) { fb.stencilAttachmentType = GL_RENDERBUFFER; fb.stencilAttachmentId = rb; }
}
void gl_framebufferTexture2D(Ctx& x, uint32_t target, uint32_t attachment, uint32_t textarget, uint32_t texture, int32_t level) {
    (void)target; (void)textarget; (void)level;
    ContextData* c = currentCtx(x);
    SharegroupData* sg = currentSg(x, c);
    if (!c || !sg || c->framebuffer == 0) return;
    Framebuffer& fb = sg->framebuffers[c->framebuffer];
    if (attachment == GL_COLOR_ATTACHMENT0) { fb.colorAttachmentType = GL_TEXTURE_2D; fb.colorAttachmentId = texture; }
}
void gl_getFramebufferAttachmentParameteriv(Ctx& x, uint32_t target, uint32_t attachment, uint32_t pname, GuestPtr params) {
    (void)target;
    ContextData* c = currentCtx(x);
    SharegroupData* sg = currentSg(x, c);
    if (!c || !sg || !params || c->framebuffer == 0) return;
    const Framebuffer& fb = sg->framebuffers[c->framebuffer];
    uint32_t type = 0, name = 0;
    if (attachment == GL_COLOR_ATTACHMENT0) { type = fb.colorAttachmentType; name = fb.colorAttachmentId; }
    else if (attachment == GL_DEPTH_ATTACHMENT) { type = fb.depthAttachmentType; name = fb.depthAttachmentId; }
    else if (attachment == GL_STENCIL_ATTACHMENT) { type = fb.stencilAttachmentType; name = fb.stencilAttachmentId; }
    if (pname == GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE) x.mem.write<int32_t>(params, int32_t(type));
    else if (pname == GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME) x.mem.write<int32_t>(params, int32_t(name));
    else x.mem.write<int32_t>(params, 0);
}

// ========================================================= Buffers & VAOs ===

void gl_genBuffers(Ctx& x, int32_t n, GuestPtr ids) {
    ContextData* c = currentCtx(x);
    SharegroupData* sg = currentSg(x, c);
    if (!sg || !ids || n <= 0) return;
    for (int32_t i = 0; i < n; ++i) {
        const uint32_t id = sg->nextBuffer++;
        sg->buffers[id].id = id;
        x.mem.write<uint32_t>(ids + uint32_t(i * 4), id);
    }
}
void gl_deleteBuffers(Ctx& x, int32_t n, GuestPtr ids) {
    ContextData* c = currentCtx(x);
    SharegroupData* sg = currentSg(x, c);
    if (!sg || !ids || n <= 0) return;
    for (int32_t i = 0; i < n; ++i) {
        const uint32_t id = x.mem.read<uint32_t>(ids + uint32_t(i * 4));
        if (id) {
            sg->buffers.erase(id);
            if (c->arrayBuffer == id) c->arrayBuffer = 0;
            if (currentVao(c).elementArrayBuffer == id) currentVao(c).elementArrayBuffer = 0;
        }
    }
}
void gl_bindBuffer(Ctx& x, uint32_t target, uint32_t buffer) {
    ContextData* c = currentCtx(x);
    SharegroupData* sg = currentSg(x, c);
    if (!c || !sg) return;
    if (target == GL_ARRAY_BUFFER) c->arrayBuffer = buffer;
    else if (target == GL_ELEMENT_ARRAY_BUFFER) {
        currentVao(c).elementArrayBuffer = buffer;
    }
    if (buffer != 0) sg->buffers[buffer].id = buffer;
}
bool gl_isBuffer(Ctx& x, uint32_t buffer) {
    ContextData* c = currentCtx(x);
    SharegroupData* sg = currentSg(x, c);
    return sg && buffer != 0 && sg->buffers.count(buffer) > 0;
}
void gl_bufferData(Ctx& x, uint32_t target, int32_t size, GuestPtr data, uint32_t usage) {
    ContextData* c = currentCtx(x);
    SharegroupData* sg = currentSg(x, c);
    if (!c || !sg || size < 0) return;
    const uint32_t bid = (target == GL_ELEMENT_ARRAY_BUFFER) ? currentVao(c).elementArrayBuffer : c->arrayBuffer;
    if (!bid) return;
    BufferObject& bo = sg->buffers[bid];
    bo.usage = usage;
    bo.data.assign(size_t(size), 0);
    if (data && size > 0) x.mem.copyOut(bo.data.data(), data, size_t(size));
}
void gl_bufferSubData(Ctx& x, uint32_t target, int32_t offset, int32_t size, GuestPtr data) {
    ContextData* c = currentCtx(x);
    SharegroupData* sg = currentSg(x, c);
    if (!c || !sg || offset < 0 || size <= 0 || !data) return;
    const uint32_t bid = (target == GL_ELEMENT_ARRAY_BUFFER) ? currentVao(c).elementArrayBuffer : c->arrayBuffer;
    if (!bid) return;
    BufferObject& bo = sg->buffers[bid];
    if (size_t(offset) + size_t(size) > bo.data.size()) bo.data.resize(size_t(offset) + size_t(size));
    x.mem.copyOut(bo.data.data() + offset, data, size_t(size));
}
void gl_getBufferParameteriv(Ctx& x, uint32_t target, uint32_t pname, GuestPtr params) {
    ContextData* c = currentCtx(x);
    SharegroupData* sg = currentSg(x, c);
    if (!c || !sg || !params) return;
    const uint32_t bid = (target == GL_ELEMENT_ARRAY_BUFFER) ? currentVao(c).elementArrayBuffer : c->arrayBuffer;
    if (!bid || !sg->buffers.count(bid)) { x.mem.write<int32_t>(params, 0); return; }
    const BufferObject& bo = sg->buffers[bid];
    if (pname == GL_BUFFER_SIZE) x.mem.write<int32_t>(params, int32_t(bo.data.size()));
    else if (pname == GL_BUFFER_USAGE) x.mem.write<int32_t>(params, int32_t(bo.usage));
}
GuestPtr gl_mapBufferOES(Ctx& x, uint32_t target, uint32_t) {
    ContextData* c = currentCtx(x);
    SharegroupData* sg = currentSg(x, c);
    if (!c || !sg) return 0;
    const uint32_t bid = (target == GL_ELEMENT_ARRAY_BUFFER) ? currentVao(c).elementArrayBuffer : c->arrayBuffer;
    if (!bid || !sg->buffers.count(bid)) return 0;
    BufferObject& bo = sg->buffers[bid];
    if (bo.data.empty()) bo.data.assign(4096, 0);
    if (!bo.mappedGuestAddr) {
        bo.mappedGuestAddr = x.proc.heap().alloc(uint32_t(bo.data.size()));
        x.mem.copyIn(bo.mappedGuestAddr, bo.data.data(), bo.data.size());
    }
    return bo.mappedGuestAddr;
}
GuestPtr gl_mapBufferRange(Ctx& x, uint32_t target, int32_t offset, int32_t length, uint32_t) {
    ContextData* c = currentCtx(x);
    SharegroupData* sg = currentSg(x, c);
    if (!c || !sg || offset < 0 || length < 0) return 0;
    const uint32_t bid = (target == GL_ELEMENT_ARRAY_BUFFER) ? currentVao(c).elementArrayBuffer : c->arrayBuffer;
    if (!bid || !sg->buffers.count(bid)) return 0;
    BufferObject& bo = sg->buffers[bid];
    const size_t needed = size_t(offset) + size_t(length > 0 ? length : 4);
    if (bo.data.size() < needed) bo.data.resize(needed, 0);
    if (bo.mappedGuestAddr && x.proc.heap().blockSize(bo.mappedGuestAddr) < bo.data.size()) {
        x.proc.heap().free(bo.mappedGuestAddr);
        bo.mappedGuestAddr = 0;
    }
    if (!bo.mappedGuestAddr) {
        bo.mappedGuestAddr = x.proc.heap().alloc(uint32_t(bo.data.size()));
        x.mem.copyIn(bo.mappedGuestAddr, bo.data.data(), bo.data.size());
    }
    return bo.mappedGuestAddr + uint32_t(offset);
}
void gl_flushMappedBufferRange(Ctx& x, uint32_t target, int32_t offset, int32_t length) {
    ContextData* c = currentCtx(x);
    SharegroupData* sg = currentSg(x, c);
    if (!c || !sg || offset < 0 || length <= 0) return;
    const uint32_t bid = (target == GL_ELEMENT_ARRAY_BUFFER) ? currentVao(c).elementArrayBuffer : c->arrayBuffer;
    if (!bid || !sg->buffers.count(bid)) return;
    BufferObject& bo = sg->buffers[bid];
    if (bo.mappedGuestAddr && size_t(offset) + size_t(length) <= bo.data.size())
        x.mem.copyOut(bo.data.data() + offset, bo.mappedGuestAddr + uint32_t(offset), size_t(length));
}
bool gl_unmapBufferOES(Ctx& x, uint32_t target) {
    ContextData* c = currentCtx(x);
    SharegroupData* sg = currentSg(x, c);
    if (!c || !sg) return false;
    const uint32_t bid = (target == GL_ELEMENT_ARRAY_BUFFER) ? currentVao(c).elementArrayBuffer : c->arrayBuffer;
    if (!bid || !sg->buffers.count(bid)) return false;
    BufferObject& bo = sg->buffers[bid];
    if (bo.mappedGuestAddr) {
        x.mem.copyOut(bo.data.data(), bo.mappedGuestAddr, bo.data.size());
        x.proc.heap().free(bo.mappedGuestAddr);
        bo.mappedGuestAddr = 0;
    }
    return true;
}

void gl_genVertexArraysOES(Ctx& x, int32_t n, GuestPtr arrays) {
    if (!arrays || n <= 0) return;
    static uint32_t nextVao = 1;
    for (int32_t i = 0; i < n; ++i) x.mem.write<uint32_t>(arrays + uint32_t(i * 4), nextVao++);
}
void gl_bindVertexArrayOES(Ctx& x, uint32_t array) { if (auto* c = currentCtx(x)) c->boundVao = array; }
void gl_deleteVertexArraysOES(Ctx&, int32_t, GuestPtr) {}
bool gl_isVertexArrayOES(Ctx&, uint32_t a) { return a != 0; }

// ================================================================ Textures ===

void gl_genTextures(Ctx& x, int32_t n, GuestPtr ids) {
    ContextData* c = currentCtx(x);
    SharegroupData* sg = currentSg(x, c);
    if (!sg || !ids || n <= 0) return;
    for (int32_t i = 0; i < n; ++i) {
        const uint32_t id = sg->nextTexture++;
        sg->textures[id].id = id;
        x.mem.write<uint32_t>(ids + uint32_t(i * 4), id);
    }
}
void gl_deleteTextures(Ctx& x, int32_t n, GuestPtr ids) {
    ContextData* c = currentCtx(x);
    SharegroupData* sg = currentSg(x, c);
    if (!sg || !ids || n <= 0) return;
    for (int32_t i = 0; i < n; ++i) {
        const uint32_t id = x.mem.read<uint32_t>(ids + uint32_t(i * 4));
        if (id) {
            sg->textures.erase(id);
            for (auto& bt : c->boundTextures) if (bt == id) bt = 0;
        }
    }
}
void gl_bindTexture(Ctx& x, uint32_t target, uint32_t tex) {
    (void)target;
    ContextData* c = currentCtx(x);
    SharegroupData* sg = currentSg(x, c);
    if (!c || !sg) return;
    c->boundTextures[c->activeTexture & 7] = tex;
    if (tex != 0) sg->textures[tex].id = tex;
}
bool gl_isTexture(Ctx& x, uint32_t tex) {
    ContextData* c = currentCtx(x);
    SharegroupData* sg = currentSg(x, c);
    return sg && tex != 0 && sg->textures.count(tex) > 0;
}
void gl_activeTexture(Ctx& x, uint32_t tex) {
    if (auto* c = currentCtx(x)) c->activeTexture = (tex >= GL_TEXTURE0) ? ((tex - GL_TEXTURE0) & 7) : (tex & 7);
}
void gl_clientActiveTexture(Ctx& x, uint32_t tex) {
    if (auto* c = currentCtx(x)) c->clientActiveTexture = (tex >= GL_TEXTURE0) ? ((tex - GL_TEXTURE0) & 7) : (tex & 7);
}

void unpackPixelsToRGBA(Ctx& x, int width, int height, uint32_t format, uint32_t type, int align, GuestPtr pixels, std::vector<uint8_t>& dst) {
    dst.assign(size_t(width) * size_t(height) * 4, 0);
    if (!pixels || width <= 0 || height <= 0) return;
    int bpp = 4;
    if (type == GL_UNSIGNED_SHORT_5_6_5 || type == GL_UNSIGNED_SHORT_4_4_4_4 || type == GL_UNSIGNED_SHORT_5_5_5_1) bpp = 2;
    else if (format == GL_RGB) bpp = 3;
    else if (format == GL_LUMINANCE_ALPHA) bpp = 2;
    else if (format == GL_ALPHA || format == GL_LUMINANCE) bpp = 1;

    const int rowBytes = ((width * bpp + align - 1) / align) * align;
    for (int y = 0; y < height; ++y) {
        const uint8_t* srcRow = x.mem.host(pixels + uint32_t(y * rowBytes));
        uint8_t* dstRow = &dst[size_t(y) * size_t(width) * 4];
        for (int px = 0; px < width; ++px) {
            uint8_t r = 255, g = 255, b = 255, a = 255;
            if (type == GL_UNSIGNED_SHORT_5_6_5) {
                uint16_t v; std::memcpy(&v, srcRow + px * 2, 2);
                r = uint8_t(((v >> 11) & 0x1F) * 255 / 31);
                g = uint8_t(((v >> 5) & 0x3F) * 255 / 63);
                b = uint8_t((v & 0x1F) * 255 / 31);
            } else if (type == GL_UNSIGNED_SHORT_4_4_4_4) {
                uint16_t v; std::memcpy(&v, srcRow + px * 2, 2);
                r = uint8_t(((v >> 12) & 0xF) * 17);
                g = uint8_t(((v >> 8) & 0xF) * 17);
                b = uint8_t(((v >> 4) & 0xF) * 17);
                a = uint8_t((v & 0xF) * 17);
            } else if (type == GL_UNSIGNED_SHORT_5_5_5_1) {
                uint16_t v; std::memcpy(&v, srcRow + px * 2, 2);
                r = uint8_t(((v >> 11) & 0x1F) * 255 / 31);
                g = uint8_t(((v >> 6) & 0x1F) * 255 / 31);
                b = uint8_t(((v >> 1) & 0x1F) * 255 / 31);
                a = (v & 1) ? 255 : 0;
            } else if (format == GL_RGBA) {
                r = srcRow[px * 4 + 0]; g = srcRow[px * 4 + 1]; b = srcRow[px * 4 + 2]; a = srcRow[px * 4 + 3];
            } else if (format == GL_BGRA_EXT) {
                b = srcRow[px * 4 + 0]; g = srcRow[px * 4 + 1]; r = srcRow[px * 4 + 2]; a = srcRow[px * 4 + 3];
            } else if (format == GL_RGB) {
                r = srcRow[px * 3 + 0]; g = srcRow[px * 3 + 1]; b = srcRow[px * 3 + 2]; a = 255;
            } else if (format == GL_LUMINANCE_ALPHA) {
                r = g = b = srcRow[px * 2 + 0]; a = srcRow[px * 2 + 1];
            } else if (format == GL_LUMINANCE) {
                r = g = b = srcRow[px]; a = 255;
            } else if (format == GL_ALPHA) {
                r = g = b = 255; a = srcRow[px];
            }
            dstRow[px * 4 + 0] = r;
            dstRow[px * 4 + 1] = g;
            dstRow[px * 4 + 2] = b;
            dstRow[px * 4 + 3] = a;
        }
    }
}

void gl_texImage2D(Ctx& x, uint32_t target, int32_t level, int32_t internalformat, int32_t width, int32_t height,
                   int32_t border, uint32_t format, uint32_t type, GuestPtr pixels) {
    (void)target; (void)internalformat; (void)border;
    if (level != 0 || width < 0 || height < 0) return;
    ContextData* c = currentCtx(x);
    SharegroupData* sg = currentSg(x, c);
    if (!c || !sg) return;
    const uint32_t tid = c->boundTextures[c->activeTexture & 7];
    if (!tid) return;
    Texture2D& tx = sg->textures[tid];
    tx.width = width;
    tx.height = height;
    tx.format = format;
    unpackPixelsToRGBA(x, width, height, format, type, c->unpackAlignment, pixels, tx.rgba);
    // Debugging aid: HVM_DUMP_TEX=<dir> writes every uploaded texture as raw RGBA.
    if (const char* dir = std::getenv("HVM_DUMP_TEX")) {
        static int n = 0;
        char name[512];
        std::snprintf(name, sizeof name, "%s/tex%03d_%dx%d_f%x_t%x.rgba", dir, n++, width, height, format, type);
        if (std::FILE* f = std::fopen(name, "wb")) { std::fwrite(tx.rgba.data(), 1, tx.rgba.size(), f); std::fclose(f); }
    }
}

void gl_texSubImage2D(Ctx& x, uint32_t target, int32_t level, int32_t xoffset, int32_t yoffset, int32_t width, int32_t height,
                      uint32_t format, uint32_t type, GuestPtr pixels) {
    (void)target;
    if (level != 0 || width <= 0 || height <= 0 || !pixels) return;
    ContextData* c = currentCtx(x);
    SharegroupData* sg = currentSg(x, c);
    if (!c || !sg) return;
    const uint32_t tid = c->boundTextures[c->activeTexture & 7];
    if (!tid || !sg->textures.count(tid)) return;
    Texture2D& tx = sg->textures[tid];
    std::vector<uint8_t> sub;
    unpackPixelsToRGBA(x, width, height, format, type, c->unpackAlignment, pixels, sub);
    for (int y = 0; y < height; ++y) {
        const int dy = yoffset + y;
        if (dy < 0 || dy >= tx.height) continue;
        for (int px = 0; px < width; ++px) {
            const int dx = xoffset + px;
            if (dx < 0 || dx >= tx.width) continue;
            std::memcpy(&tx.rgba[(size_t(dy) * size_t(tx.width) + size_t(dx)) * 4],
                        &sub[(size_t(y) * size_t(width) + size_t(px)) * 4], 4);
        }
    }
}

void gl_compressedTexImage2D(Ctx& x, uint32_t target, int32_t level, uint32_t internalformat, int32_t width, int32_t height,
                             int32_t border, int32_t imageSize, GuestPtr data) {
    (void)target; (void)border;
    if (level != 0 || width <= 0 || height <= 0) return;   // no mipmaps: level 0 only
    ContextData* c = currentCtx(x);
    SharegroupData* sg = currentSg(x, c);
    if (!c || !sg) return;
    const uint32_t tid = c->boundTextures[c->activeTexture & 7];
    if (!tid) return;
    Texture2D& tx = sg->textures[tid];
    tx.width = width;
    tx.height = height;
    tx.format = GL_RGBA;
    // GL_IMG_texture_compression_pvrtc: RGB/RGBA x 4/2 bpp.
    const bool pvrtc = internalformat >= 0x8C00 && internalformat <= 0x8C03;
    if (pvrtc && data && imageSize > 0 &&
        Graphics::decodePvrtc(x.mem.host(data), size_t(imageSize), width, height,
                              internalformat == 0x8C01 || internalformat == 0x8C03, internalformat >= 0x8C02, tx.rgba))
        return;
    if (!pvrtc) x.proc.reportMissing("glCompressedTexImage2D format 0x" + [&] { char b[16]; std::snprintf(b, sizeof b, "%x", internalformat); return std::string(b); }());
    tx.rgba.assign(size_t(width) * size_t(height) * 4, 255);
}

void gl_compressedTexSubImage2D(Ctx& x, uint32_t target, int32_t level, int32_t xoffset, int32_t yoffset,
                                int32_t width, int32_t height, uint32_t format, int32_t imageSize, GuestPtr data) {
    (void)target;
    if (level != 0 || width <= 0 || height <= 0 || !data || imageSize <= 0) return;
    ContextData* c = currentCtx(x);
    SharegroupData* sg = currentSg(x, c);
    if (!c || !sg) return;
    const uint32_t tid = c->boundTextures[c->activeTexture & 7];
    if (!tid || !sg->textures.count(tid)) return;
    Texture2D& tx = sg->textures[tid];
    if (tx.rgba.size() < size_t(tx.width) * size_t(tx.height) * 4 && tx.width > 0 && tx.height > 0)
        tx.rgba.assign(size_t(tx.width) * size_t(tx.height) * 4, 0);
    const bool pvrtc = format >= 0x8C00 && format <= 0x8C03;
    std::vector<uint8_t> sub;
    if (pvrtc && Graphics::decodePvrtc(x.mem.host(data), size_t(imageSize), width, height,
                                       format == 0x8C01 || format == 0x8C03, format >= 0x8C02, sub)) {
        for (int y = 0; y < height; ++y) {
            const int dy = yoffset + y;
            if (dy < 0 || dy >= tx.height) continue;
            for (int px = 0; px < width; ++px) {
                const int dx = xoffset + px;
                if (dx < 0 || dx >= tx.width) continue;
                std::memcpy(&tx.rgba[(size_t(dy) * size_t(tx.width) + size_t(dx)) * 4],
                            &sub[(size_t(y) * size_t(width) + size_t(px)) * 4], 4);
            }
        }
    }
}

void gl_texParameteri(Ctx& x, uint32_t target, uint32_t pname, int32_t param) {
    (void)target;
    ContextData* c = currentCtx(x);
    SharegroupData* sg = currentSg(x, c);
    if (!c || !sg) return;
    const uint32_t tid = c->boundTextures[c->activeTexture & 7];
    if (!tid) return;
    Texture2D& tx = sg->textures[tid];
    if (pname == GL_TEXTURE_MIN_FILTER) tx.minFilter = uint32_t(param);
    else if (pname == GL_TEXTURE_MAG_FILTER) tx.magFilter = uint32_t(param);
    else if (pname == GL_TEXTURE_WRAP_S) tx.wrapS = uint32_t(param);
    else if (pname == GL_TEXTURE_WRAP_T) tx.wrapT = uint32_t(param);
}
void gl_texParameterf(Ctx& x, uint32_t target, uint32_t pname, float param) { gl_texParameteri(x, target, pname, int32_t(param)); }
void gl_texParameterx(Ctx& x, uint32_t target, uint32_t pname, int32_t param) { gl_texParameteri(x, target, pname, param); }
void gl_texParameteriv(Ctx& x, uint32_t target, uint32_t pname, GuestPtr params) {
    if (!params) return;
    if (pname == GL_TEXTURE_CROP_RECT_OES) {
        ContextData* c = currentCtx(x);
        SharegroupData* sg = currentSg(x, c);
        if (c && sg && c->boundTextures[c->activeTexture & 7]) {
            Texture2D& tx = sg->textures[c->boundTextures[c->activeTexture & 7]];
            for (int i = 0; i < 4; ++i) tx.cropRect[i] = x.mem.read<int32_t>(params + uint32_t(i * 4));
        }
        return;
    }
    gl_texParameteri(x, target, pname, x.mem.read<int32_t>(params));
}
void gl_texParameterfv(Ctx& x, uint32_t target, uint32_t pname, GuestPtr params) {
    if (!params) return;
    if (pname == GL_TEXTURE_CROP_RECT_OES) {
        ContextData* c = currentCtx(x);
        SharegroupData* sg = currentSg(x, c);
        if (c && sg && c->boundTextures[c->activeTexture & 7]) {
            Texture2D& tx = sg->textures[c->boundTextures[c->activeTexture & 7]];
            for (int i = 0; i < 4; ++i) tx.cropRect[i] = int(x.mem.read<float>(params + uint32_t(i * 4)));
        }
        return;
    }
    gl_texParameterf(x, target, pname, x.mem.read<float>(params));
}

// ========================================== GLES 1.1 Matrix & Fixed State ===

void gl_matrixMode(Ctx& x, uint32_t mode) { if (auto* c = currentCtx(x)) c->matrixMode = mode; }
void gl_loadIdentity(Ctx& x) { if (auto* c = currentCtx(x)) c->currentMatrixStack().back() = Mat4::identity(); }
void gl_loadMatrixf(Ctx& x, GuestPtr m) {
    if (auto* c = currentCtx(x); c && m) x.mem.copyOut(c->currentMatrixStack().back().m, m, 64);
}
void gl_loadMatrixx(Ctx& x, GuestPtr m) {
    if (auto* c = currentCtx(x); c && m) {
        Mat4& dst = c->currentMatrixStack().back();
        for (int i = 0; i < 16; ++i) dst.m[i] = x.mem.read<int32_t>(m + uint32_t(i * 4)) / 65536.0f;
    }
}
void gl_multMatrixf(Ctx& x, GuestPtr m) {
    if (auto* c = currentCtx(x); c && m) {
        Mat4 rhs;
        x.mem.copyOut(rhs.m, m, 64);
        c->currentMatrixStack().back() = c->currentMatrixStack().back() * rhs;
    }
}
void gl_multMatrixx(Ctx& x, GuestPtr m) {
    if (auto* c = currentCtx(x); c && m) {
        Mat4 rhs;
        for (int i = 0; i < 16; ++i) rhs.m[i] = x.mem.read<int32_t>(m + uint32_t(i * 4)) / 65536.0f;
        c->currentMatrixStack().back() = c->currentMatrixStack().back() * rhs;
    }
}
void gl_pushMatrix(Ctx& x) {
    if (auto* c = currentCtx(x)) {
        auto& st = c->currentMatrixStack();
        if (st.size() < 64) st.push_back(st.back());
    }
}
void gl_popMatrix(Ctx& x) {
    if (auto* c = currentCtx(x)) {
        auto& st = c->currentMatrixStack();
        if (st.size() > 1) st.pop_back();
    }
}
void gl_orthof(Ctx& x, float l, float r, float b, float t, float n, float f) {
    auto* c = currentCtx(x);
    if (!c || r == l || t == b || f == n) return;
    Mat4 m = Mat4::identity();
    m.m[0] = 2.0f / (r - l);
    m.m[5] = 2.0f / (t - b);
    m.m[10] = -2.0f / (f - n);
    m.m[12] = -(r + l) / (r - l);
    m.m[13] = -(t + b) / (t - b);
    m.m[14] = -(f + n) / (f - n);
    c->currentMatrixStack().back() = c->currentMatrixStack().back() * m;
}
void gl_orthox(Ctx& x, int32_t l, int32_t r, int32_t b, int32_t t, int32_t n, int32_t f) {
    gl_orthof(x, l / 65536.0f, r / 65536.0f, b / 65536.0f, t / 65536.0f, n / 65536.0f, f / 65536.0f);
}
void gl_frustumf(Ctx& x, float l, float r, float b, float t, float n, float f) {
    auto* c = currentCtx(x);
    if (!c || n <= 0.0f || f <= 0.0f || r == l || t == b || f == n) return;
    Mat4 m{};
    m.m[0] = (2.0f * n) / (r - l);
    m.m[5] = (2.0f * n) / (t - b);
    m.m[8] = (r + l) / (r - l);
    m.m[9] = (t + b) / (t - b);
    m.m[10] = -(f + n) / (f - n);
    m.m[11] = -1.0f;
    m.m[14] = -(2.0f * f * n) / (f - n);
    c->currentMatrixStack().back() = c->currentMatrixStack().back() * m;
}
void gl_frustumx(Ctx& x, int32_t l, int32_t r, int32_t b, int32_t t, int32_t n, int32_t f) {
    gl_frustumf(x, l / 65536.0f, r / 65536.0f, b / 65536.0f, t / 65536.0f, n / 65536.0f, f / 65536.0f);
}
void gl_translatef(Ctx& x, float tx, float ty, float tz) {
    if (auto* c = currentCtx(x)) {
        Mat4 m = Mat4::identity();
        m.m[12] = tx; m.m[13] = ty; m.m[14] = tz;
        c->currentMatrixStack().back() = c->currentMatrixStack().back() * m;
    }
}
void gl_translatex(Ctx& x, int32_t tx, int32_t ty, int32_t tz) { gl_translatef(x, tx / 65536.0f, ty / 65536.0f, tz / 65536.0f); }
void gl_scalef(Ctx& x, float sx, float sy, float sz) {
    if (auto* c = currentCtx(x)) {
        Mat4 m = Mat4::identity();
        m.m[0] = sx; m.m[5] = sy; m.m[10] = sz;
        c->currentMatrixStack().back() = c->currentMatrixStack().back() * m;
    }
}
void gl_scalex(Ctx& x, int32_t sx, int32_t sy, int32_t sz) { gl_scalef(x, sx / 65536.0f, sy / 65536.0f, sz / 65536.0f); }
void gl_rotatef(Ctx& x, float angle, float rx, float ry, float rz) {
    auto* c = currentCtx(x);
    if (!c) return;
    const float len = std::sqrt(rx * rx + ry * ry + rz * rz);
    if (len <= 1e-6f) return;
    rx /= len; ry /= len; rz /= len;
    const float rad = angle * 3.14159265358979f / 180.0f;
    const float co = std::cos(rad), si = std::sin(rad), ic = 1.0f - co;
    Mat4 m = Mat4::identity();
    m.m[0] = rx * rx * ic + co;      m.m[4] = rx * ry * ic - rz * si; m.m[8]  = rx * rz * ic + ry * si;
    m.m[1] = ry * rx * ic + rz * si; m.m[5] = ry * ry * ic + co;      m.m[9]  = ry * rz * ic - rx * si;
    m.m[2] = rz * rx * ic - ry * si; m.m[6] = rz * ry * ic + rx * si; m.m[10] = rz * rz * ic + co;
    c->currentMatrixStack().back() = c->currentMatrixStack().back() * m;
}
void gl_rotatex(Ctx& x, int32_t angle, int32_t rx, int32_t ry, int32_t rz) {
    gl_rotatef(x, angle / 65536.0f, rx / 65536.0f, ry / 65536.0f, rz / 65536.0f);
}

void gl_color4f(Ctx& x, float r, float g, float b, float a) { if (auto* c = currentCtx(x)) c->currentColor = {r, g, b, a}; }
void gl_color4x(Ctx& x, int32_t r, int32_t g, int32_t b, int32_t a) { gl_color4f(x, r / 65536.0f, g / 65536.0f, b / 65536.0f, a / 65536.0f); }
void gl_color4ub(Ctx& x, uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    gl_color4f(x, (r & 0xFF) / 255.0f, (g & 0xFF) / 255.0f, (b & 0xFF) / 255.0f, (a & 0xFF) / 255.0f);
}
void gl_normal3f(Ctx& x, float nx, float ny, float nz) { if (auto* c = currentCtx(x)) c->currentNormal = {nx, ny, nz, 0}; }
void gl_normal3x(Ctx& x, int32_t nx, int32_t ny, int32_t nz) { gl_normal3f(x, nx / 65536.0f, ny / 65536.0f, nz / 65536.0f); }

void gl_enableClientState(Ctx& x, uint32_t array) {
    auto* c = currentCtx(x);
    if (!c) return;
    if (array == GL_VERTEX_ARRAY) currentVao(c).vertexPtr.enabled = true;
    else if (array == GL_COLOR_ARRAY) currentVao(c).colorPtr.enabled = true;
    else if (array == GL_NORMAL_ARRAY) currentVao(c).normalPtr.enabled = true;
    else if (array == GL_TEXTURE_COORD_ARRAY) currentVao(c).texCoordPtr[c->clientActiveTexture & 7].enabled = true;
    else if (array == GL_POINT_SIZE_ARRAY_OES) currentVao(c).pointSizePtr.enabled = true;
}
void gl_disableClientState(Ctx& x, uint32_t array) {
    auto* c = currentCtx(x);
    if (!c) return;
    if (array == GL_VERTEX_ARRAY) currentVao(c).vertexPtr.enabled = false;
    else if (array == GL_COLOR_ARRAY) currentVao(c).colorPtr.enabled = false;
    else if (array == GL_NORMAL_ARRAY) currentVao(c).normalPtr.enabled = false;
    else if (array == GL_TEXTURE_COORD_ARRAY) currentVao(c).texCoordPtr[c->clientActiveTexture & 7].enabled = false;
    else if (array == GL_POINT_SIZE_ARRAY_OES) currentVao(c).pointSizePtr.enabled = false;
}
void gl_vertexPointer(Ctx& x, int32_t size, uint32_t type, int32_t stride, GuestPtr ptr) {
    if (auto* c = currentCtx(x)) { currentVao(c).vertexPtr = {currentVao(c).vertexPtr.enabled, size, type, false, stride, ptr, c->arrayBuffer}; }
}
void gl_colorPointer(Ctx& x, int32_t size, uint32_t type, int32_t stride, GuestPtr ptr) {
    if (auto* c = currentCtx(x)) { currentVao(c).colorPtr = {currentVao(c).colorPtr.enabled, size, type, type != GL_FLOAT && type != GL_FIXED, stride, ptr, c->arrayBuffer}; }
}
void gl_texCoordPointer(Ctx& x, int32_t size, uint32_t type, int32_t stride, GuestPtr ptr) {
    if (auto* c = currentCtx(x)) {
        auto& tp = currentVao(c).texCoordPtr[c->clientActiveTexture & 7];
        tp = {tp.enabled, size, type, false, stride, ptr, c->arrayBuffer};
    }
}
void gl_normalPointer(Ctx& x, uint32_t type, int32_t stride, GuestPtr ptr) {
    if (auto* c = currentCtx(x)) { currentVao(c).normalPtr = {currentVao(c).normalPtr.enabled, 3, type, true, stride, ptr, c->arrayBuffer}; }
}
void gl_pointSizePointerOES(Ctx& x, uint32_t type, int32_t stride, GuestPtr ptr) {
    if (auto* c = currentCtx(x)) { currentVao(c).pointSizePtr = {currentVao(c).pointSizePtr.enabled, 1, type, false, stride, ptr, c->arrayBuffer}; }
}

// =================================== GLES 2.0 Shaders, Programs & Uniforms ===

uint32_t gl_createShader(Ctx& x, uint32_t type) {
    ContextData* c = currentCtx(x);
    SharegroupData* sg = currentSg(x, c);
    if (!sg) return 0;
    const uint32_t id = sg->nextShader++;
    sg->shaders[id] = {id, type, "", false, "", {}};
    return id;
}
void gl_deleteShader(Ctx& x, uint32_t shader) {
    if (auto* sg = currentSg(x, currentCtx(x))) sg->shaders.erase(shader);
}
bool gl_isShader(Ctx& x, uint32_t shader) {
    auto* sg = currentSg(x, currentCtx(x));
    return sg && shader != 0 && sg->shaders.count(shader) > 0;
}
void gl_shaderSource(Ctx& x, uint32_t shader, int32_t count, GuestPtr strings, GuestPtr lengths) {
    auto* sg = currentSg(x, currentCtx(x));
    if (!sg || !sg->shaders.count(shader) || count <= 0 || !strings) return;
    std::string src;
    for (int32_t i = 0; i < count; ++i) {
        const uint32_t sptr = x.mem.read<uint32_t>(strings + uint32_t(i * 4));
        if (!sptr) continue;
        if (lengths) {
            const int32_t len = x.mem.read<int32_t>(lengths + uint32_t(i * 4));
            if (len >= 0) {
                src.append(reinterpret_cast<const char*>(x.mem.host(sptr)), size_t(len));
                continue;
            }
        }
        src += x.proc.readCString(sptr);
    }
    if (const char* dir = std::getenv("HVM_DUMP_SHADERS")) {
        std::error_code error;
        std::filesystem::create_directories(dir, error);
        const auto path = std::filesystem::path(dir) / ("shader-" + std::to_string(shader) +
            (sg->shaders[shader].type == GL_VERTEX_SHADER ? ".vert" : ".frag"));
        std::ofstream out(path, std::ios::binary);
        out << src;
        if (!out) x.proc.logf(LogLevel::Warning, "Cannot dump shader %u", shader);
    }
    sg->shaders[shader].source = std::move(src);
}
void gl_compileShader(Ctx& x, uint32_t shader) {
    if (auto* sg = currentSg(x, currentCtx(x)); sg && sg->shaders.count(shader)) {
        auto& sh = sg->shaders[shader];
        sh.compiled = sh.executable.compile(sh.source, sh.type == GL_VERTEX_SHADER);
        sh.infoLog = sh.compiled && !sh.executable.warnings.empty() ? sh.executable.warnings : sh.executable.error;
        if (!sh.compiled) x.proc.logf(LogLevel::Warning, "GLSL shader %u: %s", shader, sh.infoLog.c_str());
    }
}
void writeGlInfoLog(Ctx& x, const std::string& log, int32_t maxLen, GuestPtr length, GuestPtr infoLog) {
    const auto n = maxLen > 0 && infoLog ? std::min(log.size(), size_t(maxLen - 1)) : 0;
    if (length) x.mem.write<int32_t>(length, int32_t(n));
    if (infoLog && maxLen > 0) {
        x.mem.copyIn(infoLog, log.data(), n);
        x.mem.write<uint8_t>(infoLog + uint32_t(n), 0);
    }
}
void gl_getShaderiv(Ctx& x, uint32_t shader, uint32_t pname, GuestPtr params) {
    auto* sg = currentSg(x, currentCtx(x));
    if (!sg || !params || !sg->shaders.count(shader)) return;
    const ShaderObject& sh = sg->shaders[shader];
    int32_t v = 0;
    if (pname == GL_COMPILE_STATUS) v = sh.compiled ? 1 : 0;
    else if (pname == GL_INFO_LOG_LENGTH) v = sh.infoLog.empty() ? 0 : int32_t(sh.infoLog.size() + 1);
    else if (pname == GL_SHADER_SOURCE_LENGTH) v = int32_t(sh.source.size() + 1);
    x.mem.write<int32_t>(params, v);
}
void gl_getShaderInfoLog(Ctx& x, uint32_t shader, int32_t maxLen, GuestPtr length, GuestPtr infoLog) {
    auto* sg = currentSg(x, currentCtx(x));
    if (sg && sg->shaders.count(shader)) writeGlInfoLog(x, sg->shaders.at(shader).infoLog, maxLen, length, infoLog);
}
void gl_getShaderPrecisionFormat(Ctx& x, uint32_t, uint32_t, GuestPtr range, GuestPtr precision) {
    if (range) { x.mem.write<int32_t>(range, 127); x.mem.write<int32_t>(range + 4, 127); }
    if (precision) x.mem.write<int32_t>(precision, 23);
}

uint32_t gl_createProgram(Ctx& x) {
    auto* sg = currentSg(x, currentCtx(x));
    if (!sg) return 0;
    std::lock_guard<std::mutex> lk(sg->programMu);
    const uint32_t id = sg->nextProgram++;
    sg->programs[id].id = id;
    return id;
}
bool programIsCurrent(Ctx& x, uint32_t prog) {
    auto& st = GLS(x);
    std::lock_guard<std::mutex> lk(st.mu);
    for (const auto& [tid, cid] : st.currentByThread) {
        (void)tid;
        const auto* c = rt(x).hostIf<ContextData>(cid);
        if (c && c->currentProgram == prog) return true;
    }
    const auto* fallback = rt(x).hostIf<ContextData>(st.lastActiveContext);
    return fallback && fallback->currentProgram == prog;
}
void collectDeletedProgram(Ctx& x, SharegroupData* sg, uint32_t prog) {
    if (!sg || !prog) return;
    auto it = sg->programs.find(prog);
    if (it != sg->programs.end() && it->second.pendingDelete && !programIsCurrent(x, prog))
        sg->programs.erase(it);
}
void gl_deleteProgram(Ctx& x, uint32_t prog) {
    ContextData* c = currentCtx(x);
    SharegroupData* sg = currentSg(x, c);
    if (!sg || !prog) return;
    std::lock_guard<std::mutex> lk(sg->programMu);
    auto it = sg->programs.find(prog);
    if (it == sg->programs.end()) return;
    // GLES defers deletion until the program is no longer current in any
    // context. In particular, deleting does not unbind it from this context.
    it->second.pendingDelete = true;
    collectDeletedProgram(x, sg, prog);
}
bool gl_isProgram(Ctx& x, uint32_t prog) {
    auto* sg = currentSg(x, currentCtx(x));
    if (!sg || !prog) return false;
    auto it = sg->programs.find(prog);
    return it != sg->programs.end() && !it->second.pendingDelete;
}
void gl_attachShader(Ctx& x, uint32_t prog, uint32_t shader) {
    auto* sg = currentSg(x, currentCtx(x));
    if (!sg || !sg->programs.count(prog)) return;
    auto& v = sg->programs[prog].shaders;
    if (std::find(v.begin(), v.end(), shader) == v.end()) v.push_back(shader);
}
void gl_detachShader(Ctx& x, uint32_t prog, uint32_t shader) {
    auto* sg = currentSg(x, currentCtx(x));
    if (!sg || !sg->programs.count(prog)) return;
    auto& v = sg->programs[prog].shaders;
    v.erase(std::remove(v.begin(), v.end(), shader), v.end());
}
void gl_bindAttribLocation(Ctx& x, uint32_t prog, uint32_t index, GuestPtr name) {
    auto* sg = currentSg(x, currentCtx(x));
    if (!sg || !sg->programs.count(prog) || !name || index >= 16) return;
    sg->programs[prog].boundAttribLocations[x.proc.readCString(name)] = int(index);
}

uint32_t shaderGlType(Shader::Type t) {
    if (t.sampler) return t.cube ? GL_SAMPLER_CUBE : GL_SAMPLER_2D;
    if (t.matrix) return t.matrix == 2 ? GL_FLOAT_MAT2 : t.matrix == 3 ? GL_FLOAT_MAT3 : GL_FLOAT_MAT4;
    if (t.kind == Shader::Kind::Int)
        return t.width == 1 ? GL_INT : t.width == 2 ? GL_INT_VEC2 : t.width == 3 ? GL_INT_VEC3 : GL_INT_VEC4;
    if (t.kind == Shader::Kind::Bool)
        return t.width == 1 ? GL_BOOL : t.width == 2 ? GL_BOOL_VEC2 : t.width == 3 ? GL_BOOL_VEC3 : GL_BOOL_VEC4;
    return t.width == 1 ? GL_FLOAT : t.width == 2 ? GL_FLOAT_VEC2 : t.width == 3 ? GL_FLOAT_VEC3 : GL_FLOAT_VEC4;
}
struct UniformLeaf { std::string name; Shader::Type type; int slot = 0, count = 1; bool array = false; };
void collectUniformLeaves(std::vector<UniformLeaf>& out, const std::string& name, const Shader::Type& type, int slot, int count, bool array) {
    if (!type.fields.empty()) {
        for (int i = 0; i < count; ++i) {
            const std::string prefix = array ? name + "[" + std::to_string(i) + "]" : name;
            const int base = slot + i * type.width;
            for (const auto& f : type.fields) {
                Shader::Type ft;
                ft.width = f.width; ft.matrix = f.matrix; ft.sampler = f.sampler; ft.fields = f.fields;
                ft.kind = f.kind; ft.cube = f.cube;
                collectUniformLeaves(out, prefix + "." + f.name, ft, base + f.offset, f.count, f.array);
            }
        }
        return;
    }
    out.push_back({name, type, slot, count, array});
}
void gl_linkProgram(Ctx& x, uint32_t prog) {
    auto* sg = currentSg(x, currentCtx(x));
    if (!sg || !sg->programs.count(prog)) return;
    ProgramObject& p = sg->programs[prog];
    p.linked = false; p.infoLog.clear();
    p.attribs.clear(); p.uniforms.clear(); p.uniformValues.clear();
    p.vertexInputs.clear(); p.vertexUniforms.clear(); p.fragmentUniforms.clear(); p.varyings.clear(); p.varyingWidth = 0;
    bool vertex = false, fragment = false;
    auto fail = [&](const std::string& message) { p.infoLog = message; x.proc.logf(LogLevel::Warning, "GLSL program %u: %s", prog, message.c_str()); };
    for (auto sid : p.shaders) {
        auto it = sg->shaders.find(sid);
        if (it == sg->shaders.end() || !it->second.compiled) { fail("attached shader not compiled"); return; }
        auto& sh = it->second;
        if (sh.type == GL_VERTEX_SHADER && !vertex) { p.vertex = sh.executable; vertex = true; }
        else if (sh.type == GL_FRAGMENT_SHADER && !fragment) { p.fragment = sh.executable; fragment = true; }
        else { fail("exactly one shader per stage supported"); return; }
    }
    if (!vertex || !fragment) { fail("both shader stages required"); return; }
    uint32_t used = 0;
    for (auto& v : p.vertex.variables) {
        if (v.storage != Shader::Storage::Attribute) continue;
        auto bound = p.boundAttribLocations.find(v.name);
        if (bound != p.boundAttribLocations.end()) {
            if (used & (1u << bound->second)) { fail("aliased attribute locations unsupported"); return; }
            used |= 1u << bound->second;
        }
    }
    for (auto& v : p.vertex.variables) {
        if (v.storage != Shader::Storage::Attribute) continue;
        auto bound = p.boundAttribLocations.find(v.name);
        int loc = bound != p.boundAttribLocations.end() ? bound->second : 0;
        if (bound == p.boundAttribLocations.end()) {
            while (loc < 16 && (used & (1u << loc))) ++loc;
            if (loc == 16) { fail("too many attributes"); return; }
            used |= 1u << loc;
        }
        p.attribs.push_back({v.name, shaderGlType(v.type), 1, loc});
        p.vertexInputs.push_back({v.slot, loc, v.type.width, false});
    }
    int nextLoc = 0;
    for (auto* stage : {&p.vertex, &p.fragment}) {
        auto& bindings = stage == &p.vertex ? p.vertexUniforms : p.fragmentUniforms;
        for (auto& v : stage->variables) {
            if (v.storage != Shader::Storage::Uniform) continue;
            std::vector<UniformLeaf> leaves;
            collectUniformLeaves(leaves, v.name, v.type, v.slot, v.count, v.array);
            for (auto& leaf : leaves) {
                auto it = std::find_if(p.uniforms.begin(), p.uniforms.end(), [&](auto& u) { return u.name == leaf.name; });
                int loc;
                if (it == p.uniforms.end()) {
                    loc = nextLoc;
                    nextLoc += leaf.count;
                    p.uniforms.push_back({leaf.name, shaderGlType(leaf.type), leaf.count, loc, leaf.array});
                    for (int e = 0; e < leaf.count; ++e) {
                        auto& uv = p.uniformValues[loc + e];
                        uv.type = shaderGlType(leaf.type);
                        uv.span = leaf.count - e;
                    }
                } else {
                    if (it->type != shaderGlType(leaf.type) || it->size != leaf.count || it->array != leaf.array) {
                        fail("uniform type mismatch: " + leaf.name); return;
                    }
                    loc = it->location;
                }
                for (int e = 0; e < leaf.count; ++e)
                    bindings.push_back({leaf.slot + e * leaf.type.width, loc + e, leaf.type.width, leaf.type.sampler, v.memory});
            }
        }
    }
    for (auto& f : p.fragment.variables) {
        if (f.storage != Shader::Storage::Varying) continue;
        auto* v = p.vertex.find(f.name);
        if (!v || v->storage != Shader::Storage::Varying || v->type != f.type || v->count != f.count) { fail("varying mismatch: " + f.name); return; }
        int width = f.type.width * f.count;
        if (p.varyingWidth + width > 64) { fail("varying limit exceeded (64 floats)"); return; }
        p.varyings.push_back({v->slot, f.slot, width, p.varyingWidth});
        p.varyingWidth += width;
    }
    p.linked = true;
}

void gl_useProgram(Ctx& x, uint32_t prog) {
    auto* c = currentCtx(x);
    auto* sg = currentSg(x, c);
    if (!c || !sg) return;
    std::lock_guard<std::mutex> lk(sg->programMu);
    auto it = sg->programs.find(prog);
    if (prog && (it == sg->programs.end() || it->second.pendingDelete || !it->second.linked)) { c->lastError = GL_INVALID_OPERATION; return; }
    const uint32_t previous = c->currentProgram;
    c->currentProgram = prog;
    if (previous != prog) collectDeletedProgram(x, sg, previous);
}
void gl_validateProgram(Ctx&, uint32_t) {}
void gl_getProgramiv(Ctx& x, uint32_t prog, uint32_t pname, GuestPtr params) {
    auto* sg = currentSg(x, currentCtx(x));
    if (!sg || !params || !sg->programs.count(prog)) return;
    const ProgramObject& p = sg->programs[prog];
    int32_t v = 0;
    if (pname == 0x8B80 /* GL_DELETE_STATUS */) v = p.pendingDelete ? 1 : 0;
    else if (pname == GL_LINK_STATUS || pname == GL_VALIDATE_STATUS) v = p.linked ? 1 : 0;
    else if (pname == GL_INFO_LOG_LENGTH) v = p.infoLog.empty() ? 0 : int32_t(p.infoLog.size() + 1);
    else if (pname == GL_ATTACHED_SHADERS) v = int32_t(p.shaders.size());
    else if (pname == GL_ACTIVE_ATTRIBUTES) v = int32_t(p.attribs.size());
    else if (pname == GL_ACTIVE_ATTRIBUTE_MAX_LENGTH) v = 64;
    else if (pname == GL_ACTIVE_UNIFORMS) v = int32_t(p.uniforms.size());
    else if (pname == GL_ACTIVE_UNIFORM_MAX_LENGTH) v = 64;
    x.mem.write<int32_t>(params, v);
}
void gl_getProgramInfoLog(Ctx& x, uint32_t prog, int32_t maxLen, GuestPtr length, GuestPtr infoLog) {
    auto* sg = currentSg(x, currentCtx(x));
    if (sg && sg->programs.count(prog)) writeGlInfoLog(x, sg->programs.at(prog).infoLog, maxLen, length, infoLog);
}

int32_t gl_getAttribLocation(Ctx& x, uint32_t prog, GuestPtr name) {
    auto* sg = currentSg(x, currentCtx(x));
    if (!sg || !sg->programs.count(prog) || !name) return -1;
    ProgramObject& p = sg->programs[prog];
    const std::string nm = x.proc.readCString(name);
    for (const auto& a : p.attribs) if (a.name == nm) return a.location;
    return -1;
}

int32_t gl_getUniformLocation(Ctx& x, uint32_t prog, GuestPtr name) {
    auto* sg = currentSg(x, currentCtx(x));
    if (!sg || !sg->programs.count(prog) || !name) return -1;
    ProgramObject& p = sg->programs[prog];
    std::string nm = x.proc.readCString(name);
    for (const auto& u : p.uniforms) if (u.name == nm) return u.location;
    if (nm.size() >= 3 && nm.back() == ']') {
        const auto br = nm.rfind('[');
        if (br != std::string::npos) {
            const std::string base = nm.substr(0, br);
            const std::string digits = nm.substr(br + 1, nm.size() - br - 2);
            int index = 0;
            bool numeric = !digits.empty();
            for (char ch : digits) {
                if (ch < '0' || ch > '9') { numeric = false; break; }
                index = index * 10 + (ch - '0');
            }
            if (numeric) for (const auto& u : p.uniforms)
                if (u.array && u.name == base && index < u.size) return u.location + index;
        }
    }
    return -1;
}

void writeActiveVar(Ctx& x, const ShaderVar& v, int32_t bufSize, GuestPtr length, GuestPtr size, GuestPtr type, GuestPtr name) {
    if (size) x.mem.write<int32_t>(size, v.size);
    if (type) x.mem.write<uint32_t>(type, v.type);
    if (name && bufSize > 0) {
        const size_t n = std::min(v.name.size(), size_t(bufSize - 1));
        x.mem.copyIn(name, v.name.data(), n);
        x.mem.write<uint8_t>(name + uint32_t(n), 0);
        if (length) x.mem.write<int32_t>(length, int32_t(n));
    }
}
void gl_getActiveAttrib(Ctx& x, uint32_t prog, uint32_t index, int32_t bufSize, GuestPtr length, GuestPtr size, GuestPtr type, GuestPtr name) {
    auto* sg = currentSg(x, currentCtx(x));
    if (!sg || !sg->programs.count(prog) || index >= sg->programs[prog].attribs.size()) return;
    writeActiveVar(x, sg->programs[prog].attribs[index], bufSize, length, size, type, name);
}
void gl_getActiveUniform(Ctx& x, uint32_t prog, uint32_t index, int32_t bufSize, GuestPtr length, GuestPtr size, GuestPtr type, GuestPtr name) {
    auto* sg = currentSg(x, currentCtx(x));
    if (!sg || !sg->programs.count(prog) || index >= sg->programs[prog].uniforms.size()) return;
    ShaderVar v = sg->programs[prog].uniforms[index];
    if (v.array) v.name += "[0]";
    writeActiveVar(x, v, bufSize, length, size, type, name);
}

void gl_enableVertexAttribArray(Ctx& x, uint32_t idx) { if (auto* c = currentCtx(x); c && idx < 16) currentVao(c).attribPtr[idx].enabled = true; }
void gl_disableVertexAttribArray(Ctx& x, uint32_t idx) { if (auto* c = currentCtx(x); c && idx < 16) currentVao(c).attribPtr[idx].enabled = false; }
void gl_vertexAttribPointer(Ctx& x, uint32_t idx, int32_t size, uint32_t type, uint32_t normalized, int32_t stride, GuestPtr ptr) {
    if (auto* c = currentCtx(x); c && idx < 16) {
        currentVao(c).attribPtr[idx] = {currentVao(c).attribPtr[idx].enabled, size, type, (normalized & 0xFF) != 0, stride, ptr, c->arrayBuffer};
    }
}
void gl_vertexAttrib1f(Ctx& x, uint32_t idx, float v0) { if (auto* c = currentCtx(x); c && idx < 16) c->genericAttrib[idx] = {v0, 0, 0, 1}; }
void gl_vertexAttrib2f(Ctx& x, uint32_t idx, float v0, float v1) { if (auto* c = currentCtx(x); c && idx < 16) c->genericAttrib[idx] = {v0, v1, 0, 1}; }
void gl_vertexAttrib3f(Ctx& x, uint32_t idx, float v0, float v1, float v2) { if (auto* c = currentCtx(x); c && idx < 16) c->genericAttrib[idx] = {v0, v1, v2, 1}; }
void gl_vertexAttrib4f(Ctx& x, uint32_t idx, float v0, float v1, float v2, float v3) { if (auto* c = currentCtx(x); c && idx < 16) c->genericAttrib[idx] = {v0, v1, v2, v3}; }
void gl_vertexAttrib4fv(Ctx& x, uint32_t idx, GuestPtr v) {
    if (auto* c = currentCtx(x); c && idx < 16 && v)
        c->genericAttrib[idx] = {x.mem.read<float>(v), x.mem.read<float>(v + 4), x.mem.read<float>(v + 8), x.mem.read<float>(v + 12)};
}

// glGetVertexAttrib*: state of one attribute array (or its current generic value).
bool vertexAttribState(Ctx& x, uint32_t idx, uint32_t pname, float out[4], int& n) {
    ContextData* c = currentCtx(x);
    if (!c) return false;
    if (idx >= 16) { c->lastError = GL_INVALID_VALUE; return false; }
    const ArrayPointer& a = currentVao(c).attribPtr[idx];
    n = 1;
    switch (pname) {
    case 0x8622: out[0] = a.enabled; break;          // GL_VERTEX_ATTRIB_ARRAY_ENABLED
    case 0x8623: out[0] = float(a.size); break;      // ..._SIZE
    case 0x8624: out[0] = float(a.stride); break;    // ..._STRIDE
    case 0x8625: out[0] = float(a.type); break;      // ..._TYPE
    case 0x886A: out[0] = a.normalized; break;       // ..._NORMALIZED
    case 0x889F: out[0] = float(a.buffer); break;    // ..._BUFFER_BINDING
    case 0x8626: n = 4; for (int i = 0; i < 4; ++i) out[i] = c->genericAttrib[idx][i]; break;   // GL_CURRENT_VERTEX_ATTRIB
    default: c->lastError = GL_INVALID_ENUM; return false;
    }
    return true;
}
// EXT_occlusion_query_boolean
void gl_genQueriesEXT(Ctx& x, uint32_t n, GuestPtr ids) {
    if (!currentCtx(x)) return;
    for (uint32_t i = 0; i < n; ++i) x.mem.write<uint32_t>(ids + i * 4, i + 1);
}
void gl_deleteQueriesEXT(Ctx& x, uint32_t n, GuestPtr ids) {}
uint32_t gl_isQueryEXT(Ctx& x, uint32_t id) { return id > 0 ? 1 : 0; }
void gl_beginQueryEXT(Ctx& x, uint32_t target, uint32_t id) {}
void gl_endQueryEXT(Ctx& x, uint32_t target) {}
void gl_getQueryivEXT(Ctx& x, uint32_t target, uint32_t pname, GuestPtr params) {
    if (params) x.mem.write<int32_t>(params, 0);
}
void gl_getQueryObjectuivEXT(Ctx& x, uint32_t id, uint32_t pname, GuestPtr params) {
    if (params) x.mem.write<uint32_t>(params, 1);
}

void gl_drawBuffers(Ctx& x, int32_t n, GuestPtr bufs) {}
void gl_readBuffer(Ctx& x, uint32_t mode) {}

void gl_getVertexAttribfv(Ctx& x, uint32_t idx, uint32_t pname, GuestPtr out) {
    float v[4]; int n = 0;
    if (out && vertexAttribState(x, idx, pname, v, n)) for (int i = 0; i < n; ++i) x.mem.write<float>(out + 4 * i, v[i]);
}
void gl_getVertexAttribiv(Ctx& x, uint32_t idx, uint32_t pname, GuestPtr out) {
    float v[4]; int n = 0;
    if (out && vertexAttribState(x, idx, pname, v, n)) for (int i = 0; i < n; ++i) x.mem.write<int32_t>(out + 4 * i, int32_t(v[i]));
}
void gl_getVertexAttribPointerv(Ctx& x, uint32_t idx, uint32_t pname, GuestPtr out) {
    ContextData* c = currentCtx(x);
    if (!c || !out) return;
    if (idx >= 16 || pname != 0x8645) { c->lastError = idx >= 16 ? GL_INVALID_VALUE : GL_INVALID_ENUM; return; }   // GL_VERTEX_ATTRIB_ARRAY_POINTER
    x.mem.write<uint32_t>(out, currentVao(c).attribPtr[idx].ptr);
}

UniformValue* progUniform(Ctx& x, int32_t loc) {
    ContextData* c = currentCtx(x);
    SharegroupData* sg = currentSg(x, c);
    if (!c || !sg || !c->currentProgram || loc < 0 || !sg->programs.count(c->currentProgram)) return nullptr;
    ProgramObject& p = sg->programs[c->currentProgram];
    if (!p.linked || !p.uniformValues.count(loc)) { c->lastError = GL_INVALID_OPERATION; return nullptr; }
    UniformValue& u = p.uniformValues[loc];
    return &u;
}
bool uniformSpan(Ctx& x, int32_t loc, int32_t count) {
    auto* u = progUniform(x, loc);
    if (!u) return false;
    if (count > u->span) {
        if (auto* c = currentCtx(x)) c->lastError = GL_INVALID_OPERATION;
        return false;
    }
    return true;
}

void gl_uniform1i(Ctx& x, int32_t loc, int32_t v0) { if (auto* u = progUniform(x, loc)) u->i[0] = v0; }
void gl_uniform2i(Ctx& x, int32_t loc, int32_t v0, int32_t v1) { if (auto* u = progUniform(x, loc)) { u->i[0] = v0; u->i[1] = v1; } }
void gl_uniform3i(Ctx& x, int32_t loc, int32_t v0, int32_t v1, int32_t v2) { if (auto* u = progUniform(x, loc)) { u->i[0] = v0; u->i[1] = v1; u->i[2] = v2; } }
void gl_uniform4i(Ctx& x, int32_t loc, int32_t v0, int32_t v1, int32_t v2, int32_t v3) {
    if (auto* u = progUniform(x, loc)) { u->i[0] = v0; u->i[1] = v1; u->i[2] = v2; u->i[3] = v3; }
}
void gl_uniform1iv(Ctx& x, int32_t loc, int32_t count, GuestPtr v) {
    if (count <= 0 || !v) return;
    if (count == 1) { gl_uniform1i(x, loc, x.mem.read<int32_t>(v)); return; }
    if (!uniformSpan(x, loc, count)) return;
    for (int32_t i = 0; i < count; ++i) gl_uniform1i(x, loc + i, x.mem.read<int32_t>(v + uint32_t(i * 4)));
}
void gl_uniform1f(Ctx& x, int32_t loc, float v0) { if (auto* u = progUniform(x, loc)) u->f[0] = v0; }
void gl_uniform2f(Ctx& x, int32_t loc, float v0, float v1) { if (auto* u = progUniform(x, loc)) { u->f[0] = v0; u->f[1] = v1; } }
void gl_uniform3f(Ctx& x, int32_t loc, float v0, float v1, float v2) {
    if (auto* u = progUniform(x, loc)) {
        u->f[0] = v0; u->f[1] = v1; u->f[2] = v2;
    }
}
void gl_uniform4f(Ctx& x, int32_t loc, float v0, float v1, float v2, float v3) {
    if (auto* u = progUniform(x, loc)) {
        u->f[0] = v0; u->f[1] = v1; u->f[2] = v2; u->f[3] = v3;
    }
}
void gl_uniform1fv(Ctx& x, int32_t loc, int32_t c, GuestPtr v) {
    if (c <= 0 || !v) return;
    if (c == 1) { gl_uniform1f(x, loc, x.mem.read<float>(v)); return; }
    if (!uniformSpan(x, loc, c)) return;
    for (int32_t i = 0; i < c; ++i) gl_uniform1f(x, loc + i, x.mem.read<float>(v + uint32_t(i * 4)));
}
void gl_uniform2fv(Ctx& x, int32_t loc, int32_t c, GuestPtr v) {
    if (c <= 0 || !v) return;
    if (c == 1) { gl_uniform2f(x, loc, x.mem.read<float>(v), x.mem.read<float>(v + 4)); return; }
    if (!uniformSpan(x, loc, c)) return;
    for (int32_t i = 0; i < c; ++i) gl_uniform2f(x, loc + i, x.mem.read<float>(v + uint32_t(i * 8)), x.mem.read<float>(v + uint32_t(i * 8 + 4)));
}
void gl_uniform3fv(Ctx& x, int32_t loc, int32_t c, GuestPtr v) {
    if (c <= 0 || !v) return;
    if (c == 1) { gl_uniform3f(x, loc, x.mem.read<float>(v), x.mem.read<float>(v + 4), x.mem.read<float>(v + 8)); return; }
    if (!uniformSpan(x, loc, c)) return;
    for (int32_t i = 0; i < c; ++i)
        gl_uniform3f(x, loc + i, x.mem.read<float>(v + uint32_t(i * 12)), x.mem.read<float>(v + uint32_t(i * 12 + 4)), x.mem.read<float>(v + uint32_t(i * 12 + 8)));
}
void gl_uniform4fv(Ctx& x, int32_t loc, int32_t c, GuestPtr v) {
    if (c <= 0 || !v) return;
    if (c == 1) { gl_uniform4f(x, loc, x.mem.read<float>(v), x.mem.read<float>(v + 4), x.mem.read<float>(v + 8), x.mem.read<float>(v + 12)); return; }
    if (!uniformSpan(x, loc, c)) return;
    for (int32_t i = 0; i < c; ++i)
        gl_uniform4f(x, loc + i, x.mem.read<float>(v + uint32_t(i * 16)), x.mem.read<float>(v + uint32_t(i * 16 + 4)),
                     x.mem.read<float>(v + uint32_t(i * 16 + 8)), x.mem.read<float>(v + uint32_t(i * 16 + 12)));
}
void gl_uniformMatrix4fv(Ctx& x, int32_t loc, int32_t count, uint32_t transpose, GuestPtr value) {
    if (count <= 0 || !value) return;
    if (count != 1) {
        if (!uniformSpan(x, loc, count)) return;
        for (int32_t i = 0; i < count; ++i) gl_uniformMatrix4fv(x, loc + i, 1, transpose, value + uint32_t(i * 64));
        return;
    }
    if (auto* u = progUniform(x, loc)) {
        x.mem.copyOut(u->f, value, 64);
        if (transpose & 0xFF) {
            for (int r = 0; r < 4; ++r)
                for (int c = r + 1; c < 4; ++c)
                    std::swap(u->f[c * 4 + r], u->f[r * 4 + c]);
        }
    }
}
void gl_uniformMatrix3fv(Ctx& x, int32_t loc, int32_t count, uint32_t transpose, GuestPtr value) {
    if (count <= 0 || !value) return;
    if (count != 1) {
        if (!uniformSpan(x, loc, count)) return;
        for (int32_t i = 0; i < count; ++i) gl_uniformMatrix3fv(x, loc + i, 1, transpose, value + uint32_t(i * 36));
        return;
    }
    if (auto* u = progUniform(x, loc)) {
        x.mem.copyOut(u->f, value, 36);
        if (transpose & 0xFF) {
            for (int r = 0; r < 3; ++r)
                for (int c = r + 1; c < 3; ++c)
                    std::swap(u->f[c * 3 + r], u->f[r * 3 + c]);
        }
    }
}
void gl_uniformMatrix2fv(Ctx& x, int32_t loc, int32_t count, uint32_t transpose, GuestPtr value) {
    if (count <= 0 || !value) return;
    if (count != 1) {
        if (!uniformSpan(x, loc, count)) return;
        for (int32_t i = 0; i < count; ++i) gl_uniformMatrix2fv(x, loc + i, 1, transpose, value + uint32_t(i * 16));
        return;
    }
    if (auto* u = progUniform(x, loc)) {
        x.mem.copyOut(u->f, value, 16);
        if (transpose & 0xFF) {
            for (int r = 0; r < 2; ++r)
                for (int c = r + 1; c < 2; ++c)
                    std::swap(u->f[c * 2 + r], u->f[r * 2 + c]);
        }
    }
}

// =================================================== Software Rasterizer ===

struct ShadedVertex {
    Vec4 clip;
    float ndcX = 0, ndcY = 0, ndcZ = 0, invW = 1;
    float screenX = 0, screenY = 0, screenZ = 0;
    Vec4 color{1, 1, 1, 1};
    float u = 0, v = 0;
    std::array<float, 64> varyings{};
    float shaderPointSize = 0;
};

int typeBytes(uint32_t type) {
    switch (type) {
        case GL_BYTE: case GL_UNSIGNED_BYTE: return 1;
        case GL_SHORT: case GL_UNSIGNED_SHORT: return 2;
        default: return 4;
    }
}

Vec4 fetchAttribute(Ctx& x, SharegroupData& sg, const ArrayPointer& ap, int vertexIndex, const Vec4& fallback) {
    if (!ap.enabled) return fallback;
    const int elemBytes = typeBytes(ap.type);
    const int stride = ap.stride > 0 ? ap.stride : ap.size * elemBytes;
    const size_t byteOffset = size_t(ap.ptr) + size_t(vertexIndex) * size_t(stride);
    const uint8_t* src = nullptr;
    if (ap.buffer != 0) {
        auto bit = sg.buffers.find(ap.buffer);
        if (bit == sg.buffers.end() || byteOffset + size_t(ap.size * elemBytes) > bit->second.data.size())
            return fallback;
        src = bit->second.data.data() + byteOffset;
    } else {
        if (!ap.ptr) return fallback;
        src = x.mem.host(uint32_t(byteOffset));
    }

    Vec4 out{0, 0, 0, 1};
    for (int i = 0; i < std::min(ap.size, 4); ++i) {
        const uint8_t* p = src + i * elemBytes;
        float val = 0;
        switch (ap.type) {
            case GL_FLOAT: std::memcpy(&val, p, 4); break;
            case GL_FIXED: { int32_t fx; std::memcpy(&fx, p, 4); val = fx / 65536.0f; break; }
            case GL_BYTE: { int8_t b = int8_t(*p); val = ap.normalized ? std::max(-1.0f, b / 127.0f) : float(b); break; }
            case GL_UNSIGNED_BYTE: { uint8_t b = *p; val = ap.normalized ? b / 255.0f : float(b); break; }
            case GL_SHORT: { int16_t s; std::memcpy(&s, p, 2); val = ap.normalized ? std::max(-1.0f, s / 32767.0f) : float(s); break; }
            case GL_UNSIGNED_SHORT: { uint16_t s; std::memcpy(&s, p, 2); val = ap.normalized ? s / 65535.0f : float(s); break; }
            default: break;
        }
        out[i] = val;
    }
    return out;
}

void loadShaderUniforms(const ProgramObject& p, const Shader::Program& stage, const std::vector<ProgramObject::Binding>& bindings, Shader::Values& values) {
    for (auto& b : bindings) {
        auto it = p.uniformValues.find(b.location);
        if (it == p.uniformValues.end()) continue;
        float* dst = nullptr;
        if (b.memory) {
            if (b.slot < 0 || size_t(b.slot) + size_t(std::max(b.width, 0)) > stage.uniformMem.size()) continue;
            dst = &stage.uniformMem[size_t(b.slot)];
        } else {
            if (b.slot < 0 || b.slot + b.width > Shader::MaxSlots) continue;
            dst = &values[size_t(b.slot)];
        }
        const uint32_t ty = it->second.type;
        const bool integer = ty == GL_INT || ty == GL_INT_VEC2 || ty == GL_INT_VEC3 || ty == GL_INT_VEC4 ||
                             ty == GL_BOOL || ty == GL_BOOL_VEC2 || ty == GL_BOOL_VEC3 || ty == GL_BOOL_VEC4;
        if (b.sampler || integer) {
            for (int n = 0; n < b.width && n < 4; ++n) dst[n] = float(it->second.i[n]);
        } else std::copy_n(it->second.f, b.width, dst);
    }
}
void sampleShaderTexture(void* opaque, int unit, float u, float v, float* rgba) {
    auto& state = *static_cast<std::pair<ContextData*, SharegroupData*>*>(opaque);
    Vec4 color{0, 0, 0, 1};
    if (unit >= 0 && unit < 8 && std::isfinite(u) && std::isfinite(v)) {
        auto it = state.second->textures.find(state.first->boundTextures[unit]);
        if (it != state.second->textures.end()) color = it->second.sample(u, v);
    }
    for (int i = 0; i < 4; ++i) rgba[i] = color[i];
}
// Shared by every shader invocation in one draw. Exhausting it aborts the draw,
// drops the fragment that crossed the cap, and records GL_INVALID_OPERATION.
// HVM_SHADER_BUDGET_REPORT=1 on fulltest: the heaviest GLES2 draw was 63656
// instructions across 2051 invocations (~31 each) on a 64x64 target. The same
// per-fragment cost over 320x480 is about 4.8e6 instructions. 2e8 is ~42x that
// full-screen figure (about 1300 instructions on every fragment) and still
// aborts a non-terminating fragment in a fraction of a second. The old cap was
// 1e6 per fragment, about 431 s for one 320x480 triangle.
constexpr int64_t kDrawInstructionBudget = 200'000'000;

struct DrawBudget {
    Ctx* ctx = nullptr;
    ContextData* context = nullptr;
    ProgramObject* program = nullptr;
    int64_t remaining = kDrawInstructionBudget;
    int64_t invocations = 0;
    bool exceeded = false;
    void fail() {
        if (exceeded) return;
        exceeded = true;
        if (context && context->lastError == GL_NO_ERROR) context->lastError = GL_INVALID_OPERATION;
        if (program && ctx && !program->budgetLogged) {
            program->budgetLogged = true;
            ctx->proc.logf(LogLevel::Warning,
                           "GLSL program %u: instruction budget exceeded; draw aborted", program->id);
        }
    }
};

struct BudgetReport {
    const DrawBudget* budget = nullptr;
    ~BudgetReport() {
        static const bool report = std::getenv("HVM_SHADER_BUDGET_REPORT") != nullptr;
        if (!report || !budget) return;
        static int64_t peak = 0;
        const int64_t used = budget->exceeded ? kDrawInstructionBudget : kDrawInstructionBudget - budget->remaining;
        if (!budget->exceeded && used > peak) peak = used;
        std::fprintf(stderr, "shader-draw-insns used %lld peak %lld invocations %lld exceeded %d\n",
                     static_cast<long long>(used), static_cast<long long>(peak),
                     static_cast<long long>(budget->invocations), budget->exceeded ? 1 : 0);
    }
};

void fillDepthRange(const ContextData& c, const Shader::Program& prog, Shader::Values& values) {
    if (auto* dr = prog.find("gl_DepthRange")) {
        values[dr->slot] = c.depthRange[0];
        values[dr->slot + 1] = c.depthRange[1];
        values[dr->slot + 2] = c.depthRange[1] - c.depthRange[0];
    }
}
bool setupFragment(ContextData& c, const ProgramObject& p, Shader::Values& values,
                   const ShadedVertex& a, const ShadedVertex& b, const ShadedVertex& d,
                   float w0, float w1, float w2, int px, int py, float z, bool front, float pointS, float pointT) {
    p.fragment.initialize(values);
    loadShaderUniforms(p, p.fragment, p.fragmentUniforms, values);
    float denominator = w0 * a.invW + w1 * b.invW + w2 * d.invW;
    if (std::abs(denominator) < 1e-12f) return false;
    w0 = w0 * a.invW / denominator; w1 = w1 * b.invW / denominator; w2 = w2 * d.invW / denominator;
    for (auto& varying : p.varyings) for (int i = 0; i < varying.width; ++i) {
        int offset = varying.offset + i;
        values[varying.fragmentSlot + i] = w0 * a.varyings[offset] + w1 * b.varyings[offset] + w2 * d.varyings[offset];
    }
    if (auto* fc = p.fragment.find("gl_FragCoord")) {
        values[fc->slot] = float(px) + 0.5f;
        values[fc->slot + 1] = float(py) + 0.5f;
        values[fc->slot + 2] = z;
        values[fc->slot + 3] = denominator;
    }
    if (auto* ff = p.fragment.find("gl_FrontFacing")) values[ff->slot] = front ? 1.f : 0.f;
    if (auto* pc = p.fragment.find("gl_PointCoord")) { values[pc->slot] = pointS; values[pc->slot + 1] = pointT; }
    fillDepthRange(c, p.fragment, values);
    return true;
}
std::optional<Vec4> executeFragment(ContextData& c, SharegroupData& sg, const ShadedVertex& a, const ShadedVertex& b, const ShadedVertex& d,
                     float w0, float w1, float w2, int px, int py, float z, bool front, float pointS, float pointT, DrawBudget* budget) {
    const auto& p = sg.programs.at(c.currentProgram);
    Shader::Values values;
    if (!setupFragment(c, p, values, a, b, d, w0, w1, w2, px, py, z, front, pointS, pointT)) return Vec4{0, 0, 0, 0};
    if (budget) ++budget->invocations;
    std::pair<ContextData*, SharegroupData*> state{&c, &sg};
    int64_t* left = budget ? &budget->remaining : nullptr;
    if (!p.fragment.execute(values, sampleShaderTexture, &state, left)) {
        if (budget && budget->remaining < 0) budget->fail();
        return std::nullopt;
    }
    const auto* f = &values[p.fragment.output];
    return Vec4{f[0], f[1], f[2], f[3]};
}

ShadedVertex shadeVertex(Ctx& x, ContextData& c, SharegroupData& sg, int idx, DrawBudget* budget) {
    ShadedVertex sv;
    if (c.api == 1 || c.currentProgram == 0) {
        const Vec4 pos = fetchAttribute(x, sg, currentVao(&c).vertexPtr, idx, {0, 0, 0, 1});
        sv.color = fetchAttribute(x, sg, currentVao(&c).colorPtr, idx, c.currentColor);
        const Vec4 tc = fetchAttribute(x, sg, currentVao(&c).texCoordPtr[0], idx, c.currentTexCoord[0]);
        const Vec4 tcXformed = c.textureStack.back().apply(tc);
        sv.u = tcXformed.x;
        sv.v = tcXformed.y;
        if (currentVao(&c).pointSizePtr.enabled) {
            const Vec4 ps = fetchAttribute(x, sg, currentVao(&c).pointSizePtr, idx, {c.pointSize, 0, 0, 0});
            sv.shaderPointSize = ps.x;
        }
        const Mat4 mvp = c.projectionStack.back() * c.modelviewStack.back();
        sv.clip = mvp.apply(pos);
    } else {
        const ProgramObject& p = sg.programs.at(c.currentProgram);
        Shader::Values values;
        p.vertex.initialize(values);
        loadShaderUniforms(p, p.vertex, p.vertexUniforms, values);
        for (auto& binding : p.vertexInputs) {
            const auto attr = fetchAttribute(x, sg, currentVao(&c).attribPtr[binding.location], idx, c.genericAttrib[binding.location]);
            for (int i = 0; i < binding.width; ++i) values[binding.slot + i] = attr[i];
        }
        fillDepthRange(c, p.vertex, values);
        std::pair<ContextData*, SharegroupData*> state{&c, &sg};
        int64_t* left = budget ? &budget->remaining : nullptr;
        if (budget) ++budget->invocations;
        if (!p.vertex.execute(values, sampleShaderTexture, &state, left) && budget && budget->remaining < 0) budget->fail();
        if (auto* ps = p.vertex.find("gl_PointSize")) sv.shaderPointSize = values[ps->slot];
        const float* f = &values[p.vertex.output];
        sv.clip = {f[0], f[1], f[2], f[3]};
        for (auto& varying : p.varyings)
            std::copy_n(&values[varying.vertexSlot], varying.width, &sv.varyings[varying.offset]);
    }
    // Divide by the signed w. Vertices with w<=0 or z<-w are not safe to project;
    // drawPrimitives clips those in homogeneous space and reprojects the result.
    const float w = (std::abs(sv.clip.w) > 1e-6f) ? sv.clip.w : 1.0f;
    sv.invW = 1.0f / w;
    sv.ndcX = sv.clip.x * sv.invW;
    sv.ndcY = sv.clip.y * sv.invW;
    sv.ndcZ = sv.clip.z * sv.invW;
    sv.screenX = float(c.viewport[0]) + (sv.ndcX * 0.5f + 0.5f) * float(c.viewport[2]);
    sv.screenY = float(c.viewport[1]) + (sv.ndcY * 0.5f + 0.5f) * float(c.viewport[3]);
    sv.screenZ = c.depthRange[0] + (sv.ndcZ * 0.5f + 0.5f) * (c.depthRange[1] - c.depthRange[0]);
    return sv;
}

bool depthPass(uint32_t func, float z, float ref) {
    switch (func) {
        case GL_NEVER: return false;
        case GL_LESS: return z < ref;
        case GL_EQUAL: return std::abs(z - ref) < 1e-5f;
        case GL_LEQUAL: return z <= ref;
        case GL_GREATER: return z > ref;
        case GL_NOTEQUAL: return std::abs(z - ref) >= 1e-5f;
        case GL_GEQUAL: return z >= ref;
        default: return true;
    }
}

float blendFactorVal(const ContextData& c, uint32_t factor, float srcC, float srcA, float dstC, float dstA, int ch) {
    switch (factor) {
        case GL_ZERO: return 0.0f;
        case GL_ONE: return 1.0f;
        case GL_SRC_COLOR: return srcC;
        case GL_ONE_MINUS_SRC_COLOR: return 1.0f - srcC;
        case GL_DST_COLOR: return dstC;
        case GL_ONE_MINUS_DST_COLOR: return 1.0f - dstC;
        case GL_SRC_ALPHA: return srcA;
        case GL_ONE_MINUS_SRC_ALPHA: return 1.0f - srcA;
        case GL_DST_ALPHA: return dstA;
        case GL_ONE_MINUS_DST_ALPHA: return 1.0f - dstA;
        case GL_SRC_ALPHA_SATURATE: return std::min(srcA, 1.0f - dstA);
        case 0x8001 /* GL_CONSTANT_COLOR */: return c.blendColor[ch];
        case 0x8002 /* GL_ONE_MINUS_CONSTANT_COLOR */: return 1.0f - c.blendColor[ch];
        case 0x8003 /* GL_CONSTANT_ALPHA */: return c.blendColor[3];
        case 0x8004 /* GL_ONE_MINUS_CONSTANT_ALPHA */: return 1.0f - c.blendColor[3];
        default: return 1.0f;
    }
}

inline float applyBlendEquation(uint32_t eq, float s, float sf, float d, float df) {
    switch (eq) {
        case 0x800A /* GL_FUNC_SUBTRACT */: return s * sf - d * df;
        case 0x800B /* GL_FUNC_REVERSE_SUBTRACT */: return d * df - s * sf;
        case 0x8007 /* GL_MIN_EXT */: return std::min(s, d);
        case 0x8008 /* GL_MAX_EXT */: return std::max(s, d);
        case 0x8006 /* GL_FUNC_ADD */:
        default: return s * sf + d * df;
    }
}

bool stencilPass(uint32_t func, uint8_t ref, uint8_t stored) {
    switch (func) {
        case GL_NEVER: return false;
        case GL_LESS: return ref < stored;
        case GL_EQUAL: return ref == stored;
        case GL_LEQUAL: return ref <= stored;
        case GL_GREATER: return ref > stored;
        case GL_NOTEQUAL: return ref != stored;
        case GL_GEQUAL: return ref >= stored;
        case GL_ALWAYS: default: return true;
    }
}

uint8_t stencilResult(uint32_t op, uint8_t stored, uint8_t ref) {
    switch (op) {
        case GL_ZERO: return 0;
        case GL_REPLACE: return ref;
        case GL_INCR: return stored == 255 ? 255 : uint8_t(stored + 1);
        case GL_DECR: return stored == 0 ? 0 : uint8_t(stored - 1);
        case GL_INCR_WRAP: return uint8_t(stored + 1);
        case GL_DECR_WRAP: return uint8_t(stored - 1);
        case GL_INVERT: return uint8_t(~stored);
        case GL_KEEP: default: return stored;
    }
}

void writeFragment(ContextData& c, RenderTarget& rt, int px, int py, float z, Vec4 frag, bool frontFacing = true) {
    if ((!rt.rgba && !rt.depth && !rt.stencil) || px < 0 || px >= rt.width || py < 0 || py >= rt.height) return;
    if (c.alphaTest && !depthPass(c.alphaFunc, frag.w, c.alphaRef)) return;
    const size_t idx = size_t(py) * size_t(rt.width) + size_t(px);
    const auto& stencil = frontFacing ? c.stencilFront : c.stencilBack;
    auto applyStencil = [&](uint32_t op) {
        const uint8_t next = stencilResult(op, rt.stencil[idx], uint8_t(stencil.ref));
        rt.stencil[idx] = (next & uint8_t(stencil.writeMask)) | (rt.stencil[idx] & uint8_t(~stencil.writeMask));
    };
    if (c.stencilTest && rt.stencil) {
        const uint8_t ref = uint8_t(stencil.ref) & uint8_t(stencil.mask);
        const uint8_t stored = rt.stencil[idx] & uint8_t(stencil.mask);
        if (!stencilPass(stencil.func, ref, stored)) {
            applyStencil(stencil.fail);
            return;
        }
    }
    if (c.depthTest && rt.depth && !depthPass(c.depthFunc, z, rt.depth[idx])) {
        if (c.stencilTest && rt.stencil) applyStencil(stencil.zfail);
        return;
    }
    if (c.stencilTest && rt.stencil) applyStencil(stencil.zpass);
    if (c.depthTest && rt.depth && c.depthMask) rt.depth[idx] = z;
    if (!rt.rgba) return;
    uint8_t* dst = &rt.rgba[idx * 4];
    float outR = frag.x, outG = frag.y, outB = frag.z, outA = frag.w;
    if (c.blend) {
        const float dstR = dst[0] / 255.0f, dstG = dst[1] / 255.0f, dstB = dst[2] / 255.0f, dstA = dst[3] / 255.0f;
        const float srR = blendFactorVal(c, c.blendSrcRGB, outR, outA, dstR, dstA, 0);
        const float srG = blendFactorVal(c, c.blendSrcRGB, outG, outA, dstG, dstA, 1);
        const float srB = blendFactorVal(c, c.blendSrcRGB, outB, outA, dstB, dstA, 2);
        const float drR = blendFactorVal(c, c.blendDstRGB, outR, outA, dstR, dstA, 0);
        const float drG = blendFactorVal(c, c.blendDstRGB, outG, outA, dstG, dstA, 1);
        const float drB = blendFactorVal(c, c.blendDstRGB, outB, outA, dstB, dstA, 2);
        const float sa  = blendFactorVal(c, c.blendSrcAlpha, outA, outA, dstA, dstA, 3);
        const float da  = blendFactorVal(c, c.blendDstAlpha, outA, outA, dstA, dstA, 3);
        outR = applyBlendEquation(c.blendEqRGB, outR, srR, dstR, drR);
        outG = applyBlendEquation(c.blendEqRGB, outG, srG, dstG, drG);
        outB = applyBlendEquation(c.blendEqRGB, outB, srB, dstB, drB);
        outA = applyBlendEquation(c.blendEqAlpha, outA, sa, dstA, da);
    }
    if (c.colorMask[0]) dst[0] = uint8_t(std::clamp(outR, 0.0f, 1.0f) * 255.0f + 0.5f);
    if (c.colorMask[1]) dst[1] = uint8_t(std::clamp(outG, 0.0f, 1.0f) * 255.0f + 0.5f);
    if (c.colorMask[2]) dst[2] = uint8_t(std::clamp(outB, 0.0f, 1.0f) * 255.0f + 0.5f);
    if (c.colorMask[3]) dst[3] = uint8_t(std::clamp(outA, 0.0f, 1.0f) * 255.0f + 0.5f);
}

const Texture2D* boundTextureForDraw(ContextData& c, SharegroupData& sg) {
    if (c.api == 1 || c.currentProgram == 0) {
        const int act = c.activeTexture & 7;
        if (c.texture2DEnabled[act]) {
            const uint32_t tid = c.boundTextures[act];
            if (tid && sg.textures.count(tid)) return &sg.textures.at(tid);
        }
        for (int i = 0; i < 8; ++i) {
            if (c.texture2DEnabled[i]) {
                const uint32_t tid = c.boundTextures[i];
                if (tid && sg.textures.count(tid)) return &sg.textures.at(tid);
            }
        }
        return nullptr;
    }
    return nullptr; // GLES2 samples each declared sampler through the interpreter.
}

// Guard used by the perspective divide. Matches the historical epsilon in shadeVertex.
constexpr float kNearWEpsilon = 1e-6f;
// Near, w-guard, then the four side planes. A triangle gains at most one
// vertex per plane (3 + 6 = 9); 16 leaves room for that.
constexpr int kClipPlaneCount = 6;
constexpr int kMaxClipVerts = 16;

// plane 0 near z+w, 1 guard w-eps, 2 left x+w, 3 right -x+w, 4 bottom y+w, 5 top -y+w.
float clipPlaneDistance(const ShadedVertex& v, int plane) {
    switch (plane) {
        case 0: return v.clip.z + v.clip.w;
        case 1: return v.clip.w - kNearWEpsilon;
        case 2: return v.clip.x + v.clip.w;
        case 3: return -v.clip.x + v.clip.w;
        case 4: return v.clip.y + v.clip.w;
        default: return -v.clip.y + v.clip.w;
    }
}

// Fast path: every plane the clipper would keep, with a strict w test.
// shadeVertex replaces |w|<=epsilon with 1, so a vertex on the guard must go
// through the clipper and be reprojected with its real w.
bool insideClipVolume(const ShadedVertex& v) {
    if (!std::isfinite(v.clip.x) || !std::isfinite(v.clip.y) || !std::isfinite(v.clip.z) ||
        !std::isfinite(v.clip.w) || !(v.clip.w > kNearWEpsilon))
        return false;
    for (int plane = 0; plane < kClipPlaneCount; ++plane) {
        if (plane == 1) continue;
        if (clipPlaneDistance(v, plane) < 0.0f) return false;
    }
    return true;
}

// The guard plane's float32 lerp undershoots 1e-6 (about 9.54e-7) even though
// the clipper accepted the edge (distance >= 0). Acceptance has to keep that
// output. Requiring w > 1e-6 drops the whole primitive.
bool clippedWIsDrawable(const ShadedVertex& v) {
    return std::isfinite(v.clip.x) && std::isfinite(v.clip.y) && std::isfinite(v.clip.z) &&
           std::isfinite(v.clip.w) && v.clip.w > 0.0f;
}

// Linear in clip space. Varyings, color and UVs must be lerped before the divide;
// the rasterizer then applies perspective correction with the new 1/w.
ShadedVertex interpolateInClipSpace(const ShadedVertex& a, const ShadedVertex& b, float t) {
    ShadedVertex o;
    auto lerp = [t](float p, float q) { return p + (q - p) * t; };
    o.clip.x = lerp(a.clip.x, b.clip.x);
    o.clip.y = lerp(a.clip.y, b.clip.y);
    o.clip.z = lerp(a.clip.z, b.clip.z);
    o.clip.w = lerp(a.clip.w, b.clip.w);
    o.color.x = lerp(a.color.x, b.color.x);
    o.color.y = lerp(a.color.y, b.color.y);
    o.color.z = lerp(a.color.z, b.color.z);
    o.color.w = lerp(a.color.w, b.color.w);
    o.u = lerp(a.u, b.u);
    o.v = lerp(a.v, b.v);
    for (size_t i = 0; i < o.varyings.size(); ++i) o.varyings[i] = lerp(a.varyings[i], b.varyings[i]);
    return o;
}

void projectShadedVertex(ShadedVertex& sv, const ContextData& c) {
    const float invW = 1.0f / sv.clip.w;
    sv.invW = invW;
    sv.ndcX = sv.clip.x * invW;
    sv.ndcY = sv.clip.y * invW;
    sv.ndcZ = sv.clip.z * invW;
    sv.screenX = float(c.viewport[0]) + (sv.ndcX * 0.5f + 0.5f) * float(c.viewport[2]);
    sv.screenY = float(c.viewport[1]) + (sv.ndcY * 0.5f + 0.5f) * float(c.viewport[3]);
    sv.screenZ = c.depthRange[0] + (sv.ndcZ * 0.5f + 0.5f) * (c.depthRange[1] - c.depthRange[0]);
}

// Sutherland-Hodgman. Output keeps the input winding.
void clipPolygonAgainst(const ShadedVertex* in, int nIn, ShadedVertex* out, int& nOut, int plane) {
    nOut = 0;
    if (nIn <= 0) return;
    for (int i = 0; i < nIn; ++i) {
        const ShadedVertex& cur = in[i];
        const ShadedVertex& prev = in[(i + nIn - 1) % nIn];
        const float dCur = clipPlaneDistance(cur, plane);
        const float dPrev = clipPlaneDistance(prev, plane);
        const bool curIn = dCur >= 0.0f;
        const bool prevIn = dPrev >= 0.0f;
        if (curIn != prevIn) {
            const float denom = dPrev - dCur;
            if (std::abs(denom) > 1e-20f && nOut < kMaxClipVerts)
                out[nOut++] = interpolateInClipSpace(prev, cur, dPrev / denom);
        }
        if (curIn && nOut < kMaxClipVerts) out[nOut++] = cur;
    }
}

bool clipSegmentVolume(ShadedVertex& a, ShadedVertex& b) {
    for (int plane = 0; plane < kClipPlaneCount; ++plane) {
        const float da = clipPlaneDistance(a, plane);
        const float db = clipPlaneDistance(b, plane);
        const bool aIn = da >= 0.0f;
        const bool bIn = db >= 0.0f;
        if (!aIn && !bIn) return false;
        if (aIn && bIn) continue;
        const float denom = da - db;
        if (std::abs(denom) < 1e-20f) return false;
        const ShadedVertex hit = interpolateInClipSpace(a, b, da / denom);
        if (!aIn) a = hit;
        else b = hit;
    }
    return clippedWIsDrawable(a) && clippedWIsDrawable(b);
}

void rasterizeTriangle(ContextData& c, SharegroupData& sg, RenderTarget& rt,
                       const ShadedVertex& v0, const ShadedVertex& v1, const ShadedVertex& v2,
                       DrawBudget* budget, bool clipped = false) {
    if (!clipped && (!insideClipVolume(v0) || !insideClipVolume(v1) || !insideClipVolume(v2))) {
        ShadedVertex polyA[kMaxClipVerts], polyB[kMaxClipVerts];
        polyA[0] = v0;
        polyA[1] = v1;
        polyA[2] = v2;
        ShadedVertex* in = polyA;
        ShadedVertex* out = polyB;
        int nIn = 3;
        for (int plane = 0; plane < kClipPlaneCount; ++plane) {
            int nOut = 0;
            clipPolygonAgainst(in, nIn, out, nOut, plane);
            ShadedVertex* tmp = in;
            in = out;
            out = tmp;
            nIn = nOut;
        }
        if (nIn < 3) return;
        for (int i = 0; i < nIn; ++i) {
            if (!clippedWIsDrawable(in[i])) return;
            projectShadedVertex(in[i], c);
        }
        for (int i = 1; i + 1 < nIn; ++i)
            rasterizeTriangle(c, sg, rt, in[0], in[i], in[i + 1], budget, true);
        return;
    }
    for (auto* v : {&v0, &v1, &v2})
        if (!std::isfinite(v->screenX) || !std::isfinite(v->screenY) || !std::isfinite(v->screenZ)) return;
    const float area = (v1.screenX - v0.screenX) * (v2.screenY - v0.screenY) -
                       (v1.screenY - v0.screenY) * (v2.screenX - v0.screenX);
    if (std::abs(area) < 1e-4f) return;
    const bool ccw = area > 0;
    const bool isFront = (c.frontFace == GL_CCW) ? ccw : !ccw;
    if (c.cullFace) {
        if (c.cullFaceMode == GL_FRONT_AND_BACK) return;
        if ((c.cullFaceMode == GL_FRONT && isFront) || (c.cullFaceMode == GL_BACK && !isFront)) return;
    }

    int clipX0 = 0, clipY0 = 0, clipX1 = rt.width, clipY1 = rt.height;
    if (c.scissorTest) {
        clipX0 = std::clamp(c.scissor[0], 0, rt.width);
        clipY0 = std::clamp(c.scissor[1], 0, rt.height);
        clipX1 = std::clamp(c.scissor[0] + c.scissor[2], 0, rt.width);
        clipY1 = std::clamp(c.scissor[1] + c.scissor[3], 0, rt.height);
    }

    // Clamp in floating point before conversion: valid shaders can produce
    // coordinates outside the range of a host int.
    const int minX = int(std::clamp(std::floor(std::min({v0.screenX, v1.screenX, v2.screenX})), float(clipX0), float(clipX1)));
    const int maxX = int(std::clamp(std::ceil(std::max({v0.screenX, v1.screenX, v2.screenX})), float(clipX0 - 1), float(clipX1 - 1)));
    const int minY = int(std::clamp(std::floor(std::min({v0.screenY, v1.screenY, v2.screenY})), float(clipY0), float(clipY1)));
    const int maxY = int(std::clamp(std::ceil(std::max({v0.screenY, v1.screenY, v2.screenY})), float(clipY0 - 1), float(clipY1 - 1)));
    if (minX > maxX || minY > maxY) return;

    const float invArea = 1.0f / area;
    const Texture2D* tex = boundTextureForDraw(c, sg);
    // Debugging aid: HVM_TRACE_TRI=<w>x<h> prints triangles drawn with a texture of that size.
    if (static const char* tr = std::getenv("HVM_TRACE_TRI"); tr && tex) {
        static int printed = 0;
        int tw = 0, th = 0;
        if (std::sscanf(tr, "%dx%d", &tw, &th) == 2 && tex->width == tw && tex->height == th && printed < 60) {
            ++printed;
            std::fprintf(stderr, "tri (%.3f,%.3f uv %.5f,%.5f) (%.3f,%.3f uv %.5f,%.5f) (%.3f,%.3f uv %.5f,%.5f) filt %x\n",
                         v0.screenX, v0.screenY, v0.u, v0.v, v1.screenX, v1.screenY, v1.u, v1.v, v2.screenX, v2.screenY, v2.u, v2.v, tex->magFilter);
        }
    }

    auto bary = [&](int px, int py, float& w0, float& w1, float& w2) {
        const float fx = float(px) + 0.5f;
        const float fy = float(py) + 0.5f;
        w0 = ((v2.screenX - v1.screenX) * (fy - v1.screenY) - (v2.screenY - v1.screenY) * (fx - v1.screenX)) * invArea;
        w1 = ((v0.screenX - v2.screenX) * (fy - v2.screenY) - (v0.screenY - v2.screenY) * (fx - v2.screenX)) * invArea;
        w2 = 1.0f - w0 - w1;
    };
    const bool shaded = c.api != 1 && c.currentProgram != 0;
    // dFdx/dFdy are finite differences on a 2x2 window-aligned quad:
    // lane 0 (x,y), lane 1 (x+1,y), lane 2 (x,y+1), lane 3 (x+1,y+1), x and y even.
    // Helper lanes outside the triangle still run so a covered neighbor has a
    // value; only covered in-bounds fragments are written. Lines and points have
    // no quad, so their derivatives stay zero. Across divergent control flow the
    // neighbor contributes whatever value it currently holds.
    if (shaded && budget && sg.programs.at(c.currentProgram).fragment.usesDerivatives) {
        const ProgramObject& prog = sg.programs.at(c.currentProgram);
        const int x0 = minX & ~1;
        const int y0 = minY & ~1;
        for (int qy = y0; qy <= maxY && !budget->exceeded; qy += 2) {
            for (int qx = x0; qx <= maxX && !budget->exceeded; qx += 2) {
                const int px[4] = {qx, qx + 1, qx, qx + 1};
                const int py[4] = {qy, qy, qy + 1, qy + 1};
                float w0[4], w1[4], w2[4], z[4];
                bool covered[4] = {};
                bool any = false;
                for (int lane = 0; lane < 4; ++lane) {
                    bary(px[lane], py[lane], w0[lane], w1[lane], w2[lane]);
                    const bool inClip = px[lane] >= clipX0 && px[lane] < clipX1 && py[lane] >= clipY0 && py[lane] < clipY1;
                    covered[lane] = inClip && w0[lane] >= -1e-4f && w1[lane] >= -1e-4f && w2[lane] >= -1e-4f;
                    z[lane] = w0[lane] * v0.screenZ + w1[lane] * v1.screenZ + w2[lane] * v2.screenZ;
                    any = any || covered[lane];
                }
                if (!any) continue;
                std::array<Shader::Values, 4> lanes{};
                int active = 0;
                for (int lane = 0; lane < 4; ++lane) {
                    if (!setupFragment(c, prog, lanes[lane], v0, v1, v2, w0[lane], w1[lane], w2[lane], px[lane], py[lane], z[lane], isFront, 0, 0)) {
                        if (covered[lane]) writeFragment(c, rt, px[lane], py[lane], z[lane], Vec4{0, 0, 0, 0}, isFront);
                        covered[lane] = false;
                        continue;
                    }
                    active |= 1 << lane;
                    ++budget->invocations;
                }
                if (!active) continue;
                int discarded = 0;
                std::pair<ContextData*, SharegroupData*> state{&c, &sg};
                if (!prog.fragment.executeQuad(lanes, sampleShaderTexture, &state, &budget->remaining, active, discarded)) {
                    if (budget->remaining < 0) budget->fail();
                    return;
                }
                for (int lane = 0; lane < 4; ++lane) {
                    if (!covered[lane] || (discarded & (1 << lane))) continue;
                    const float* f = &lanes[lane][prog.fragment.output];
                    writeFragment(c, rt, px[lane], py[lane], z[lane], Vec4{f[0], f[1], f[2], f[3]}, isFront);
                }
            }
        }
        return;
    }

    for (int py = minY; py <= maxY && !(budget && budget->exceeded); ++py) {
        for (int px = minX; px <= maxX && !(budget && budget->exceeded); ++px) {
            float w0, w1, w2;
            bary(px, py, w0, w1, w2);
            if (w0 < -1e-4f || w1 < -1e-4f || w2 < -1e-4f) continue;

            const float z = w0 * v0.screenZ + w1 * v1.screenZ + w2 * v2.screenZ;
            if (shaded) {
                if (auto frag = executeFragment(c, sg, v0, v1, v2, w0, w1, w2, px, py, z, isFront, 0, 0, budget))
                    writeFragment(c, rt, px, py, z, *frag, isFront);
                continue;
            }
            // GLES1 color and UV stay screen-linear after clipping. The clip
            // lerps them in clip space so the new edge vertex is correct; this
            // fill does not reweight by 1/w. GLES2 varyings are perspective-correct
            // in executeFragment. Constant-w (ortho) draws match either formula.
            Vec4 col{
                w0 * v0.color.x + w1 * v1.color.x + w2 * v2.color.x,
                w0 * v0.color.y + w1 * v1.color.y + w2 * v2.color.y,
                w0 * v0.color.z + w1 * v1.color.z + w2 * v2.color.z,
                w0 * v0.color.w + w1 * v1.color.w + w2 * v2.color.w
            };
            if (tex) {
                const float u = w0 * v0.u + w1 * v1.u + w2 * v2.u;
                const float v = w0 * v0.v + w1 * v1.v + w2 * v2.v;
                const Vec4 tc = tex->sample(u, v);
                const uint32_t env = (c.api == 1) ? c.texEnvMode[c.activeTexture & 7] : GL_MODULATE;
                if (env == GL_REPLACE) {
                    col = tc;
                } else if (env == 0x2101 /* GL_DECAL */) {
                    col = {col.x * (1.0f - tc.w) + tc.x * tc.w,
                           col.y * (1.0f - tc.w) + tc.y * tc.w,
                           col.z * (1.0f - tc.w) + tc.z * tc.w,
                           col.w};
                } else if (env == 0x0104 /* GL_ADD */) {
                    col = {std::min(1.0f, col.x + tc.x), std::min(1.0f, col.y + tc.y), std::min(1.0f, col.z + tc.z), col.w * tc.w};
                } else if (env == 0x0BE2 /* GL_BLEND */) {
                    col = {col.x * (1.0f - tc.x) + c.currentColor.x * tc.x,
                           col.y * (1.0f - tc.y) + c.currentColor.y * tc.y,
                           col.z * (1.0f - tc.z) + c.currentColor.z * tc.z,
                           col.w * tc.w};
                } else {
                    col = {col.x * tc.x, col.y * tc.y, col.z * tc.z, col.w * tc.w};
                }
            }
            writeFragment(c, rt, px, py, z, col, isFront);
        }
    }
}

void rasterizeLine(ContextData& c, SharegroupData& sg, RenderTarget& rt,
                   const ShadedVertex& v0, const ShadedVertex& v1, DrawBudget* budget, bool clipped = false) {
    if (!clipped && (!insideClipVolume(v0) || !insideClipVolume(v1))) {
        ShadedVertex a = v0, b = v1;
        if (!clipSegmentVolume(a, b)) return;
        projectShadedVertex(a, c);
        projectShadedVertex(b, c);
        rasterizeLine(c, sg, rt, a, b, budget, true);
        return;
    }
    if (!std::isfinite(v0.screenX) || !std::isfinite(v0.screenY) ||
        !std::isfinite(v1.screenX) || !std::isfinite(v1.screenY))
        return;
    // Walk only the portion inside the framebuffer. A clip-space x of 1e10
    // projects past INT_MAX, and int(ceil(delta)) is undefined and drops the line.
    const float x0 = v0.screenX, y0 = v0.screenY, x1 = v1.screenX, y1 = v1.screenY;
    const float dx = x1 - x0, dy = y1 - y0;
    float t0 = 0.0f, t1 = 1.0f;
    auto clipT = [&](float p, float q) {
        if (p == 0.0f) return q >= 0.0f;
        const float r = q / p;
        if (p < 0.0f) {
            if (r > t1) return false;
            if (r > t0) t0 = r;
        } else {
            if (r < t0) return false;
            if (r < t1) t1 = r;
        }
        return true;
    };
    const float xmax = float(rt.width), ymax = float(rt.height);
    if (!clipT(-dx, x0) || !clipT(dx, xmax - x0) || !clipT(-dy, y0) || !clipT(dy, ymax - y0) || !(t0 <= t1))
        return;
    const float sdx = dx * (t1 - t0), sdy = dy * (t1 - t0);
    const double span = std::max(std::abs(double(sdx)), std::abs(double(sdy)));
    const double cap = double(std::max(rt.width, rt.height)) * 2.0 + 2.0;
    if (!std::isfinite(span) || span > cap) return;
    const int steps = std::max(1, int(std::ceil(span)));
    const Texture2D* tex = boundTextureForDraw(c, sg);
    for (int i = 0; i <= steps && !(budget && budget->exceeded); ++i) {
        const float u = float(i) / float(steps);
        const float t = t0 + (t1 - t0) * u;
        const float fx = x0 + dx * t;
        const float fy = y0 + dy * t;
        if (!(fx >= 0.0f && fy >= 0.0f && fx < xmax && fy < ymax)) continue;
        const int px = int(fx);
        const int py = int(fy);
        if (px < 0 || px >= rt.width || py < 0 || py >= rt.height) continue;
        const float z = v0.screenZ + (v1.screenZ - v0.screenZ) * t;
        if (c.api != 1 && c.currentProgram != 0) {
            if (auto frag = executeFragment(c, sg, v0, v1, v1, 1 - t, t, 0, px, py, z, true, 0, 0, budget))
                writeFragment(c, rt, px, py, z, *frag);
            continue;
        }
        Vec4 col{
            v0.color.x + (v1.color.x - v0.color.x) * t,
            v0.color.y + (v1.color.y - v0.color.y) * t,
            v0.color.z + (v1.color.z - v0.color.z) * t,
            v0.color.w + (v1.color.w - v0.color.w) * t
        };
        if (tex) {
            const Vec4 tc = tex->sample(v0.u + (v1.u - v0.u) * t, v0.v + (v1.v - v0.v) * t);
            col = {col.x * tc.x, col.y * tc.y, col.z * tc.z, col.w * tc.w};
        }
        writeFragment(c, rt, px, py, z, col);
    }
}

void drawPrimitives(Ctx& x, uint32_t mode, const std::vector<int>& indices) {
    ContextData* c = currentCtx(x);
    SharegroupData* sg = currentSg(x, c);
    if (!c || !sg || indices.empty()) return;
    std::lock_guard<std::mutex> programLock(sg->programMu);
    ProgramObject* bound = nullptr;
    if (c->api != 1 && c->currentProgram != 0) {
        auto it = sg->programs.find(c->currentProgram);
        if (it == sg->programs.end() || !it->second.linked) { c->lastError = GL_INVALID_OPERATION; return; }
        bound = &it->second;
    }
    RenderTarget rt = activeTarget(x, *c, *sg);
    if (!rt.rgba || rt.width <= 0 || rt.height <= 0) return;

    DrawBudget limit;
    if (bound) {
        limit.ctx = &x;
        limit.context = c;
        limit.program = bound;
    }
    DrawBudget* lim = bound ? &limit : nullptr;
    BudgetReport report{lim};

    std::vector<ShadedVertex> verts;
    verts.reserve(indices.size());
    for (int idx : indices) {
        if (lim && lim->exceeded) break;
        verts.push_back(shadeVertex(x, *c, *sg, idx, lim));
    }
    if (lim && lim->exceeded) return;
    // Debugging aid: HVM_TRACE_DRAW=1 prints each draw call (mode, vertex count, texture, blend).
    if (static const bool trace = std::getenv("HVM_TRACE_DRAW") != nullptr; trace) {
        const Texture2D* t = boundTextureForDraw(*c, *sg);
        std::fprintf(stderr, "draw mode %u n %zu tex %dx%d blend %d %x/%x first (%.2f,%.2f uv %.4f,%.4f) col %.2f,%.2f,%.2f,%.2f\n",
                     mode, verts.size(), t ? t->width : 0, t ? t->height : 0, c->blend, c->blendSrcRGB, c->blendDstRGB,
                     verts[0].screenX, verts[0].screenY, verts[0].u, verts[0].v, verts[0].color.x, verts[0].color.y, verts[0].color.z, verts[0].color.w);
    }

    switch (mode) {
        case GL_TRIANGLES:
            for (size_t i = 0; i + 2 < verts.size() && !(lim && lim->exceeded); i += 3)
                rasterizeTriangle(*c, *sg, rt, verts[i], verts[i + 1], verts[i + 2], lim);
            break;
        case GL_TRIANGLE_STRIP:
            for (size_t i = 0; i + 2 < verts.size() && !(lim && lim->exceeded); ++i) {
                if (i & 1) rasterizeTriangle(*c, *sg, rt, verts[i + 1], verts[i], verts[i + 2], lim);
                else rasterizeTriangle(*c, *sg, rt, verts[i], verts[i + 1], verts[i + 2], lim);
            }
            break;
        case GL_TRIANGLE_FAN:
            for (size_t i = 1; i + 1 < verts.size() && !(lim && lim->exceeded); ++i)
                rasterizeTriangle(*c, *sg, rt, verts[0], verts[i], verts[i + 1], lim);
            break;
        case GL_LINES:
            for (size_t i = 0; i + 1 < verts.size() && !(lim && lim->exceeded); i += 2)
                rasterizeLine(*c, *sg, rt, verts[i], verts[i + 1], lim);
            break;
        case GL_LINE_STRIP:
            for (size_t i = 0; i + 1 < verts.size() && !(lim && lim->exceeded); ++i)
                rasterizeLine(*c, *sg, rt, verts[i], verts[i + 1], lim);
            break;
        case GL_LINE_LOOP:
            for (size_t i = 0; i < verts.size() && !(lim && lim->exceeded); ++i)
                rasterizeLine(*c, *sg, rt, verts[i], verts[(i + 1) % verts.size()], lim);
            break;
        case GL_POINTS: {
            for (const auto& v : verts) {
                if (lim && lim->exceeded) break;
                if (!insideClipVolume(v)) continue;
                if (!std::isfinite(v.screenX) || !std::isfinite(v.screenY)) continue;
                if (v.screenX < -1.0e7f || v.screenX > 1.0e7f || v.screenY < -1.0e7f || v.screenY > 1.0e7f) continue;
                const bool shaded = c->api != 1 && c->currentProgram != 0;
                const float ps = v.shaderPointSize > 0.f ? v.shaderPointSize : c->pointSize;
                const int r = std::max(0, int(ps / 2.0f));
                const float size = std::max(ps, 1.f);
                const int cx = int(v.screenX), cy = int(v.screenY);
                for (int dy = -r; dy <= r && !(lim && lim->exceeded); ++dy) {
                    for (int dx = -r; dx <= r && !(lim && lim->exceeded); ++dx) {
                        const int px = cx + dx, py = cy + dy;
                        if (px < 0 || px >= rt.width || py < 0 || py >= rt.height) continue;
                        if (!shaded) { writeFragment(*c, rt, px, py, v.screenZ, v.color); continue; }
                        const float pcx = (float(px) + 0.5f - v.screenX) / size + 0.5f;
                        const float pcy = (float(py) + 0.5f - v.screenY) / size + 0.5f;
                        if (auto frag = executeFragment(*c, *sg, v, v, v, 1, 0, 0, px, py, v.screenZ, true, pcx, pcy, lim))
                            writeFragment(*c, rt, px, py, v.screenZ, *frag);
                    }
                }
            }
            break;
        }
        default: break;
    }
}

void gl_drawArrays(Ctx& x, uint32_t mode, int32_t first, int32_t count) {
    if (count <= 0 || first < 0) return;
    totalDrawCalls++;
    std::vector<int> indices(static_cast<size_t>(count));
    for (int32_t i = 0; i < count; ++i) indices[size_t(i)] = first + i;
    drawPrimitives(x, mode, indices);
}
void gl_drawArraysInstanced(Ctx& x, uint32_t mode, int32_t first, int32_t count, int32_t primcount) {
    for (int32_t i = 0; i < primcount; ++i) gl_drawArrays(x, mode, first, count);
}

void gl_drawElements(Ctx& x, uint32_t mode, int32_t count, uint32_t type, GuestPtr indicesPtr) {
    totalDrawCalls++;
    ContextData* c = currentCtx(x);
    SharegroupData* sg = currentSg(x, c);
    if (!c || !sg || count <= 0) return;
    const int elemBytes = (type == GL_UNSIGNED_BYTE) ? 1 : (type == GL_UNSIGNED_SHORT) ? 2 : 4;
    const uint8_t* src = nullptr;
    if (currentVao(c).elementArrayBuffer != 0) {
        auto bit = sg->buffers.find(currentVao(c).elementArrayBuffer);
        if (bit == sg->buffers.end() || size_t(indicesPtr) + size_t(count * elemBytes) > bit->second.data.size()) return;
        src = bit->second.data.data() + size_t(indicesPtr);
    } else {
        if (!indicesPtr) return;
        src = x.mem.host(indicesPtr);
    }
    std::vector<int> indices(static_cast<size_t>(count));
    for (int32_t i = 0; i < count; ++i) {
        const uint8_t* p = src + i * elemBytes;
        if (elemBytes == 1) indices[size_t(i)] = *p;
        else if (elemBytes == 2) { uint16_t v; std::memcpy(&v, p, 2); indices[size_t(i)] = v; }
        else { uint32_t v; std::memcpy(&v, p, 4); indices[size_t(i)] = int(v); }
    }
    drawPrimitives(x, mode, indices);
}
void gl_drawElementsInstanced(Ctx& x, uint32_t mode, int32_t count, uint32_t type, GuestPtr indicesPtr, int32_t primcount) {
    for (int32_t i = 0; i < primcount; ++i) gl_drawElements(x, mode, count, type, indicesPtr);
}
void gl_vertexAttribDivisor(Ctx&, uint32_t, uint32_t) {}
void gl_texStorage2D(Ctx& x, uint32_t target, int32_t levels, uint32_t internalformat, int32_t width, int32_t height) {
    for (int32_t l = 0; l < levels; ++l) {
        gl_texImage2D(x, target, l, internalformat, std::max(1, width >> l), std::max(1, height >> l), 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    }
}
void gl_copyTextureLevels(Ctx&, uint32_t, uint32_t, int32_t, int32_t) {}
void gl_getShaderSource(Ctx& x, uint32_t shader, int32_t bufsize, GuestPtr lengthOut, GuestPtr sourceOut) {
    if (bufsize <= 0 || !sourceOut) return;
    SharegroupData* sg = currentSg(x, currentCtx(x));
    if (!sg || !sg->shaders.count(shader)) return;
    const std::string& src = sg->shaders[shader].source;
    const int32_t n = std::min<int32_t>(bufsize - 1, int32_t(src.size()));
    std::memcpy(x.mem.host(sourceOut), src.data(), size_t(n));
    x.mem.write<uint8_t>(sourceOut + uint32_t(n), 0);
    if (lengthOut) x.mem.write<int32_t>(lengthOut, n);
}

// GL_OES_draw_texture: draws the bound texture directly into screen coordinates (x, y, z, w, h).
void gl_drawTexfOES(Ctx& x, float sx, float sy, float sz, float sw, float sh) {
    ContextData* c = currentCtx(x);
    SharegroupData* sg = currentSg(x, c);
    if (!c || !sg || sw <= 0 || sh <= 0) return;
    RenderTarget rt = activeTarget(x, *c, *sg);
    if (!rt.rgba) return;
    const Texture2D* tex = boundTextureForDraw(*c, *sg);
    float u0 = 0.0f, v0_coord = 0.0f, u1 = 1.0f, v1_coord = 1.0f;
    if (tex && tex->width > 0 && tex->height > 0) {
        if (tex->cropRect[2] != 0 && tex->cropRect[3] != 0) {
            u0 = float(tex->cropRect[0]) / float(tex->width);
            u1 = float(tex->cropRect[0] + tex->cropRect[2]) / float(tex->width);
            v0_coord = float(tex->cropRect[1]) / float(tex->height);
            v1_coord = float(tex->cropRect[1] + tex->cropRect[3]) / float(tex->height);
        }
    }
    ShadedVertex v0, v1, v2, v3;
    v0.screenX = sx;      v0.screenY = sy;      v0.screenZ = sz; v0.color = c->currentColor; v0.u = u0; v0.v = v0_coord;
    v1.screenX = sx + sw; v1.screenY = sy;      v1.screenZ = sz; v1.color = c->currentColor; v1.u = u1; v1.v = v0_coord;
    v2.screenX = sx + sw; v2.screenY = sy + sh; v2.screenZ = sz; v2.color = c->currentColor; v2.u = u1; v2.v = v1_coord;
    v3.screenX = sx;      v3.screenY = sy + sh; v3.screenZ = sz; v3.color = c->currentColor; v3.u = u0; v3.v = v1_coord;
    rasterizeTriangle(*c, *sg, rt, v0, v1, v2, nullptr);
    rasterizeTriangle(*c, *sg, rt, v0, v2, v3, nullptr);
}
void gl_drawTexiOES(Ctx& x, int32_t sx, int32_t sy, int32_t sz, int32_t sw, int32_t sh) {
    gl_drawTexfOES(x, float(sx), float(sy), float(sz), float(sw), float(sh));
}
void gl_drawTexsOES(Ctx& x, int32_t sx, int32_t sy, int32_t sz, int32_t sw, int32_t sh) {
    gl_drawTexfOES(x, float(int16_t(sx)), float(int16_t(sy)), float(int16_t(sz)), float(int16_t(sw)), float(int16_t(sh)));
}
void gl_drawTexxOES(Ctx& x, int32_t sx, int32_t sy, int32_t sz, int32_t sw, int32_t sh) {
    gl_drawTexfOES(x, sx / 65536.0f, sy / 65536.0f, sz / 65536.0f, sw / 65536.0f, sh / 65536.0f);
}

void gl_texEnvi(Ctx& x, uint32_t, uint32_t pname, int32_t param) {
    if (auto* c = currentCtx(x); c && pname == 0x2200 /* GL_TEXTURE_ENV_MODE */)
        c->texEnvMode[c->activeTexture & 7] = uint32_t(param);
}
void gl_texEnvf(Ctx& x, uint32_t t, uint32_t pname, float param) { gl_texEnvi(x, t, pname, int32_t(param)); }
void gl_texEnvx(Ctx& x, uint32_t t, uint32_t pname, int32_t param) { gl_texEnvi(x, t, pname, param); }

} // namespace

void registerOpenGLES(Process& p, ObjC::Runtime& r) {
    auto& st = GLS(p);

    // CAEAGLLayer
    ObjC::ClassBuilder eaglLayer = r.defineClass("CAEAGLLayer", "CALayer");
    st.eaglLayer = eaglLayer.cls();
    eaglLayer.m<&eaglLayer_init>("init")
             .m<&eaglLayer_initWithCoder>("initWithCoder:")
             .m<&eaglLayer_dealloc>("dealloc", "v@:")
             .m<&eaglLayer_props>("drawableProperties")
             .m<&eaglLayer_setProps>("setDrawableProperties:", "v@:@");

    for (const char* k : {"kEAGLDrawablePropertyRetainedBacking", "kEAGLDrawablePropertyColorFormat",
                          "kEAGLColorFormatRGBA8", "kEAGLColorFormatRGB565", "kEAGLColorFormatSRGBA8"}) {
        const uint32_t cell = p.defineDataBlock(std::string("_") + k, 4);
        p.mem().write<uint32_t>(cell, Foundation::constString(p, k));
    }

    // EAGLSharegroup & EAGLContext
    ObjC::ClassBuilder sg = r.defineClass("EAGLSharegroup", "NSObject");
    st.eaglSharegroup = sg.cls();
    sg.m<&sg_init>("init");

    ObjC::ClassBuilder ctx = r.defineClass("EAGLContext", "NSObject");
    st.eaglContext = ctx.cls();
    ctx.cm<&ctx_setCurrent>("setCurrentContext:", "c@:@")
       .cm<&ctx_current>("currentContext")
       .m<&ctx_initWithAPI>("initWithAPI:", "@@:I")
       .m<&ctx_initWithAPISharegroup>("initWithAPI:sharegroup:", "@@:I@")
       .m<&ctx_dealloc>("dealloc", "v@:")
       .m<&ctx_api>("API", "I@:")
       .m<&ctx_sharegroup>("sharegroup")
       .m<&ctx_renderbufferStorage>("renderbufferStorage:fromDrawable:", "c@:I@")
       .m<&ctx_presentRenderbuffer>("presentRenderbuffer:", "c@:I");

    // OpenGL ES 1.1 & 2.0 C functions
    p.def<&gl_getError>("_glGetError");
    p.def<&gl_getString>("_glGetString");
    p.def<&gl_enable>("_glEnable");
    p.def<&gl_disable>("_glDisable");
    p.def<&gl_isEnabled>("_glIsEnabled");
    p.def<&gl_viewport>("_glViewport");
    p.def<&gl_scissor>("_glScissor");
    p.def<&gl_clearColor>("_glClearColor");
    p.def<&gl_clearColorx>("_glClearColorx");
    p.def<&gl_clearDepthf>("_glClearDepthf");
    p.def<&gl_clearDepthx>("_glClearDepthx");
    p.def<&gl_clearStencil>("_glClearStencil");
    p.def<&gl_clear>("_glClear");
    p.def<&gl_blendFunc>("_glBlendFunc");
    p.def<&gl_blendFuncSeparate>("_glBlendFuncSeparate");
    p.def<&gl_blendColor>("_glBlendColor");
    p.def<&gl_blendColor>("_glBlendColorOES");
    p.def<&gl_blendEquation>("_glBlendEquation");
    p.def<&gl_blendEquationSeparate>("_glBlendEquationSeparate");
    p.def<&gl_blendEquation>("_glBlendEquationOES");
    p.def<&gl_blendFuncSeparate>("_glBlendFuncSeparateOES");
    p.def<&gl_blendEquationSeparate>("_glBlendEquationSeparateOES");
    p.def<&gl_depthFunc>("_glDepthFunc");
    p.def<&gl_depthMask>("_glDepthMask");
    p.def<&gl_depthRangef>("_glDepthRangef");
    p.def<&gl_depthRangex>("_glDepthRangex");
    p.def<&gl_colorMask>("_glColorMask");
    p.def<&gl_cullFace>("_glCullFace");
    p.def<&gl_frontFace>("_glFrontFace");
    p.def<&gl_alphaFunc>("_glAlphaFunc");
    p.def<&gl_alphaFuncx>("_glAlphaFuncx");
    p.def<&gl_pixelStorei>("_glPixelStorei");
    p.def<&gl_lineWidth>("_glLineWidth");
    p.def<&gl_lineWidthx>("_glLineWidthx");
    p.def<&gl_pointSize>("_glPointSize");
    p.def<&gl_pointSizex>("_glPointSizex");
    p.def<&gl_noop>("_glFlush");
    p.def<&gl_noop>("_glFinish");
    p.def<&gl_noop2>("_glHint");
    p.def<&gl_noop2>("_glPolygonOffset");
    p.def<&gl_noop2>("_glPolygonOffsetx");
    p.def<&gl_noop2>("_glSampleCoverage");
    p.def<&gl_noop2>("_glSampleCoveragex");
    p.def<&gl_stencilFunc>("_glStencilFunc");
    p.def<&gl_stencilOp>("_glStencilOp");
    p.def<&gl_stencilMask>("_glStencilMask");
    p.def<&gl_stencilFuncSeparate>("_glStencilFuncSeparate");
    p.def<&gl_stencilOpSeparate>("_glStencilOpSeparate");
    p.def<&gl_stencilMaskSeparate>("_glStencilMaskSeparate");
    p.def<&gl_getIntegerv>("_glGetIntegerv");
    p.def<&gl_getFloatv>("_glGetFloatv");
    p.def<&gl_getBooleanv>("_glGetBooleanv");
    p.def<&gl_readPixels>("_glReadPixels");

    // FBOs & Renderbuffers (Core + OES)
    p.def<&gl_genRenderbuffers>("_glGenRenderbuffers");
    p.def<&gl_genRenderbuffers>("_glGenRenderbuffersOES");
    p.def<&gl_deleteRenderbuffers>("_glDeleteRenderbuffers");
    p.def<&gl_deleteRenderbuffers>("_glDeleteRenderbuffersOES");
    p.def<&gl_bindRenderbuffer>("_glBindRenderbuffer");
    p.def<&gl_bindRenderbuffer>("_glBindRenderbufferOES");
    p.def<&gl_isRenderbuffer>("_glIsRenderbuffer");
    p.def<&gl_isRenderbuffer>("_glIsRenderbufferOES");
    p.def<&gl_renderbufferStorage>("_glRenderbufferStorage");
    p.def<&gl_renderbufferStorageMultisample>("_glRenderbufferStorageMultisampleAPPLE");
    p.def<&gl_renderbufferStorageMultisample>("_glRenderbufferStorageMultisampleEXT");
    p.def<&gl_renderbufferStorageMultisample>("_glRenderbufferStorageMultisample");
    p.def<&gl_resolveMultisample>("_glResolveMultisampleFramebufferAPPLE");
    p.def<&gl_renderbufferStorage>("_glRenderbufferStorageOES");
    p.def<&gl_getRenderbufferParameteriv>("_glGetRenderbufferParameteriv");
    p.def<&gl_getRenderbufferParameteriv>("_glGetRenderbufferParameterivOES");

    p.def<&gl_genFramebuffers>("_glGenFramebuffers");
    p.def<&gl_genFramebuffers>("_glGenFramebuffersOES");
    p.def<&gl_deleteFramebuffers>("_glDeleteFramebuffers");
    p.def<&gl_deleteFramebuffers>("_glDeleteFramebuffersOES");
    p.def<&gl_bindFramebuffer>("_glBindFramebuffer");
    p.def<&gl_bindFramebuffer>("_glBindFramebufferOES");
    p.def<&gl_isFramebuffer>("_glIsFramebuffer");
    p.def<&gl_isFramebuffer>("_glIsFramebufferOES");
    p.def<&gl_checkFramebufferStatus>("_glCheckFramebufferStatus");
    p.def<&gl_checkFramebufferStatus>("_glCheckFramebufferStatusOES");
    p.def<&gl_framebufferRenderbuffer>("_glFramebufferRenderbuffer");
    p.def<&gl_framebufferRenderbuffer>("_glFramebufferRenderbufferOES");
    p.def<&gl_framebufferTexture2D>("_glFramebufferTexture2D");
    p.def<&gl_framebufferTexture2D>("_glFramebufferTexture2DOES");
    p.def<&gl_getFramebufferAttachmentParameteriv>("_glGetFramebufferAttachmentParameteriv");
    p.def<&gl_getFramebufferAttachmentParameteriv>("_glGetFramebufferAttachmentParameterivOES");
    p.def<&gl_blitFramebuffer>("_glBlitFramebuffer");
    p.def<&gl_blitFramebuffer>("_glBlitFramebufferANGLE");
    p.def<&gl_blitFramebuffer>("_glBlitFramebufferNV");
    p.def<&gl_blitFramebuffer>("_glBlitFramebufferEXT");
    p.def<&gl_noop3>("_glDiscardFramebufferEXT");
    p.def<&gl_noop3>("_glInvalidateFramebuffer");
    p.def<&gl_noop2>("_glPushGroupMarkerEXT");
    p.def<&gl_noop>("_glPopGroupMarkerEXT");

    // Buffers & VAOs
    p.def<&gl_genBuffers>("_glGenBuffers");
    p.def<&gl_deleteBuffers>("_glDeleteBuffers");
    p.def<&gl_bindBuffer>("_glBindBuffer");
    p.def<&gl_isBuffer>("_glIsBuffer");
    p.def<&gl_bufferData>("_glBufferData");
    p.def<&gl_bufferSubData>("_glBufferSubData");
    p.def<&gl_getBufferParameteriv>("_glGetBufferParameteriv");
    p.def<&gl_mapBufferOES>("_glMapBufferOES");
    p.def<&gl_mapBufferOES>("_glMapBuffer");
    p.def<&gl_mapBufferRange>("_glMapBufferRange");
    p.def<&gl_mapBufferRange>("_glMapBufferRangeEXT");
    p.def<&gl_flushMappedBufferRange>("_glFlushMappedBufferRange");
    p.def<&gl_flushMappedBufferRange>("_glFlushMappedBufferRangeEXT");
    p.def<&gl_unmapBufferOES>("_glUnmapBufferOES");
    p.def<&gl_unmapBufferOES>("_glUnmapBuffer");
    p.def<&gl_genVertexArraysOES>("_glGenVertexArraysOES");
    p.def<&gl_bindVertexArrayOES>("_glBindVertexArrayOES");
    p.def<&gl_deleteVertexArraysOES>("_glDeleteVertexArraysOES");
    p.def<&gl_isVertexArrayOES>("_glIsVertexArrayOES");

    // Textures
    p.def<&gl_genTextures>("_glGenTextures");
    p.def<&gl_deleteTextures>("_glDeleteTextures");
    p.def<&gl_bindTexture>("_glBindTexture");
    p.def<&gl_isTexture>("_glIsTexture");
    p.def<&gl_activeTexture>("_glActiveTexture");
    p.def<&gl_clientActiveTexture>("_glClientActiveTexture");
    p.def<&gl_texImage2D>("_glTexImage2D");
    p.def<&gl_texSubImage2D>("_glTexSubImage2D");
    p.def<&gl_compressedTexImage2D>("_glCompressedTexImage2D");
    p.def<&gl_compressedTexSubImage2D>("_glCompressedTexSubImage2D");
    p.def<&gl_texParameteri>("_glTexParameteri");
    p.def<&gl_texParameterf>("_glTexParameterf");
    p.def<&gl_texParameterx>("_glTexParameterx");
    p.def<&gl_texParameteriv>("_glTexParameteriv");
    p.def<&gl_texParameterfv>("_glTexParameterfv");
    p.def<&gl_noop1>("_glGenerateMipmap");
    p.def<&gl_noop1>("_glGenerateMipmapOES");

    // GLES 1.1 Fixed-Function
    p.def<&gl_matrixMode>("_glMatrixMode");
    p.def<&gl_loadIdentity>("_glLoadIdentity");
    p.def<&gl_loadMatrixf>("_glLoadMatrixf");
    p.def<&gl_loadMatrixx>("_glLoadMatrixx");
    p.def<&gl_multMatrixf>("_glMultMatrixf");
    p.def<&gl_multMatrixx>("_glMultMatrixx");
    p.def<&gl_pushMatrix>("_glPushMatrix");
    p.def<&gl_popMatrix>("_glPopMatrix");
    p.def<&gl_orthof>("_glOrthof");
    p.def<&gl_orthox>("_glOrthox");
    p.def<&gl_frustumf>("_glFrustumf");
    p.def<&gl_frustumx>("_glFrustumx");
    p.def<&gl_translatef>("_glTranslatef");
    p.def<&gl_translatex>("_glTranslatex");
    p.def<&gl_scalef>("_glScalef");
    p.def<&gl_scalex>("_glScalex");
    p.def<&gl_rotatef>("_glRotatef");
    p.def<&gl_rotatex>("_glRotatex");
    p.def<&gl_color4f>("_glColor4f");
    p.def<&gl_color4x>("_glColor4x");
    p.def<&gl_color4ub>("_glColor4ub");
    p.def<&gl_normal3f>("_glNormal3f");
    p.def<&gl_normal3x>("_glNormal3x");
    p.def<&gl_enableClientState>("_glEnableClientState");
    p.def<&gl_disableClientState>("_glDisableClientState");
    p.def<&gl_vertexPointer>("_glVertexPointer");
    p.def<&gl_colorPointer>("_glColorPointer");
    p.def<&gl_texCoordPointer>("_glTexCoordPointer");
    p.def<&gl_normalPointer>("_glNormalPointer");
    p.def<&gl_pointSizePointerOES>("_glPointSizePointerOES");
    p.def<&gl_texEnvi>("_glTexEnvi");
    p.def<&gl_texEnvf>("_glTexEnvf");
    p.def<&gl_texEnvx>("_glTexEnvx");
    p.def<&gl_noop3>("_glTexEnviv");
    p.def<&gl_noop3>("_glTexEnvfv");
    p.def<&gl_noop3>("_glTexEnvxv");
    p.def<&gl_noop1>("_glShadeModel");
    p.def<&gl_noop3>("_glLightf");
    p.def<&gl_noop3>("_glLightfv");
    p.def<&gl_noop3>("_glLightx");
    p.def<&gl_noop3>("_glLightxv");
    p.def<&gl_noop2>("_glLightModelf");
    p.def<&gl_noop2>("_glLightModelfv");
    p.def<&gl_noop3>("_glMaterialf");
    p.def<&gl_noop3>("_glMaterialfv");
    p.def<&gl_noop2>("_glFogf");
    p.def<&gl_noop2>("_glFogfv");
    p.def<&gl_noop2>("_glFogx");
    p.def<&gl_noop2>("_glFogxv");
    p.def<&gl_drawTexfOES>("_glDrawTexfOES");
    p.def<&gl_drawTexiOES>("_glDrawTexiOES");
    p.def<&gl_drawTexsOES>("_glDrawTexsOES");
    p.def<&gl_drawTexxOES>("_glDrawTexxOES");

    // GLES 2.0 Shaders, Programs, Attribs, Uniforms
    p.def<&gl_createShader>("_glCreateShader");
    p.def<&gl_deleteShader>("_glDeleteShader");
    p.def<&gl_isShader>("_glIsShader");
    p.def<&gl_shaderSource>("_glShaderSource");
    p.def<&gl_compileShader>("_glCompileShader");
    p.def<&gl_getShaderiv>("_glGetShaderiv");
    p.def<&gl_getShaderInfoLog>("_glGetShaderInfoLog");
    p.def<&gl_getShaderPrecisionFormat>("_glGetShaderPrecisionFormat");
    p.def<&gl_noop>("_glReleaseShaderCompiler");
    p.def<&gl_createProgram>("_glCreateProgram");
    p.def<&gl_deleteProgram>("_glDeleteProgram");
    p.def<&gl_isProgram>("_glIsProgram");
    p.def<&gl_attachShader>("_glAttachShader");
    p.def<&gl_detachShader>("_glDetachShader");
    p.def<&gl_bindAttribLocation>("_glBindAttribLocation");
    p.def<&gl_linkProgram>("_glLinkProgram");
    p.def<&gl_useProgram>("_glUseProgram");
    p.def<&gl_validateProgram>("_glValidateProgram");
    p.def<&gl_getProgramiv>("_glGetProgramiv");
    p.def<&gl_getProgramInfoLog>("_glGetProgramInfoLog");
    p.def<&gl_getAttribLocation>("_glGetAttribLocation");
    p.def<&gl_getUniformLocation>("_glGetUniformLocation");
    p.def<&gl_getActiveAttrib>("_glGetActiveAttrib");
    p.def<&gl_getActiveUniform>("_glGetActiveUniform");
    p.def<&gl_enableVertexAttribArray>("_glEnableVertexAttribArray");
    p.def<&gl_disableVertexAttribArray>("_glDisableVertexAttribArray");
    p.def<&gl_vertexAttribPointer>("_glVertexAttribPointer");
    p.def<&gl_vertexAttrib1f>("_glVertexAttrib1f");
    p.def<&gl_vertexAttrib2f>("_glVertexAttrib2f");
    p.def<&gl_vertexAttrib3f>("_glVertexAttrib3f");
    p.def<&gl_vertexAttrib4f>("_glVertexAttrib4f");
    p.def<&gl_vertexAttrib4fv>("_glVertexAttrib4fv");
    p.def<&gl_drawBuffers>("_glDrawBuffers");
    p.def<&gl_readBuffer>("_glReadBuffer");
    p.def<&gl_getVertexAttribfv>("_glGetVertexAttribfv");
    p.def<&gl_genQueriesEXT>("_glGenQueriesEXT");
    p.def<&gl_genQueriesEXT>("_glGenQueries");
    p.def<&gl_deleteQueriesEXT>("_glDeleteQueriesEXT");
    p.def<&gl_deleteQueriesEXT>("_glDeleteQueries");
    p.def<&gl_isQueryEXT>("_glIsQueryEXT");
    p.def<&gl_isQueryEXT>("_glIsQuery");
    p.def<&gl_beginQueryEXT>("_glBeginQueryEXT");
    p.def<&gl_beginQueryEXT>("_glBeginQuery");
    p.def<&gl_endQueryEXT>("_glEndQueryEXT");
    p.def<&gl_endQueryEXT>("_glEndQuery");
    p.def<&gl_getQueryivEXT>("_glGetQueryivEXT");
    p.def<&gl_getQueryivEXT>("_glGetQueryiv");
    p.def<&gl_getQueryObjectuivEXT>("_glGetQueryObjectuivEXT");
    p.def<&gl_getQueryObjectuivEXT>("_glGetQueryObjectuiv");
    p.def<&gl_getVertexAttribiv>("_glGetVertexAttribiv");
    p.def<&gl_getVertexAttribPointerv>("_glGetVertexAttribPointerv");
    p.def<&gl_uniform1i>("_glUniform1i");
    p.def<&gl_uniform2i>("_glUniform2i");
    p.def<&gl_uniform3i>("_glUniform3i");
    p.def<&gl_uniform4i>("_glUniform4i");
    p.def<&gl_uniform1iv>("_glUniform1iv");
    p.def<&gl_uniform1f>("_glUniform1f");
    p.def<&gl_uniform2f>("_glUniform2f");
    p.def<&gl_uniform3f>("_glUniform3f");
    p.def<&gl_uniform4f>("_glUniform4f");
    p.def<&gl_uniform1fv>("_glUniform1fv");
    p.def<&gl_uniform2fv>("_glUniform2fv");
    p.def<&gl_uniform3fv>("_glUniform3fv");
    p.def<&gl_uniform4fv>("_glUniform4fv");
    p.def<&gl_uniformMatrix2fv>("_glUniformMatrix2fv");
    p.def<&gl_uniformMatrix3fv>("_glUniformMatrix3fv");
    p.def<&gl_uniformMatrix4fv>("_glUniformMatrix4fv");

    // Draw calls
    p.def<&gl_drawArrays>("_glDrawArrays");
    p.def<&gl_drawElements>("_glDrawElements");
    p.def<&gl_drawArraysInstanced>("_glDrawArraysInstancedEXT");
    p.def<&gl_drawArraysInstanced>("_glDrawArraysInstanced");
    p.def<&gl_drawElementsInstanced>("_glDrawElementsInstancedEXT");
    p.def<&gl_drawElementsInstanced>("_glDrawElementsInstanced");
    p.def<&gl_vertexAttribDivisor>("_glVertexAttribDivisorEXT");
    p.def<&gl_vertexAttribDivisor>("_glVertexAttribDivisor");
    p.def<&gl_texStorage2D>("_glTexStorage2DEXT");
    p.def<&gl_texStorage2D>("_glTexStorage2D");
    p.def<&gl_copyTextureLevels>("_glCopyTextureLevelsAPPLE");
    p.def<&gl_getShaderSource>("_glGetShaderSource");

    // GPU backend: forward the C functions to the host driver (overrides the above).
    if (HostGL::enabled(p)) HostGL::registerFunctions(p);

    p.onExit([&p] {
        CPU::Cpu cpu{};
        Ctx x{p, cpu, p.mem()};
        auto& u = ui(p);
        for (id l : u.eaglLayers) {
            LayerData& ld = layerData(x, l);
            if (!ld.everOnScreen && ld.pendingOffscreen > 0) {
                p.perf().offscreenPresents += ld.pendingOffscreen;
            }
        }
    });
}

} // namespace Hydrogen::Runtime::UIKit
