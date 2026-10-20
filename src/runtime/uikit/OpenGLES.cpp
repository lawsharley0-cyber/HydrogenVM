// HydrogenVM — OpenGL ES 1.1 & 2.0, EAGLContext, EAGLSharegroup, and CAEAGLLayer.
//
// Provides a complete OpenGL ES 1.1 (fixed-function + OES extensions incl.
// OES_framebuffer_object and OES_draw_texture) and OpenGL ES 2.0 (shaders,
// programs, uniforms, vertex attributes, VBOs, FBOs, textures) state tracker
// and software rasterizer wired into CAEAGLLayer via LayerData::externalContents.
#include "UIKit.hpp"
#include "runtime/graphics/Pvrtc.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
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
    GL_FLOAT_MAT2 = 0x8B5A,
    GL_FLOAT_MAT3 = 0x8B5B,
    GL_FLOAT_MAT4 = 0x8B5C,
    GL_SAMPLER_2D = 0x8B5E,
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
    bool compiled = true;
    std::string infoLog;
};

struct ShaderVar {
    std::string name;
    uint32_t type = GL_FLOAT_VEC4;
    int size = 1;
    int location = 0;
};

struct UniformValue {
    uint32_t type = GL_FLOAT_VEC4;
    float f[16] = {0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    int i[4] = {0, 0, 0, 0};
    bool isSet = false;
};

struct ProgramObject {
    uint32_t id = 0;
    std::vector<uint32_t> shaders;
    bool linked = false;
    std::string infoLog;
    std::map<std::string, int> boundAttribLocations;
    std::vector<ShaderVar> attribs;
    std::vector<ShaderVar> uniforms;
    std::unordered_map<int, UniformValue> uniformValues;

    // Auto-detected roles for shader execution:
    int posAttrib = 0;
    int colorAttrib = -1;
    int texCoordAttrib = -1;
    int mvpUniform = -1;
    int fragColorUniform = -1;
    int samplerUniform = -1;
    bool usesTexture = false;
    bool usesVertexColor = false;
    Vec4 constFragColor{1, 1, 1, 1};
    bool hasConstFragColor = false;
};

struct SharegroupData : ObjC::HostObject {
    uint32_t nextBuffer = 1, nextTexture = 1, nextRenderbuffer = 1, nextFramebuffer = 1, nextShader = 1, nextProgram = 1;
    std::unordered_map<uint32_t, BufferObject> buffers;
    std::unordered_map<uint32_t, Texture2D> textures;
    std::unordered_map<uint32_t, Renderbuffer> renderbuffers;
    std::unordered_map<uint32_t, Framebuffer> framebuffers;
    std::unordered_map<uint32_t, ShaderObject> shaders;
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

struct ContextData : ObjC::HostObject {
    uint32_t api = 2;
    ObjC::id sharegroup = 0;   // retained EAGLSharegroup
    uint32_t lastError = GL_NO_ERROR;

    // Bindings
    uint32_t arrayBuffer = 0;
    uint32_t elementArrayBuffer = 0;
    uint32_t framebuffer = 0;
    uint32_t renderbuffer = 0;
    uint32_t activeTexture = 0;           // 0..7
    uint32_t clientActiveTexture = 0;     // 0..7
    uint32_t boundTextures[8] = {};
    uint32_t currentProgram = 0;
    uint32_t boundVao = 0;

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
    uint32_t depthFunc = GL_LESS;
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
    ArrayPointer vertexPtr;
    ArrayPointer colorPtr;
    ArrayPointer normalPtr;
    ArrayPointer texCoordPtr[8];

    // GLES 2.0 Vertex Attributes (16 slots)
    ArrayPointer attribPtr[16];
    Vec4 genericAttrib[16];

    // Default FBO (0) fallback renderbuffer
    Renderbuffer defaultRb;

    ContextData() {
        for (int i = 0; i < 16; ++i) genericAttrib[i] = {0, 0, 0, 1};
        defaultRb.width = 320;
        defaultRb.height = 480;
        defaultRb.rgba.assign(320 * 480 * 4, 0);
        defaultRb.depth.assign(320 * 480, 1.0f);
    }

    std::vector<Mat4>& currentMatrixStack() {
        if (matrixMode == GL_PROJECTION) return projectionStack;
        if (matrixMode == GL_TEXTURE) return textureStack;
        return modelviewStack;
    }
};

struct GLState {
    explicit GLState(Process&) {}
    Class eaglLayer = 0, eaglContext = 0, eaglSharegroup = 0;
    std::mutex mu;
    std::unordered_map<uint32_t, ObjC::id> currentByThread; // GuestThread::id -> EAGLContext id
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
        if (it != st.currentByThread.end()) cid = it->second;
    }
    return cid ? rt(x).hostIf<ContextData>(cid) : nullptr;
}

SharegroupData* currentSg(Ctx& x, ContextData* c) {
    if (!c || !c->sharegroup) return nullptr;
    return rt(x).hostIf<SharegroupData>(c->sharegroup);
}

// Obtain the active color and depth target for the current context.
struct RenderTarget {
    uint8_t* rgba = nullptr;
    float* depth = nullptr;
    int width = 0, height = 0;
    bool isTexture = false;
};

RenderTarget activeTarget(Ctx& x, ContextData& c, SharegroupData& sg) {
    if (c.framebuffer != 0) {
        auto fit = sg.framebuffers.find(c.framebuffer);
        if (fit != sg.framebuffers.end()) {
            const Framebuffer& fb = fit->second;
            RenderTarget rtOut{};
            if (fb.colorAttachmentType == GL_RENDERBUFFER && fb.colorAttachmentId != 0) {
                auto rbit = sg.renderbuffers.find(fb.colorAttachmentId);
                if (rbit != sg.renderbuffers.end()) {
                    Renderbuffer& rb = rbit->second;
                    rtOut.rgba = rb.rgba.data();
                    rtOut.width = rb.width;
                    rtOut.height = rb.height;
                    // No depth here: a framebuffer without a depth attachment has no depth
                    // buffer, so depth testing is skipped (as in GL). Using the colour
                    // renderbuffer's private depth made coplanar 2D sprites z-fight.
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
            if (fb.depthAttachmentType == GL_RENDERBUFFER && fb.depthAttachmentId != 0) {
                auto dbit = sg.renderbuffers.find(fb.depthAttachmentId);
                if (dbit != sg.renderbuffers.end()) {
                    Renderbuffer& drb = dbit->second;
                    if (drb.depth.size() != size_t(drb.width) * size_t(drb.height))
                        drb.depth.assign(size_t(drb.width) * size_t(drb.height), 1.0f);
                    rtOut.depth = drb.depth.data();
                }
            }
            if (rtOut.rgba && rtOut.width > 0 && rtOut.height > 0) return rtOut;
        }
    }
    if (c.renderbuffer != 0) {
        auto rbit = sg.renderbuffers.find(c.renderbuffer);
        if (rbit != sg.renderbuffers.end() && rbit->second.width > 0 && rbit->second.height > 0 && !rbit->second.rgba.empty()) {
            Renderbuffer& rb = rbit->second;
            if (rb.depth.size() != size_t(rb.width) * size_t(rb.height))
                rb.depth.assign(size_t(rb.width) * size_t(rb.height), 1.0f);
            return {rb.rgba.data(), rb.depth.data(), rb.width, rb.height, false};
        }
    }
    return {c.defaultRb.rgba.data(), c.defaultRb.depth.data(), c.defaultRb.width, c.defaultRb.height, false};
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
    return self;
}

void ctx_dealloc(Ctx& x, id self, SEL) {
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
    std::lock_guard<std::mutex> lk(st.mu);
    if (ctx) st.currentByThread[tid] = ctx;
    else st.currentByThread.erase(tid);
    return true;
}

id ctx_current(Ctx& x, id, SEL) {
    auto& st = GLS(x);
    const uint32_t tid = x.proc.currentThread().id;
    std::lock_guard<std::mutex> lk(st.mu);
    auto it = st.currentByThread.find(tid);
    return it != st.currentByThread.end() ? it->second : 0;
}

uint32_t ctx_api(Ctx& x, id self, SEL) { return rt(x).host<ContextData>(self).api; }
id ctx_sharegroup(Ctx& x, id self, SEL) { return rt(x).host<ContextData>(self).sharegroup; }

bool ctx_renderbufferStorage(Ctx& x, id self, SEL, uint32_t target, id drawable) {
    (void)target;
    ContextData& c = rt(x).host<ContextData>(self);
    SharegroupData* sg = currentSg(x, &c);
    if (!sg || c.renderbuffer == 0) return false;
    Renderbuffer& rb = sg->renderbuffers[c.renderbuffer];
    rb.id = c.renderbuffer;
    if (!drawable) {
        rb.width = rb.height = 0;
        rb.rgba.clear();
        rb.depth.clear();
        rb.drawableLayer = 0;
        return true;
    }
    LayerData& ld = layerData(x, drawable);
    const float scale = ld.contentsScale > 0 ? ld.contentsScale : ui(x).scale;
    float bw = ld.bounds.w > 0 ? ld.bounds.w : ui(x).screenW;
    float bh = ld.bounds.h > 0 ? ld.bounds.h : ui(x).screenH;
    rb.width = std::max(1, int(std::ceil(bw * scale)));
    rb.height = std::max(1, int(std::ceil(bh * scale)));
    rb.internalFormat = GL_RGBA8_OES;
    rb.rgba.assign(size_t(rb.width) * size_t(rb.height) * 4, 0);
    rb.depth.assign(size_t(rb.width) * size_t(rb.height), 1.0f);
    rb.drawableLayer = drawable;
    c.viewport[0] = 0; c.viewport[1] = 0; c.viewport[2] = rb.width; c.viewport[3] = rb.height;
    c.scissor[0] = 0;  c.scissor[1] = 0;  c.scissor[2] = rb.width;  c.scissor[3] = rb.height;
    return true;
}

bool ctx_presentRenderbuffer(Ctx& x, id self, SEL, uint32_t target) {
    (void)target;
    ContextData& c = rt(x).host<ContextData>(self);
    SharegroupData* sg = currentSg(x, &c);
    if (!sg) return false;
    uint32_t rbId = c.renderbuffer;
    if (rbId == 0 && c.framebuffer != 0) {
        auto fit = sg->framebuffers.find(c.framebuffer);
        if (fit != sg->framebuffers.end() && fit->second.colorAttachmentType == GL_RENDERBUFFER)
            rbId = fit->second.colorAttachmentId;
    }
    if (rbId == 0) return false;
    auto it = sg->renderbuffers.find(rbId);
    if (it == sg->renderbuffers.end()) return false;
    Renderbuffer& rb = it->second;
    if (!rb.drawableLayer || rb.width <= 0 || rb.height <= 0 || rb.rgba.empty()) return false;

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
            std::fwrite(rb.rgba.data(), 1, rb.rgba.size(), fp);
            std::fclose(fp);
        }
    }
    for (int y = 0; y < rb.height; ++y) {
        const uint8_t* srcRow = &rb.rgba[size_t(rb.height - 1 - y) * size_t(rb.width) * 4];
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
    return true;
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
                "GL_APPLE_framebuffer_multisample GL_EXT_discard_framebuffer");
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
        case GL_VERTEX_ARRAY: return c->vertexPtr.enabled;
        case GL_COLOR_ARRAY: return c->colorPtr.enabled;
        case GL_NORMAL_ARRAY: return c->normalPtr.enabled;
        case GL_TEXTURE_COORD_ARRAY: return c->texCoordPtr[c->clientActiveTexture & 7].enabled;
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
    if (!rt.rgba || rt.width <= 0 || rt.height <= 0) return;

    int x0 = 0, y0 = 0, x1 = rt.width, y1 = rt.height;
    if (c->scissorTest) {
        x0 = std::clamp(c->scissor[0], 0, rt.width);
        y0 = std::clamp(c->scissor[1], 0, rt.height);
        x1 = std::clamp(c->scissor[0] + c->scissor[2], 0, rt.width);
        y1 = std::clamp(c->scissor[1] + c->scissor[3], 0, rt.height);
    }
    if (x1 <= x0 || y1 <= y0) return;

    if (mask & GL_COLOR_BUFFER_BIT) {
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
void gl_noop4(Ctx&, uint32_t, uint32_t, uint32_t, uint32_t) {}

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
        case GL_RENDERBUFFER_BINDING: wr(c ? int32_t(c->renderbuffer) : 0); break;
        case GL_ARRAY_BUFFER_BINDING: wr(c ? int32_t(c->arrayBuffer) : 0); break;
        case GL_ELEMENT_ARRAY_BUFFER_BINDING: wr(c ? int32_t(c->elementArrayBuffer) : 0); break;
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
    rb.rgba.assign(size_t(width) * size_t(height) * 4, 0);
    rb.depth.assign(size_t(width) * size_t(height), 1.0f);
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
        case GL_RENDERBUFFER_RED_SIZE: case GL_RENDERBUFFER_GREEN_SIZE: case GL_RENDERBUFFER_BLUE_SIZE: case GL_RENDERBUFFER_ALPHA_SIZE: v = 8; break;
        case GL_RENDERBUFFER_DEPTH_SIZE: v = 24; break;
        case GL_RENDERBUFFER_STENCIL_SIZE: v = 8; break;
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
    (void)target;
    ContextData* c = currentCtx(x);
    SharegroupData* sg = currentSg(x, c);
    if (!c || !sg) return;
    c->framebuffer = fb;
    if (fb != 0) sg->framebuffers[fb].id = fb;
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
            if (c->elementArrayBuffer == id) c->elementArrayBuffer = 0;
        }
    }
}
void gl_bindBuffer(Ctx& x, uint32_t target, uint32_t buffer) {
    ContextData* c = currentCtx(x);
    SharegroupData* sg = currentSg(x, c);
    if (!c || !sg) return;
    if (target == GL_ARRAY_BUFFER) c->arrayBuffer = buffer;
    else if (target == GL_ELEMENT_ARRAY_BUFFER) c->elementArrayBuffer = buffer;
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
    const uint32_t bid = (target == GL_ELEMENT_ARRAY_BUFFER) ? c->elementArrayBuffer : c->arrayBuffer;
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
    const uint32_t bid = (target == GL_ELEMENT_ARRAY_BUFFER) ? c->elementArrayBuffer : c->arrayBuffer;
    if (!bid) return;
    BufferObject& bo = sg->buffers[bid];
    if (size_t(offset) + size_t(size) > bo.data.size()) bo.data.resize(size_t(offset) + size_t(size));
    x.mem.copyOut(bo.data.data() + offset, data, size_t(size));
}
void gl_getBufferParameteriv(Ctx& x, uint32_t target, uint32_t pname, GuestPtr params) {
    ContextData* c = currentCtx(x);
    SharegroupData* sg = currentSg(x, c);
    if (!c || !sg || !params) return;
    const uint32_t bid = (target == GL_ELEMENT_ARRAY_BUFFER) ? c->elementArrayBuffer : c->arrayBuffer;
    if (!bid || !sg->buffers.count(bid)) { x.mem.write<int32_t>(params, 0); return; }
    const BufferObject& bo = sg->buffers[bid];
    if (pname == GL_BUFFER_SIZE) x.mem.write<int32_t>(params, int32_t(bo.data.size()));
    else if (pname == GL_BUFFER_USAGE) x.mem.write<int32_t>(params, int32_t(bo.usage));
}
GuestPtr gl_mapBufferOES(Ctx& x, uint32_t target, uint32_t) {
    ContextData* c = currentCtx(x);
    SharegroupData* sg = currentSg(x, c);
    if (!c || !sg) return 0;
    const uint32_t bid = (target == GL_ELEMENT_ARRAY_BUFFER) ? c->elementArrayBuffer : c->arrayBuffer;
    if (!bid || !sg->buffers.count(bid)) return 0;
    BufferObject& bo = sg->buffers[bid];
    if (!bo.mappedGuestAddr && !bo.data.empty()) {
        bo.mappedGuestAddr = x.proc.heap().alloc(uint32_t(bo.data.size()));
        x.mem.copyIn(bo.mappedGuestAddr, bo.data.data(), bo.data.size());
    }
    return bo.mappedGuestAddr;
}
bool gl_unmapBufferOES(Ctx& x, uint32_t target) {
    ContextData* c = currentCtx(x);
    SharegroupData* sg = currentSg(x, c);
    if (!c || !sg) return false;
    const uint32_t bid = (target == GL_ELEMENT_ARRAY_BUFFER) ? c->elementArrayBuffer : c->arrayBuffer;
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
    if (array == GL_VERTEX_ARRAY) c->vertexPtr.enabled = true;
    else if (array == GL_COLOR_ARRAY) c->colorPtr.enabled = true;
    else if (array == GL_NORMAL_ARRAY) c->normalPtr.enabled = true;
    else if (array == GL_TEXTURE_COORD_ARRAY) c->texCoordPtr[c->clientActiveTexture & 7].enabled = true;
}
void gl_disableClientState(Ctx& x, uint32_t array) {
    auto* c = currentCtx(x);
    if (!c) return;
    if (array == GL_VERTEX_ARRAY) c->vertexPtr.enabled = false;
    else if (array == GL_COLOR_ARRAY) c->colorPtr.enabled = false;
    else if (array == GL_NORMAL_ARRAY) c->normalPtr.enabled = false;
    else if (array == GL_TEXTURE_COORD_ARRAY) c->texCoordPtr[c->clientActiveTexture & 7].enabled = false;
}
void gl_vertexPointer(Ctx& x, int32_t size, uint32_t type, int32_t stride, GuestPtr ptr) {
    if (auto* c = currentCtx(x)) { c->vertexPtr = {c->vertexPtr.enabled, size, type, false, stride, ptr, c->arrayBuffer}; }
}
void gl_colorPointer(Ctx& x, int32_t size, uint32_t type, int32_t stride, GuestPtr ptr) {
    if (auto* c = currentCtx(x)) { c->colorPtr = {c->colorPtr.enabled, size, type, type != GL_FLOAT && type != GL_FIXED, stride, ptr, c->arrayBuffer}; }
}
void gl_texCoordPointer(Ctx& x, int32_t size, uint32_t type, int32_t stride, GuestPtr ptr) {
    if (auto* c = currentCtx(x)) {
        auto& tp = c->texCoordPtr[c->clientActiveTexture & 7];
        tp = {tp.enabled, size, type, false, stride, ptr, c->arrayBuffer};
    }
}
void gl_normalPointer(Ctx& x, uint32_t type, int32_t stride, GuestPtr ptr) {
    if (auto* c = currentCtx(x)) { c->normalPtr = {c->normalPtr.enabled, 3, type, true, stride, ptr, c->arrayBuffer}; }
}

// =================================== GLES 2.0 Shaders, Programs & Uniforms ===

uint32_t gl_createShader(Ctx& x, uint32_t type) {
    ContextData* c = currentCtx(x);
    SharegroupData* sg = currentSg(x, c);
    if (!sg) return 0;
    const uint32_t id = sg->nextShader++;
    sg->shaders[id] = {id, type, "", true, ""};
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
    sg->shaders[shader].source = std::move(src);
}
void gl_compileShader(Ctx& x, uint32_t shader) {
    if (auto* sg = currentSg(x, currentCtx(x)); sg && sg->shaders.count(shader))
        sg->shaders[shader].compiled = true;
}
void gl_getShaderiv(Ctx& x, uint32_t shader, uint32_t pname, GuestPtr params) {
    auto* sg = currentSg(x, currentCtx(x));
    if (!sg || !params || !sg->shaders.count(shader)) return;
    const ShaderObject& sh = sg->shaders[shader];
    int32_t v = 0;
    if (pname == GL_COMPILE_STATUS) v = sh.compiled ? 1 : 0;
    else if (pname == GL_INFO_LOG_LENGTH) v = int32_t(sh.infoLog.size());
    else if (pname == GL_SHADER_SOURCE_LENGTH) v = int32_t(sh.source.size() + 1);
    x.mem.write<int32_t>(params, v);
}
void gl_getShaderInfoLog(Ctx& x, uint32_t, int32_t maxLen, GuestPtr length, GuestPtr infoLog) {
    if (length) x.mem.write<int32_t>(length, 0);
    if (infoLog && maxLen > 0) x.mem.write<uint8_t>(infoLog, 0);
}
void gl_getShaderPrecisionFormat(Ctx& x, uint32_t, uint32_t, GuestPtr range, GuestPtr precision) {
    if (range) { x.mem.write<int32_t>(range, 127); x.mem.write<int32_t>(range + 4, 127); }
    if (precision) x.mem.write<int32_t>(precision, 23);
}

uint32_t gl_createProgram(Ctx& x) {
    auto* sg = currentSg(x, currentCtx(x));
    if (!sg) return 0;
    const uint32_t id = sg->nextProgram++;
    sg->programs[id].id = id;
    return id;
}
void gl_deleteProgram(Ctx& x, uint32_t prog) {
    ContextData* c = currentCtx(x);
    SharegroupData* sg = currentSg(x, c);
    if (!sg) return;
    sg->programs.erase(prog);
    if (c && c->currentProgram == prog) c->currentProgram = 0;
}
bool gl_isProgram(Ctx& x, uint32_t prog) {
    auto* sg = currentSg(x, currentCtx(x));
    return sg && prog != 0 && sg->programs.count(prog) > 0;
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

uint32_t parseGlslType(const std::string& t) {
    if (t == "float") return GL_FLOAT;
    if (t == "vec2") return GL_FLOAT_VEC2;
    if (t == "vec3") return GL_FLOAT_VEC3;
    if (t == "vec4") return GL_FLOAT_VEC4;
    if (t == "mat2") return GL_FLOAT_MAT2;
    if (t == "mat3") return GL_FLOAT_MAT3;
    if (t == "mat4") return GL_FLOAT_MAT4;
    if (t == "int") return GL_INT;
    if (t == "ivec2") return GL_INT_VEC2;
    if (t == "ivec3") return GL_INT_VEC3;
    if (t == "ivec4") return GL_INT_VEC4;
    if (t == "bool") return GL_BOOL;
    if (t == "sampler2D") return GL_SAMPLER_2D;
    return GL_FLOAT_VEC4;
}

std::string lowerStr(std::string s) {
    for (char& c : s) c = char(std::tolower(uint8_t(c)));
    return s;
}

void gl_linkProgram(Ctx& x, uint32_t prog) {
    auto* sg = currentSg(x, currentCtx(x));
    if (!sg || !sg->programs.count(prog)) return;
    ProgramObject& p = sg->programs[prog];
    p.linked = true;
    p.attribs.clear();
    p.uniforms.clear();
    p.posAttrib = 0; p.colorAttrib = -1; p.texCoordAttrib = -1;
    p.mvpUniform = -1; p.fragColorUniform = -1; p.samplerUniform = -1;
    p.usesTexture = false; p.usesVertexColor = false; p.hasConstFragColor = false;

    std::string combinedSrc, fragSrc;
    for (uint32_t sid : p.shaders) {
        if (sg->shaders.count(sid)) {
            combinedSrc += "\n" + sg->shaders[sid].source;
            if (sg->shaders[sid].type == GL_FRAGMENT_SHADER) fragSrc += "\n" + sg->shaders[sid].source;
        }
    }

    // Parse attribute and uniform declarations: e.g. `attribute highp vec4 a_position;`
    std::regex declRx(R"(\b(attribute|uniform)\s+(?:(?:lowp|mediump|highp)\s+)?([A-Za-z0-9_]+)\s+([A-Za-z0-9_]+)(?:\s*\[\s*(\d+)\s*\])?\s*;)");
    std::sregex_iterator it(combinedSrc.begin(), combinedSrc.end(), declRx), end;
    uint32_t usedMask = 0;
    for (auto& [nm, loc] : p.boundAttribLocations) if (loc >= 0 && loc < 16) usedMask |= (1u << loc);

    for (; it != end; ++it) {
        const std::string qual = (*it)[1].str();
        const std::string typeStr = (*it)[2].str();
        const std::string name = (*it)[3].str();
        const int arrSize = (*it)[4].matched ? std::max(1, std::atoi((*it)[4].str().c_str())) : 1;
        const uint32_t glType = parseGlslType(typeStr);

        if (qual == "attribute") {
            bool exists = false;
            for (auto& a : p.attribs) if (a.name == name) { exists = true; break; }
            if (exists) continue;
            int loc = -1;
            auto bit = p.boundAttribLocations.find(name);
            if (bit != p.boundAttribLocations.end()) loc = bit->second;
            else {
                for (int k = 0; k < 16; ++k) if (!(usedMask & (1u << k))) { loc = k; usedMask |= (1u << k); break; }
            }
            if (loc < 0) loc = 0;
            p.attribs.push_back({name, glType, arrSize, loc});
            const std::string ln = lowerStr(name);
            if (ln.find("pos") != std::string::npos || ln == "vertex" || ln == "a_vertex") p.posAttrib = loc;
            else if (ln.find("color") != std::string::npos || ln.find("col") != std::string::npos) { p.colorAttrib = loc; p.usesVertexColor = true; }
            else if (ln.find("tex") != std::string::npos || ln.find("uv") != std::string::npos || ln.find("coord") != std::string::npos) p.texCoordAttrib = loc;
        } else {
            bool exists = false;
            for (auto& u : p.uniforms) if (u.name == name) { exists = true; break; }
            if (exists) continue;
            const int loc = int(p.uniforms.size());
            p.uniforms.push_back({name, glType, arrSize, loc});
            p.uniformValues[loc].type = glType;
            const std::string ln = lowerStr(name);
            if (glType == GL_FLOAT_MAT4 && p.mvpUniform < 0) p.mvpUniform = loc;
            else if (glType == GL_SAMPLER_2D && p.samplerUniform < 0) { p.samplerUniform = loc; p.usesTexture = true; }
            else if ((glType == GL_FLOAT_VEC4 || glType == GL_FLOAT_VEC3) && p.fragColorUniform < 0) p.fragColorUniform = loc;
        }
    }

    if (fragSrc.find("texture2D") != std::string::npos) p.usesTexture = true;
    // Detect constant gl_FragColor = vec4(r, g, b, a) in fragment shader:
    std::regex vec4Rx(R"(gl_FragColor\s*=\s*vec4\s*\(\s*([0-9.+-eE]+)\s*,\s*([0-9.+-eE]+)\s*,\s*([0-9.+-eE]+)\s*,\s*([0-9.+-eE]+)\s*\))");
    std::smatch m;
    if (std::regex_search(fragSrc, m, vec4Rx)) {
        p.constFragColor = {
            std::strtof(m[1].str().c_str(), nullptr),
            std::strtof(m[2].str().c_str(), nullptr),
            std::strtof(m[3].str().c_str(), nullptr),
            std::strtof(m[4].str().c_str(), nullptr)
        };
        p.hasConstFragColor = true;
    }
}

void gl_useProgram(Ctx& x, uint32_t prog) { if (auto* c = currentCtx(x)) c->currentProgram = prog; }
void gl_validateProgram(Ctx&, uint32_t) {}
void gl_getProgramiv(Ctx& x, uint32_t prog, uint32_t pname, GuestPtr params) {
    auto* sg = currentSg(x, currentCtx(x));
    if (!sg || !params || !sg->programs.count(prog)) return;
    const ProgramObject& p = sg->programs[prog];
    int32_t v = 0;
    if (pname == GL_LINK_STATUS || pname == GL_VALIDATE_STATUS) v = p.linked ? 1 : 0;
    else if (pname == GL_INFO_LOG_LENGTH) v = int32_t(p.infoLog.size());
    else if (pname == GL_ATTACHED_SHADERS) v = int32_t(p.shaders.size());
    else if (pname == GL_ACTIVE_ATTRIBUTES) v = int32_t(p.attribs.size());
    else if (pname == GL_ACTIVE_ATTRIBUTE_MAX_LENGTH) v = 64;
    else if (pname == GL_ACTIVE_UNIFORMS) v = int32_t(p.uniforms.size());
    else if (pname == GL_ACTIVE_UNIFORM_MAX_LENGTH) v = 64;
    x.mem.write<int32_t>(params, v);
}
void gl_getProgramInfoLog(Ctx& x, uint32_t, int32_t maxLen, GuestPtr length, GuestPtr infoLog) {
    if (length) x.mem.write<int32_t>(length, 0);
    if (infoLog && maxLen > 0) x.mem.write<uint8_t>(infoLog, 0);
}

int32_t gl_getAttribLocation(Ctx& x, uint32_t prog, GuestPtr name) {
    auto* sg = currentSg(x, currentCtx(x));
    if (!sg || !sg->programs.count(prog) || !name) return -1;
    ProgramObject& p = sg->programs[prog];
    const std::string nm = x.proc.readCString(name);
    for (const auto& a : p.attribs) if (a.name == nm) return a.location;
    auto bit = p.boundAttribLocations.find(nm);
    if (bit != p.boundAttribLocations.end()) return bit->second;
    // Dynamically allocate if queried cleanly
    const int loc = int(p.attribs.size()) < 16 ? int(p.attribs.size()) : 0;
    p.attribs.push_back({nm, GL_FLOAT_VEC4, 1, loc});
    return loc;
}

int32_t gl_getUniformLocation(Ctx& x, uint32_t prog, GuestPtr name) {
    auto* sg = currentSg(x, currentCtx(x));
    if (!sg || !sg->programs.count(prog) || !name) return -1;
    ProgramObject& p = sg->programs[prog];
    std::string nm = x.proc.readCString(name);
    if (auto br = nm.find('['); br != std::string::npos) nm.erase(br);
    for (const auto& u : p.uniforms) if (u.name == nm) return u.location;
    const int loc = int(p.uniforms.size());
    p.uniforms.push_back({nm, GL_FLOAT_VEC4, 1, loc});
    return loc;
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
    writeActiveVar(x, sg->programs[prog].uniforms[index], bufSize, length, size, type, name);
}

void gl_enableVertexAttribArray(Ctx& x, uint32_t idx) { if (auto* c = currentCtx(x); c && idx < 16) c->attribPtr[idx].enabled = true; }
void gl_disableVertexAttribArray(Ctx& x, uint32_t idx) { if (auto* c = currentCtx(x); c && idx < 16) c->attribPtr[idx].enabled = false; }
void gl_vertexAttribPointer(Ctx& x, uint32_t idx, int32_t size, uint32_t type, uint32_t normalized, int32_t stride, GuestPtr ptr) {
    if (auto* c = currentCtx(x); c && idx < 16) {
        c->attribPtr[idx] = {c->attribPtr[idx].enabled, size, type, (normalized & 0xFF) != 0, stride, ptr, c->arrayBuffer};
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

UniformValue* progUniform(Ctx& x, int32_t loc, ProgramObject** outProg = nullptr) {
    ContextData* c = currentCtx(x);
    SharegroupData* sg = currentSg(x, c);
    if (!c || !sg || !c->currentProgram || loc < 0 || !sg->programs.count(c->currentProgram)) return nullptr;
    ProgramObject& p = sg->programs[c->currentProgram];
    if (outProg) *outProg = &p;
    UniformValue& u = p.uniformValues[loc];
    u.isSet = true;
    return &u;
}

void gl_uniform1i(Ctx& x, int32_t loc, int32_t v0) { if (auto* u = progUniform(x, loc)) u->i[0] = v0; }
void gl_uniform2i(Ctx& x, int32_t loc, int32_t v0, int32_t v1) { if (auto* u = progUniform(x, loc)) { u->i[0] = v0; u->i[1] = v1; } }
void gl_uniform3i(Ctx& x, int32_t loc, int32_t v0, int32_t v1, int32_t v2) { if (auto* u = progUniform(x, loc)) { u->i[0] = v0; u->i[1] = v1; u->i[2] = v2; } }
void gl_uniform4i(Ctx& x, int32_t loc, int32_t v0, int32_t v1, int32_t v2, int32_t v3) {
    if (auto* u = progUniform(x, loc)) { u->i[0] = v0; u->i[1] = v1; u->i[2] = v2; u->i[3] = v3; }
}
void gl_uniform1iv(Ctx& x, int32_t loc, int32_t count, GuestPtr v) {
    if (count > 0 && v) gl_uniform1i(x, loc, x.mem.read<int32_t>(v));
}
void gl_uniform1f(Ctx& x, int32_t loc, float v0) { if (auto* u = progUniform(x, loc)) u->f[0] = v0; }
void gl_uniform2f(Ctx& x, int32_t loc, float v0, float v1) { if (auto* u = progUniform(x, loc)) { u->f[0] = v0; u->f[1] = v1; } }
void gl_uniform3f(Ctx& x, int32_t loc, float v0, float v1, float v2) {
    ProgramObject* p = nullptr;
    if (auto* u = progUniform(x, loc, &p)) {
        u->f[0] = v0; u->f[1] = v1; u->f[2] = v2; u->f[3] = 1.0f;
        if (p && p->fragColorUniform < 0) p->fragColorUniform = loc;
    }
}
void gl_uniform4f(Ctx& x, int32_t loc, float v0, float v1, float v2, float v3) {
    ProgramObject* p = nullptr;
    if (auto* u = progUniform(x, loc, &p)) {
        u->f[0] = v0; u->f[1] = v1; u->f[2] = v2; u->f[3] = v3;
        if (p && p->fragColorUniform < 0) p->fragColorUniform = loc;
    }
}
void gl_uniform1fv(Ctx& x, int32_t loc, int32_t c, GuestPtr v) { if (c > 0 && v) gl_uniform1f(x, loc, x.mem.read<float>(v)); }
void gl_uniform2fv(Ctx& x, int32_t loc, int32_t c, GuestPtr v) { if (c > 0 && v) gl_uniform2f(x, loc, x.mem.read<float>(v), x.mem.read<float>(v + 4)); }
void gl_uniform3fv(Ctx& x, int32_t loc, int32_t c, GuestPtr v) { if (c > 0 && v) gl_uniform3f(x, loc, x.mem.read<float>(v), x.mem.read<float>(v + 4), x.mem.read<float>(v + 8)); }
void gl_uniform4fv(Ctx& x, int32_t loc, int32_t c, GuestPtr v) {
    if (c > 0 && v) gl_uniform4f(x, loc, x.mem.read<float>(v), x.mem.read<float>(v + 4), x.mem.read<float>(v + 8), x.mem.read<float>(v + 12));
}
void gl_uniformMatrix4fv(Ctx& x, int32_t loc, int32_t count, uint32_t transpose, GuestPtr value) {
    if (count <= 0 || !value) return;
    ProgramObject* p = nullptr;
    if (auto* u = progUniform(x, loc, &p)) {
        x.mem.copyOut(u->f, value, 64);
        if (transpose & 0xFF) {
            for (int r = 0; r < 4; ++r)
                for (int c = r + 1; c < 4; ++c)
                    std::swap(u->f[c * 4 + r], u->f[r * 4 + c]);
        }
        if (p && p->mvpUniform < 0) p->mvpUniform = loc;
    }
}
void gl_uniformMatrix3fv(Ctx& x, int32_t loc, int32_t count, uint32_t, GuestPtr value) {
    if (count <= 0 || !value) return;
    if (auto* u = progUniform(x, loc)) x.mem.copyOut(u->f, value, 36);
}
void gl_uniformMatrix2fv(Ctx& x, int32_t loc, int32_t count, uint32_t, GuestPtr value) {
    if (count <= 0 || !value) return;
    if (auto* u = progUniform(x, loc)) x.mem.copyOut(u->f, value, 16);
}

// =================================================== Software Rasterizer ===

struct ShadedVertex {
    Vec4 clip;
    float ndcX = 0, ndcY = 0, ndcZ = 0, invW = 1;
    float screenX = 0, screenY = 0, screenZ = 0;
    Vec4 color{1, 1, 1, 1};
    float u = 0, v = 0;
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

ShadedVertex shadeVertex(Ctx& x, ContextData& c, SharegroupData& sg, int idx) {
    ShadedVertex sv;
    if (c.api == 1 || c.currentProgram == 0) {
        const Vec4 pos = fetchAttribute(x, sg, c.vertexPtr, idx, {0, 0, 0, 1});
        sv.color = fetchAttribute(x, sg, c.colorPtr, idx, c.currentColor);
        const Vec4 tc = fetchAttribute(x, sg, c.texCoordPtr[0], idx, c.currentTexCoord[0]);
        const Vec4 tcXformed = c.textureStack.back().apply(tc);
        sv.u = tcXformed.x;
        sv.v = tcXformed.y;
        const Mat4 mvp = c.projectionStack.back() * c.modelviewStack.back();
        sv.clip = mvp.apply(pos);
    } else {
        const ProgramObject& p = sg.programs[c.currentProgram];
        const int posLoc = (p.posAttrib >= 0 && p.posAttrib < 16) ? p.posAttrib : 0;
        const Vec4 pos = fetchAttribute(x, sg, c.attribPtr[posLoc], idx, c.genericAttrib[posLoc]);
        if (p.mvpUniform >= 0 && p.uniformValues.count(p.mvpUniform) && p.uniformValues.at(p.mvpUniform).isSet) {
            Mat4 mvp;
            std::memcpy(mvp.m, p.uniformValues.at(p.mvpUniform).f, 64);
            sv.clip = mvp.apply(pos);
        } else {
            sv.clip = pos;
        }
        if (p.colorAttrib >= 0 && p.colorAttrib < 16) {
            sv.color = fetchAttribute(x, sg, c.attribPtr[p.colorAttrib], idx, c.genericAttrib[p.colorAttrib]);
        } else if (p.fragColorUniform >= 0 && p.uniformValues.count(p.fragColorUniform) && p.uniformValues.at(p.fragColorUniform).isSet) {
            const float* f = p.uniformValues.at(p.fragColorUniform).f;
            sv.color = {f[0], f[1], f[2], f[3]};
        } else if (p.hasConstFragColor) {
            sv.color = p.constFragColor;
        } else {
            sv.color = {1, 1, 1, 1};
        }
        if (p.texCoordAttrib >= 0 && p.texCoordAttrib < 16) {
            const Vec4 tc = fetchAttribute(x, sg, c.attribPtr[p.texCoordAttrib], idx, c.genericAttrib[p.texCoordAttrib]);
            sv.u = tc.x;
            sv.v = tc.y;
        }
    }
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

float blendFactorVal(uint32_t factor, float srcC, float srcA, float dstC, float dstA) {
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
        default: return 1.0f;
    }
}

void writeFragment(ContextData& c, RenderTarget& rt, int px, int py, float z, Vec4 frag) {
    if (c.alphaTest && !depthPass(c.alphaFunc, frag.w, c.alphaRef)) return;
    const size_t idx = size_t(py) * size_t(rt.width) + size_t(px);
    if (c.depthTest && rt.depth) {
        if (!depthPass(c.depthFunc, z, rt.depth[idx])) return;
        if (c.depthMask) rt.depth[idx] = z;
    }
    uint8_t* dst = &rt.rgba[idx * 4];
    float outR = frag.x, outG = frag.y, outB = frag.z, outA = frag.w;
    if (c.blend) {
        const float dstR = dst[0] / 255.0f, dstG = dst[1] / 255.0f, dstB = dst[2] / 255.0f, dstA = dst[3] / 255.0f;
        const float srR = blendFactorVal(c.blendSrcRGB, outR, outA, dstR, dstA);
        const float srG = blendFactorVal(c.blendSrcRGB, outG, outA, dstG, dstA);
        const float srB = blendFactorVal(c.blendSrcRGB, outB, outA, dstB, dstA);
        const float drR = blendFactorVal(c.blendDstRGB, outR, outA, dstR, dstA);
        const float drG = blendFactorVal(c.blendDstRGB, outG, outA, dstG, dstA);
        const float drB = blendFactorVal(c.blendDstRGB, outB, outA, dstB, dstA);
        const float sa  = blendFactorVal(c.blendSrcAlpha, outA, outA, dstA, dstA);
        const float da  = blendFactorVal(c.blendDstAlpha, outA, outA, dstA, dstA);
        outR = outR * srR + dstR * drR;
        outG = outG * srG + dstG * drG;
        outB = outB * srB + dstB * drB;
        outA = outA * sa + dstA * da;
    }
    if (c.colorMask[0]) dst[0] = uint8_t(std::clamp(outR, 0.0f, 1.0f) * 255.0f + 0.5f);
    if (c.colorMask[1]) dst[1] = uint8_t(std::clamp(outG, 0.0f, 1.0f) * 255.0f + 0.5f);
    if (c.colorMask[2]) dst[2] = uint8_t(std::clamp(outB, 0.0f, 1.0f) * 255.0f + 0.5f);
    if (c.colorMask[3]) dst[3] = uint8_t(std::clamp(outA, 0.0f, 1.0f) * 255.0f + 0.5f);
}

const Texture2D* boundTextureForDraw(ContextData& c, SharegroupData& sg) {
    if (c.api == 1 || c.currentProgram == 0) {
        if (!c.texture2DEnabled[0]) return nullptr;
        const uint32_t tid = c.boundTextures[0];
        return (tid && sg.textures.count(tid)) ? &sg.textures.at(tid) : nullptr;
    }
    const ProgramObject& p = sg.programs[c.currentProgram];
    if (!p.usesTexture && p.texCoordAttrib < 0) return nullptr;
    int unit = 0;
    if (p.samplerUniform >= 0 && p.uniformValues.count(p.samplerUniform))
        unit = p.uniformValues.at(p.samplerUniform).i[0] & 7;
    const uint32_t tid = c.boundTextures[unit];
    return (tid && sg.textures.count(tid)) ? &sg.textures.at(tid) : nullptr;
}

void rasterizeTriangle(ContextData& c, SharegroupData& sg, RenderTarget& rt,
                       const ShadedVertex& v0, const ShadedVertex& v1, const ShadedVertex& v2) {
    const float area = (v1.screenX - v0.screenX) * (v2.screenY - v0.screenY) -
                       (v1.screenY - v0.screenY) * (v2.screenX - v0.screenX);
    if (std::abs(area) < 1e-4f) return;
    if (c.cullFace) {
        if (c.cullFaceMode == GL_FRONT_AND_BACK) return;
        const bool ccw = area > 0;
        const bool isFront = (c.frontFace == GL_CCW) ? ccw : !ccw;
        if ((c.cullFaceMode == GL_FRONT && isFront) || (c.cullFaceMode == GL_BACK && !isFront)) return;
    }

    int clipX0 = 0, clipY0 = 0, clipX1 = rt.width, clipY1 = rt.height;
    if (c.scissorTest) {
        clipX0 = std::clamp(c.scissor[0], 0, rt.width);
        clipY0 = std::clamp(c.scissor[1], 0, rt.height);
        clipX1 = std::clamp(c.scissor[0] + c.scissor[2], 0, rt.width);
        clipY1 = std::clamp(c.scissor[1] + c.scissor[3], 0, rt.height);
    }

    const int minX = std::max(clipX0, int(std::floor(std::min({v0.screenX, v1.screenX, v2.screenX}))));
    const int maxX = std::min(clipX1 - 1, int(std::ceil(std::max({v0.screenX, v1.screenX, v2.screenX}))));
    const int minY = std::max(clipY0, int(std::floor(std::min({v0.screenY, v1.screenY, v2.screenY}))));
    const int maxY = std::min(clipY1 - 1, int(std::ceil(std::max({v0.screenY, v1.screenY, v2.screenY}))));
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

    for (int py = minY; py <= maxY; ++py) {
        const float fy = float(py) + 0.5f;
        for (int px = minX; px <= maxX; ++px) {
            const float fx = float(px) + 0.5f;
            const float w0 = ((v2.screenX - v1.screenX) * (fy - v1.screenY) - (v2.screenY - v1.screenY) * (fx - v1.screenX)) * invArea;
            const float w1 = ((v0.screenX - v2.screenX) * (fy - v2.screenY) - (v0.screenY - v2.screenY) * (fx - v2.screenX)) * invArea;
            const float w2 = 1.0f - w0 - w1;
            if (w0 < -1e-4f || w1 < -1e-4f || w2 < -1e-4f) continue;

            const float z = w0 * v0.screenZ + w1 * v1.screenZ + w2 * v2.screenZ;
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
                if (c.api == 1 && c.texEnvMode[0] == GL_REPLACE) col = tc;
                else col = {col.x * tc.x, col.y * tc.y, col.z * tc.z, col.w * tc.w};
            }
            writeFragment(c, rt, px, py, z, col);
        }
    }
}

void rasterizeLine(ContextData& c, SharegroupData& sg, RenderTarget& rt,
                   const ShadedVertex& v0, const ShadedVertex& v1) {
    const float dx = v1.screenX - v0.screenX, dy = v1.screenY - v0.screenY;
    const int steps = std::max(1, int(std::ceil(std::max(std::abs(dx), std::abs(dy)))));
    const Texture2D* tex = boundTextureForDraw(c, sg);
    for (int i = 0; i <= steps; ++i) {
        const float t = float(i) / float(steps);
        const int px = int(v0.screenX + dx * t);
        const int py = int(v0.screenY + dy * t);
        if (px < 0 || px >= rt.width || py < 0 || py >= rt.height) continue;
        const float z = v0.screenZ + (v1.screenZ - v0.screenZ) * t;
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
    RenderTarget rt = activeTarget(x, *c, *sg);
    if (!rt.rgba || rt.width <= 0 || rt.height <= 0) return;

    std::vector<ShadedVertex> verts;
    verts.reserve(indices.size());
    for (int idx : indices) verts.push_back(shadeVertex(x, *c, *sg, idx));
    // Debugging aid: HVM_TRACE_DRAW=1 prints each draw call (mode, vertex count, texture, blend).
    if (static const bool trace = std::getenv("HVM_TRACE_DRAW") != nullptr; trace) {
        const Texture2D* t = boundTextureForDraw(*c, *sg);
        std::fprintf(stderr, "draw mode %u n %zu tex %dx%d blend %d %x/%x first (%.2f,%.2f uv %.4f,%.4f) col %.2f,%.2f,%.2f,%.2f\n",
                     mode, verts.size(), t ? t->width : 0, t ? t->height : 0, c->blend, c->blendSrcRGB, c->blendDstRGB,
                     verts[0].screenX, verts[0].screenY, verts[0].u, verts[0].v, verts[0].color.x, verts[0].color.y, verts[0].color.z, verts[0].color.w);
    }

    switch (mode) {
        case GL_TRIANGLES:
            for (size_t i = 0; i + 2 < verts.size(); i += 3)
                rasterizeTriangle(*c, *sg, rt, verts[i], verts[i + 1], verts[i + 2]);
            break;
        case GL_TRIANGLE_STRIP:
            for (size_t i = 0; i + 2 < verts.size(); ++i) {
                if (i & 1) rasterizeTriangle(*c, *sg, rt, verts[i + 1], verts[i], verts[i + 2]);
                else rasterizeTriangle(*c, *sg, rt, verts[i], verts[i + 1], verts[i + 2]);
            }
            break;
        case GL_TRIANGLE_FAN:
            for (size_t i = 1; i + 1 < verts.size(); ++i)
                rasterizeTriangle(*c, *sg, rt, verts[0], verts[i], verts[i + 1]);
            break;
        case GL_LINES:
            for (size_t i = 0; i + 1 < verts.size(); i += 2)
                rasterizeLine(*c, *sg, rt, verts[i], verts[i + 1]);
            break;
        case GL_LINE_STRIP:
            for (size_t i = 0; i + 1 < verts.size(); ++i)
                rasterizeLine(*c, *sg, rt, verts[i], verts[i + 1]);
            break;
        case GL_LINE_LOOP:
            for (size_t i = 0; i < verts.size(); ++i)
                rasterizeLine(*c, *sg, rt, verts[i], verts[(i + 1) % verts.size()]);
            break;
        case GL_POINTS: {
            const int r = std::max(0, int(c->pointSize / 2.0f));
            for (const auto& v : verts) {
                const int cx = int(v.screenX), cy = int(v.screenY);
                for (int dy = -r; dy <= r; ++dy) {
                    for (int dx = -r; dx <= r; ++dx) {
                        const int px = cx + dx, py = cy + dy;
                        if (px >= 0 && px < rt.width && py >= 0 && py < rt.height)
                            writeFragment(*c, rt, px, py, v.screenZ, v.color);
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
    std::vector<int> indices(static_cast<size_t>(count));
    for (int32_t i = 0; i < count; ++i) indices[size_t(i)] = first + i;
    drawPrimitives(x, mode, indices);
}

void gl_drawElements(Ctx& x, uint32_t mode, int32_t count, uint32_t type, GuestPtr indicesPtr) {
    ContextData* c = currentCtx(x);
    SharegroupData* sg = currentSg(x, c);
    if (!c || !sg || count <= 0) return;
    const int elemBytes = (type == GL_UNSIGNED_BYTE) ? 1 : (type == GL_UNSIGNED_SHORT) ? 2 : 4;
    const uint8_t* src = nullptr;
    if (c->elementArrayBuffer != 0) {
        auto bit = sg->buffers.find(c->elementArrayBuffer);
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

// GL_OES_draw_texture: draws the bound texture directly into screen coordinates (x, y, z, w, h).
void gl_drawTexfOES(Ctx& x, float sx, float sy, float sz, float sw, float sh) {
    ContextData* c = currentCtx(x);
    SharegroupData* sg = currentSg(x, c);
    if (!c || !sg || sw <= 0 || sh <= 0) return;
    RenderTarget rt = activeTarget(x, *c, *sg);
    if (!rt.rgba) return;
    const Texture2D* tex = boundTextureForDraw(*c, *sg);
    ShadedVertex v0, v1, v2, v3;
    v0.screenX = sx;      v0.screenY = sy;      v0.screenZ = sz; v0.color = c->currentColor; v0.u = 0; v0.v = 0;
    v1.screenX = sx + sw; v1.screenY = sy;      v1.screenZ = sz; v1.color = c->currentColor; v1.u = 1; v1.v = 0;
    v2.screenX = sx + sw; v2.screenY = sy + sh; v2.screenZ = sz; v2.color = c->currentColor; v2.u = 1; v2.v = 1;
    v3.screenX = sx;      v3.screenY = sy + sh; v3.screenZ = sz; v3.color = c->currentColor; v3.u = 0; v3.v = 1;
    (void)tex;
    rasterizeTriangle(*c, *sg, rt, v0, v1, v2);
    rasterizeTriangle(*c, *sg, rt, v0, v2, v3);
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
    p.def<&gl_noop3>("_glStencilFunc");
    p.def<&gl_noop3>("_glStencilOp");
    p.def<&gl_noop1>("_glStencilMask");
    p.def<&gl_noop4>("_glStencilFuncSeparate");
    p.def<&gl_noop4>("_glStencilOpSeparate");
    p.def<&gl_noop2>("_glStencilMaskSeparate");
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
    p.def<&gl_noop3>("_glDiscardFramebufferEXT");
    p.def<&gl_noop>("_glResolveMultisampleFramebufferAPPLE");

    // Buffers & VAOs
    p.def<&gl_genBuffers>("_glGenBuffers");
    p.def<&gl_deleteBuffers>("_glDeleteBuffers");
    p.def<&gl_bindBuffer>("_glBindBuffer");
    p.def<&gl_isBuffer>("_glIsBuffer");
    p.def<&gl_bufferData>("_glBufferData");
    p.def<&gl_bufferSubData>("_glBufferSubData");
    p.def<&gl_getBufferParameteriv>("_glGetBufferParameteriv");
    p.def<&gl_mapBufferOES>("_glMapBufferOES");
    p.def<&gl_unmapBufferOES>("_glUnmapBufferOES");
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
}

} // namespace Hydrogen::Runtime::UIKit
