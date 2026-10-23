// HydrogenVM — OpenGL ES 1.1 fixed function on an ES 2/3 host context.
//
// Used when the host driver has no ES 1.1 (newer iOS) or with HVM_GL1_EMULATE=1.
// installGles1Emulation() replaces the ES 1.1 entry points of a HostContext's
// function table with the functions below (same host signatures), and wraps the
// shared entry points whose behaviour differs (glEnable for fixed-function caps,
// draws, glGet*, texture uploads). State lives in Gles1Emu; each draw picks a
// shader program generated for the current fixed-function state (cached by a
// key), uploads the matrices, lights, fog and texture-environment uniforms, and
// calls the real draw.
//
// Client arrays map to fixed attribute locations; glVertexPointer & co. call
// glVertexAttribPointer right away, so the buffer binding at that moment is
// captured exactly as ES 1.1 does.
#include "runtime/gl/HostGLInternal.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace Hydrogen::Runtime::HostGL {

namespace {

// ---------------------------------------------------------------- enums ---
constexpr uint32_t GL_FALSE_ = 0;
constexpr uint32_t kModelview = 0x1700, kProjection = 0x1701, kTexture = 0x1702;
constexpr uint32_t kTexture0 = 0x84C0;
constexpr uint32_t kVertexArray = 0x8074, kNormalArray = 0x8075, kColorArray = 0x8076, kTexCoordArray = 0x8078,
                   kPointSizeArrayOES = 0x8B9C;
constexpr uint32_t kCapTexture2D = 0x0DE1, kLighting = 0x0B50, kLight0 = 0x4000, kFog = 0x0B60, kAlphaTest = 0x0BC0,
                   kNormalize = 0x0BA1, kRescaleNormal = 0x803A, kColorMaterial = 0x0B57, kPointSprite = 0x8861,
                   kClipPlane0 = 0x3000, kPointSmooth = 0x0B10, kLineSmooth = 0x0B20, kMultisample = 0x809D,
                   kSampleAlphaToOne = 0x809F, kColorLogicOp = 0x0BF2;
constexpr uint32_t kFloat = 0x1406, kUnsignedByte = 0x1401, kByte = 0x1400, kShort = 0x1402, kFixed = 0x140C;
constexpr uint32_t kPoints = 0x0000;
// texture env
constexpr uint32_t kTextureEnv = 0x2300, kTextureEnvMode = 0x2200, kTextureEnvColor = 0x2201;
constexpr uint32_t kModulate = 0x2100, kDecal = 0x2101, kBlend = 0x0BE2, kReplace = 0x1E01, kAdd = 0x0104, kCombine = 0x8570;
constexpr uint32_t kCombineRgb = 0x8571, kCombineAlpha = 0x8572, kRgbScale = 0x8573, kAlphaScale = 0x0D1C;
constexpr uint32_t kAddSigned = 0x8574, kInterpolate = 0x8575, kSubtract = 0x84E7, kDot3Rgb = 0x86AE, kDot3Rgba = 0x86AF;
constexpr uint32_t kSrc0Rgb = 0x8580, kSrc0Alpha = 0x8588, kOperand0Rgb = 0x8590, kOperand0Alpha = 0x8598;
constexpr uint32_t kConstant = 0x8576, kPrimaryColor = 0x8577, kPrevious = 0x8578, kTexSrc = 0x1702;
constexpr uint32_t kSrcColor = 0x0300, kOneMinusSrcColor = 0x0301, kSrcAlpha = 0x0302, kOneMinusSrcAlpha = 0x0303;
constexpr uint32_t kPointSpriteTarget = 0x8861, kCoordReplace = 0x8862;
// texture formats
constexpr uint32_t kAlpha = 0x1906, kRgb = 0x1907, kRgba = 0x1908, kLuminance = 0x1909, kLuminanceAlpha = 0x190A, kBgra = 0x80E1;
constexpr uint32_t kGenerateMipmap = 0x8191, kTextureCropRect = 0x8B9D, kTextureBinding2D = 0x8069;
// lights / material / fog
constexpr uint32_t kAmbient = 0x1200, kDiffuse = 0x1201, kSpecular = 0x1202, kPosition = 0x1203, kSpotDirection = 0x1204,
                   kSpotExponent = 0x1205, kSpotCutoff = 0x1206, kConstantAtt = 0x1207, kLinearAtt = 0x1208, kQuadraticAtt = 0x1209;
constexpr uint32_t kEmission = 0x1600, kShininess = 0x1601, kAmbientAndDiffuse = 0x1602;
constexpr uint32_t kLightModelAmbient = 0x0B53, kLightModelTwoSide = 0x0B52;
constexpr uint32_t kFogMode = 0x0B65, kFogDensity = 0x0B62, kFogStart = 0x0B63, kFogEnd = 0x0B64, kFogColor = 0x0B66;
constexpr uint32_t kExp = 0x0800, kExp2 = 0x0801, kLinear = 0x2601;
constexpr uint32_t kPointSizeMin = 0x8126, kPointSizeMax = 0x8127, kPointFadeThreshold = 0x8128, kPointDistanceAttenuation = 0x8129;
// alpha test funcs
constexpr uint32_t kNever = 0x0200, kAlways = 0x0207;
// queries
constexpr uint32_t kMatrixMode = 0x0BA0, kModelviewMatrix = 0x0BA6, kProjectionMatrix = 0x0BA7, kTextureMatrix = 0x0BA8;
constexpr uint32_t kMaxTextureUnits = 0x84E2, kMaxLights = 0x0D31, kMaxClipPlanes = 0x0D32, kMaxModelviewStackDepth = 0x0D36,
                   kMaxProjectionStackDepth = 0x0D38, kMaxTextureStackDepth = 0x0D39, kModelviewStackDepth = 0x0BA3,
                   kProjectionStackDepth = 0x0BA4, kTextureStackDepth = 0x0BA5;
constexpr uint32_t kCurrentColor = 0x0B00, kCurrentNormal = 0x0B02, kCurrentTextureCoords = 0x0B03, kClientActiveTexture = 0x84E1,
                   kActiveTexture = 0x84E0, kAlphaTestFunc = 0x0BC1, kAlphaTestRef = 0x0BC2, kShadeModel = 0x0B54,
                   kPointSize = 0x0B11, kSmoothPointSizeRange = 0x0B12, kAliasedPointSizeRange = 0x846D;
constexpr uint32_t kVertexArrayPointer = 0x808E, kNormalArrayPointer = 0x808F, kColorArrayPointer = 0x8090, kTexCoordArrayPointer = 0x8092,
                   kPointSizeArrayPointer = 0x898C;
constexpr uint32_t kVertexArraySize = 0x807A, kVertexArrayType = 0x807B, kVertexArrayStride = 0x807C, kColorArraySize = 0x8081,
                   kTexCoordArraySize = 0x8088;
// hints that ES 2 rejects
constexpr uint32_t kPerspectiveCorrectionHint = 0x0C50, kPointSmoothHint = 0x0C51, kLineSmoothHint = 0x0C52, kFogHint = 0x0C54;
// misc
constexpr uint32_t kArrayBuffer = 0x8892, kArrayBufferBinding = 0x8894, kElementArrayBufferBinding = 0x8895;
constexpr uint32_t kTriangleStrip = 0x0005, kVertexShader = 0x8B31, kFragmentShader = 0x8B30;
constexpr uint32_t kCompileStatus = 0x8B81, kLinkStatus = 0x8B82, kViewport = 0x0BA2, kDepthTest = 0x0B71, kCullFace = 0x0B44,
                   kCurrentProgram = 0x8B8D;
constexpr uint32_t kFramebuffer = 0x8D40, kRenderbuffer = 0x8D41;

// Attribute locations (fixed, bound before linking).
enum Loc : uint32_t { LPos = 0, LColor = 1, LNormal = 2, LTex0 = 3, LTex1 = 4, LPointSize = 5 };

constexpr int kUnits = 2, kLights = 8, kPlanes = 6;

// ---------------------------------------------------------------- math ---
struct Mat4 {
    float m[16];   // column-major
    static Mat4 identity() { Mat4 r{}; r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1; return r; }
    Mat4 operator*(const Mat4& b) const {
        Mat4 r{};
        for (int c = 0; c < 4; ++c)
            for (int rr = 0; rr < 4; ++rr) {
                float s = 0;
                for (int k = 0; k < 4; ++k) s += m[k * 4 + rr] * b.m[c * 4 + k];
                r.m[c * 4 + rr] = s;
            }
        return r;
    }
    void apply(const float v[4], float out[4]) const {
        for (int r = 0; r < 4; ++r) out[r] = m[r] * v[0] + m[4 + r] * v[1] + m[8 + r] * v[2] + m[12 + r] * v[3];
    }
    bool isIdentity() const { const Mat4 i = identity(); return std::memcmp(m, i.m, sizeof m) == 0; }
};
Mat4 fromArray(const float* a) { Mat4 r; std::memcpy(r.m, a, sizeof r.m); return r; }
Mat4 ortho(float l, float r, float b, float t, float n, float f) {
    Mat4 o = Mat4::identity();
    o.m[0] = 2 / (r - l); o.m[5] = 2 / (t - b); o.m[10] = -2 / (f - n);
    o.m[12] = -(r + l) / (r - l); o.m[13] = -(t + b) / (t - b); o.m[14] = -(f + n) / (f - n);
    return o;
}
Mat4 frustum(float l, float r, float b, float t, float n, float f) {
    Mat4 o{};
    o.m[0] = 2 * n / (r - l); o.m[5] = 2 * n / (t - b);
    o.m[8] = (r + l) / (r - l); o.m[9] = (t + b) / (t - b); o.m[10] = -(f + n) / (f - n); o.m[11] = -1;
    o.m[14] = -2 * f * n / (f - n);
    return o;
}
Mat4 translate(float x, float y, float z) { Mat4 o = Mat4::identity(); o.m[12] = x; o.m[13] = y; o.m[14] = z; return o; }
Mat4 scale(float x, float y, float z) { Mat4 o = Mat4::identity(); o.m[0] = x; o.m[5] = y; o.m[10] = z; return o; }
Mat4 rotate(float deg, float x, float y, float z) {
    const float len = std::sqrt(x * x + y * y + z * z);
    if (len == 0) return Mat4::identity();
    x /= len; y /= len; z /= len;
    const float a = deg * float(M_PI) / 180.0f, c = std::cos(a), s = std::sin(a), t = 1 - c;
    Mat4 o = Mat4::identity();
    o.m[0] = x * x * t + c;     o.m[4] = x * y * t - z * s; o.m[8] = x * z * t + y * s;
    o.m[1] = y * x * t + z * s; o.m[5] = y * y * t + c;     o.m[9] = y * z * t - x * s;
    o.m[2] = x * z * t - y * s; o.m[6] = y * z * t + x * s; o.m[10] = z * z * t + c;
    return o;
}
// Inverse-transpose of the upper 3x3 (normal matrix), column-major 3x3.
void normalMatrix(const Mat4& mv, float out[9]) {
    const float* m = mv.m;
    const float a = m[0], b = m[4], c = m[8], d = m[1], e = m[5], f = m[9], g = m[2], h = m[6], i = m[10];
    const float A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    const float det = a * A + b * B + c * C;
    const float inv = det != 0 ? 1.0f / det : 0.0f;
    // inverse (row-major) = adj^T / det; transpose of inverse = adj / det (cofactor matrix)
    const float co[9] = {A, B, C,
                         -(b * i - c * h), a * i - c * g, -(a * h - b * g),
                         b * f - c * e, -(a * f - c * d), a * e - b * d};
    // cofactor matrix rows -> column-major mat3 of (M^-1)^T: element (r,c) = co[r*3+c]/det
    for (int r = 0; r < 3; ++r)
        for (int cc = 0; cc < 3; ++cc) out[cc * 3 + r] = co[r * 3 + cc] * inv;
}
bool invert(const Mat4& in, Mat4& out) {
    const float* m = in.m;
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    const float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (det == 0) return false;
    for (int i = 0; i < 16; ++i) out.m[i] = inv[i] / det;
    return true;
}

// ---------------------------------------------------------------- state ---
struct Light {
    float ambient[4] = {0, 0, 0, 1}, diffuse[4] = {0, 0, 0, 1}, specular[4] = {0, 0, 0, 1};
    float position[4] = {0, 0, 1, 0};   // eye space
    float spotDir[3] = {0, 0, -1};      // eye space
    float spotExp = 0, spotCutoff = 180;
    float att[3] = {1, 0, 0};
};
struct Env {
    uint32_t mode = kModulate, combineRgb = kModulate, combineA = kModulate;
    uint32_t srcRgb[3] = {kTexSrc, kPrevious, kConstant}, srcA[3] = {kTexSrc, kPrevious, kConstant};
    uint32_t opRgb[3] = {kSrcColor, kSrcColor, kSrcAlpha}, opA[3] = {kSrcAlpha, kSrcAlpha, kSrcAlpha};
    float rgbScale = 1, alphaScale = 1;
    float color[4] = {0, 0, 0, 0};
    bool coordReplace = false;
};
struct ProgramInfo {
    uint32_t id = 0;
    int uMvp = -1, uMv = -1, uNormal = -1, uTexMat[kUnits] = {-1, -1}, uSampler[kUnits] = {-1, -1};
    int uMatAmbient = -1, uMatDiffuse = -1, uMatSpecular = -1, uMatEmission = -1, uMatShininess = -1, uModelAmbient = -1;
    int uLAmb = -1, uLDif = -1, uLSpec = -1, uLPos = -1, uLSpotDir = -1, uLSpot = -1, uLAtt = -1;
    int uFog = -1, uFogColor = -1, uAlphaRef = -1, uEnvColor[kUnits] = {-1, -1}, uEnvScale[kUnits] = {-1, -1};
    int uPoint = -1, uPointAtt = -1, uPlanes = -1;
};

} // namespace

struct Gles1Emu {
    Fns real;   // the driver's functions, before the overrides
    uint32_t matrixMode = kModelview;
    std::vector<Mat4> mv{Mat4::identity()}, proj{Mat4::identity()}, tex[kUnits] = {{Mat4::identity()}, {Mat4::identity()}};
    uint32_t activeUnit = 0, clientUnit = 0;
    float color[4] = {1, 1, 1, 1}, normal[3] = {0, 0, 1}, texcoord[kUnits][4] = {{0, 0, 0, 1}, {0, 0, 0, 1}};
    bool arrayOn[6] = {};   // by Loc
    // pointer queries (glGetPointerv): host pointer or buffer offset as given
    const void* arrayPtr[6] = {};
    int arraySize[6] = {4, 4, 3, 4, 4, 1};
    uint32_t arrayType[6] = {kFloat, kFloat, kFloat, kFloat, kFloat, kFloat};
    int arrayStride[6] = {};
    bool tex2d[kUnits] = {}, lighting = false, lightOn[kLights] = {}, fog = false, alphaTest = false,
         normalize = false, rescale = false, colorMaterial = false, pointSprite = false, clipOn[kPlanes] = {};
    bool pointSmooth = false, lineSmooth = false, multisample = true, alphaToOne = false, logicOp = false;
    uint32_t alphaFunc = kAlways;
    float alphaRef = 0;
    Env env[kUnits];
    Light light[kLights];
    float matAmbient[4] = {0.2f, 0.2f, 0.2f, 1}, matDiffuse[4] = {0.8f, 0.8f, 0.8f, 1}, matSpecular[4] = {0, 0, 0, 1},
          matEmission[4] = {0, 0, 0, 1}, matShininess = 0;
    float modelAmbient[4] = {0.2f, 0.2f, 0.2f, 1};
    bool twoSide = false;
    uint32_t fogMode = kExp;
    float fogDensity = 1, fogStart = 0, fogEnd = 1, fogColor[4] = {0, 0, 0, 0};
    float pointSize = 1, pointMin = 0, pointMax = 64, pointFade = 1, pointAtt[3] = {1, 0, 0};
    float planes[kPlanes][4] = {};
    uint32_t shadeModel = 0x1D01;   // SMOOTH
    std::unordered_map<uint32_t, uint32_t> texFormat;            // texture name -> base format
    std::unordered_map<uint32_t, bool> texGenMip;                // GL_GENERATE_MIPMAP per texture
    std::unordered_map<uint32_t, std::array<int32_t, 4>> crop;   // GL_TEXTURE_CROP_RECT_OES
    uint32_t boundTex[kUnits] = {};
    std::map<std::string, ProgramInfo> programs;
    uint32_t drawTexVbo = 0;

    Mat4& top() {
        if (matrixMode == kProjection) return proj.back();
        if (matrixMode == kTexture) return tex[activeUnit].back();
        return mv.back();
    }
    std::vector<Mat4>& stack() {
        if (matrixMode == kProjection) return proj;
        if (matrixMode == kTexture) return tex[activeUnit];
        return mv;
    }
};

namespace {

Gles1Emu& E() { return *currentHostContext()->gles1; }
bool have() { HostContext* hc = currentHostContext(); return hc && hc->gles1; }

// ----------------------------------------------------------- shader gen ---

uint32_t baseFormat(Gles1Emu& e, int unit) {
    auto it = e.texFormat.find(e.boundTex[unit]);
    return it == e.texFormat.end() ? kRgba : it->second;
}

std::string hex(uint32_t v) { char b[16]; std::snprintf(b, sizeof b, "%x", v); return b; }

std::string stateKey(Gles1Emu& e, bool points) {
    std::string k;
    k.reserve(96);
    k += points ? 'P' : 'T';
    k += e.lighting ? 'L' : '-';
    if (e.lighting) {
        for (int i = 0; i < kLights; ++i) k += e.lightOn[i] ? '1' : '0';
        k += e.colorMaterial ? 'C' : '-';
        k += (e.normalize || e.rescale) ? 'N' : '-';
        k += e.twoSide ? '2' : '-';
    }
    k += e.fog ? hex(e.fogMode) : "-";
    k += e.alphaTest ? hex(e.alphaFunc) : "-";
    for (int i = 0; i < kPlanes; ++i) k += e.clipOn[i] ? 'c' : '.';
    if (points) {
        k += e.arrayOn[LPointSize] ? 'S' : '-';
        k += e.pointSprite ? 'R' : '-';
    }
    for (int u = 0; u < kUnits; ++u) {
        k += '|';
        if (!e.tex2d[u]) { k += '0'; continue; }
        const Env& v = e.env[u];
        k += hex(baseFormat(e, u)) + ":" + hex(v.mode);
        if (v.mode == kCombine) {
            k += ":" + hex(v.combineRgb) + ":" + hex(v.combineA);
            for (int i = 0; i < 3; ++i) k += ":" + hex(v.srcRgb[i]) + hex(v.opRgb[i]) + hex(v.srcA[i]) + hex(v.opA[i]);
        }
        if (points && e.pointSprite && v.coordReplace) k += 'R';
    }
    return k;
}

std::string vertexShader(Gles1Emu& e, bool points) {
    std::string s =
        "attribute vec4 aPos; attribute vec4 aColor; attribute vec3 aNormal; attribute vec4 aTex0; attribute vec4 aTex1; attribute float aPointSize;\n"
        "uniform mat4 uMvp; uniform mat4 uMv; uniform mat3 uNormal; uniform mat4 uTexMat0; uniform mat4 uTexMat1;\n"
        "varying vec4 vColor; varying vec4 vTex0; varying vec4 vTex1; varying float vFogDist; varying float vClip[6];\n"
        "uniform vec4 uPlanes[6]; uniform vec3 uPoint; uniform vec3 uPointAtt;\n";
    if (e.lighting) {
        s += "uniform vec4 uMatAmbient, uMatDiffuse, uMatSpecular, uMatEmission; uniform float uMatShininess; uniform vec4 uModelAmbient;\n"
             "uniform vec4 uLAmb[8], uLDif[8], uLSpec[8], uLPos[8]; uniform vec3 uLSpotDir[8], uLAtt[8]; uniform vec2 uLSpot[8];\n";
    }
    s += "void main() {\n"
         "  vec4 eye = uMv * aPos;\n"
         "  gl_Position = uMvp * aPos;\n"
         "  vTex0 = uTexMat0 * aTex0; vTex1 = uTexMat1 * aTex1;\n"
         "  vFogDist = abs(eye.z);\n";
    for (int i = 0; i < kPlanes; ++i)
        if (e.clipOn[i]) s += "  vClip[" + std::to_string(i) + "] = dot(uPlanes[" + std::to_string(i) + "], eye);\n";
    if (e.lighting) {
        s += std::string("  vec3 n = uNormal * aNormal;\n") + ((e.normalize || e.rescale) ? "  n = normalize(n);\n" : "") +
             std::string("  vec4 ma = ") + (e.colorMaterial ? "aColor" : "uMatAmbient") + ";\n" +
             "  vec4 md = " + (e.colorMaterial ? "aColor" : "uMatDiffuse") + ";\n" +
             "  vec3 c = uMatEmission.rgb + ma.rgb * uModelAmbient.rgb;\n";
        for (int i = 0; i < kLights; ++i) {
            if (!e.lightOn[i]) continue;
            const std::string I = std::to_string(i);
            s += "  {\n"
                 "    vec3 L; float att = 1.0;\n"
                 "    if (uLPos[" + I + "].w == 0.0) L = normalize(uLPos[" + I + "].xyz);\n"
                 "    else { vec3 d = uLPos[" + I + "].xyz - eye.xyz; float dl = length(d); L = d / dl;\n"
                 "           att = 1.0 / (uLAtt[" + I + "].x + uLAtt[" + I + "].y * dl + uLAtt[" + I + "].z * dl * dl); }\n"
                 "    if (uLSpot[" + I + "].y > -0.5) { float sd = dot(-L, normalize(uLSpotDir[" + I + "]));\n"
                 "      att *= sd >= uLSpot[" + I + "].y ? pow(max(sd, 0.0), uLSpot[" + I + "].x) : 0.0; }\n"
                 "    float nl = max(dot(n, L), 0.0);\n"
                 "    vec3 h = normalize(L + vec3(0.0, 0.0, 1.0));\n"
                 "    float sp = nl > 0.0 ? pow(max(dot(n, h), 0.0), uMatShininess) : 0.0;\n"
                 "    c += att * (uLAmb[" + I + "].rgb * ma.rgb + nl * uLDif[" + I + "].rgb * md.rgb + sp * uLSpec[" + I + "].rgb * uMatSpecular.rgb);\n"
                 "  }\n";
        }
        s += "  vColor = clamp(vec4(c, md.a), 0.0, 1.0);\n";
    } else {
        s += "  vColor = aColor;\n";
    }
    if (points) {
        s += std::string("  float ps = ") + (e.arrayOn[LPointSize] ? "aPointSize" : "uPoint.x") + ";\n"
             "  float dd = length(eye.xyz);\n"
             "  ps *= inversesqrt(max(uPointAtt.x + uPointAtt.y * dd + uPointAtt.z * dd * dd, 1e-6));\n"
             "  gl_PointSize = clamp(ps, uPoint.y, uPoint.z);\n";
    }
    s += "}\n";
    return s;
}

// The texture source color and alpha a unit sees, by base internal format.
// ES 2 samples GL_ALPHA as (0,0,0,A) and GL_LUMINANCE as (L,L,L,1), which is
// what the ES 1.1 combine tables expect for sources.
std::string combineSrc(uint32_t src, uint32_t op, bool alpha, int unit) {
    std::string v;
    switch (src) {
        case kTexSrc: v = "t" + std::to_string(unit); break;
        case kConstant: v = "uEnvColor" + std::to_string(unit); break;
        case kPrimaryColor: v = "vColor"; break;
        default: v = "prev"; break;
    }
    if (alpha) {
        if (op == kOneMinusSrcAlpha) return "(1.0 - " + v + ".a)";
        return v + ".a";
    }
    switch (op) {
        case kOneMinusSrcColor: return "(vec3(1.0) - " + v + ".rgb)";
        case kSrcAlpha: return "vec3(" + v + ".a)";
        case kOneMinusSrcAlpha: return "vec3(1.0 - " + v + ".a)";
        default: return v + ".rgb";
    }
}

std::string combineOp(uint32_t fn, const std::string& a0, const std::string& a1, const std::string& a2, bool alpha) {
    switch (fn) {
        case kReplace: return a0;
        case kModulate: return a0 + " * " + a1;
        case kAdd: return a0 + " + " + a1;
        case kAddSigned: return a0 + " + " + a1 + (alpha ? " - 0.5" : " - vec3(0.5)");
        case kInterpolate: return a0 + " * " + a2 + " + " + a1 + " * (" + (alpha ? "1.0" : "vec3(1.0)") + " - " + a2 + ")";
        case kSubtract: return a0 + " - " + a1;
        case kDot3Rgb:
        case kDot3Rgba:
            return alpha ? a0 : "vec3(4.0 * dot(" + a0 + " - vec3(0.5), " + a1 + " - vec3(0.5)))";
        default: return a0 + " * " + a1;
    }
}

std::string fragmentShader(Gles1Emu& e, bool points) {
    std::string s =
        "precision mediump float;\n"
        "varying vec4 vColor; varying vec4 vTex0; varying vec4 vTex1; varying float vFogDist; varying float vClip[6];\n"
        "uniform sampler2D uSampler0; uniform sampler2D uSampler1; uniform vec4 uEnvColor0; uniform vec4 uEnvColor1;\n"
        "uniform vec2 uEnvScale0; uniform vec2 uEnvScale1; uniform vec4 uFog; uniform vec4 uFogColor; uniform float uAlphaRef;\n"
        "void main() {\n";
    for (int i = 0; i < kPlanes; ++i)
        if (e.clipOn[i]) s += "  if (vClip[" + std::to_string(i) + "] < 0.0) discard;\n";
    s += "  vec4 prev = vColor;\n";
    for (int u = 0; u < kUnits; ++u) {
        if (!e.tex2d[u]) continue;
        const std::string U = std::to_string(u);
        const Env& v = e.env[u];
        const bool sprite = points && e.pointSprite && v.coordReplace;
        s += "  vec4 t" + U + " = texture2D(uSampler" + U + ", " + (sprite ? "gl_PointCoord" : "vTex" + U + ".xy / vTex" + U + ".w") + ");\n";
        const uint32_t f = baseFormat(e, u);
        const bool hasColor = f != kAlpha, hasAlpha = f == kAlpha || f == kLuminanceAlpha || f == kRgba;
        const std::string T = "t" + U;
        if (v.mode == kCombine) {
            const std::string r = combineOp(v.combineRgb, combineSrc(v.srcRgb[0], v.opRgb[0], false, u), combineSrc(v.srcRgb[1], v.opRgb[1], false, u),
                                            combineSrc(v.srcRgb[2], v.opRgb[2], false, u), false);
            const std::string a = v.combineRgb == kDot3Rgba ? "" :
                combineOp(v.combineA, combineSrc(v.srcA[0], v.opA[0], true, u), combineSrc(v.srcA[1], v.opA[1], true, u),
                          combineSrc(v.srcA[2], v.opA[2], true, u), true);
            s += "  { vec3 cr = " + r + ";\n";
            if (v.combineRgb == kDot3Rgba) s += "    prev = clamp(vec4(cr, cr.r) * uEnvScale" + U + ".x, 0.0, 1.0); }\n";
            else s += "    float ca = " + a + ";\n    prev = clamp(vec4(cr * uEnvScale" + U + ".x, ca * uEnvScale" + U + ".y), 0.0, 1.0); }\n";
            continue;
        }
        std::string c = "prev.rgb", a = "prev.a";
        switch (v.mode) {
            case kReplace:
                if (hasColor) c = T + ".rgb";
                if (hasAlpha) a = T + ".a";
                break;
            case kDecal:
                if (hasColor) c = hasAlpha ? "mix(prev.rgb, " + T + ".rgb, " + T + ".a)" : T + ".rgb";
                break;
            case kBlend:
                if (hasColor) c = "mix(prev.rgb, uEnvColor" + U + ".rgb, " + T + ".rgb)";
                if (hasAlpha) a = "prev.a * " + T + ".a";
                break;
            case kAdd:
                if (hasColor) c = "prev.rgb + " + T + ".rgb";
                if (hasAlpha) a = "prev.a * " + T + ".a";
                break;
            default:   // MODULATE
                if (hasColor) c = "prev.rgb * " + T + ".rgb";
                if (hasAlpha) a = "prev.a * " + T + ".a";
                break;
        }
        s += "  prev = clamp(vec4(" + c + ", " + a + "), 0.0, 1.0);\n";
    }
    if (e.fog) {
        if (e.fogMode == kLinear) s += "  float fogf = (uFog.y - vFogDist) / (uFog.y - uFog.x);\n";
        else if (e.fogMode == kExp2) s += "  float fogf = exp(-(uFog.z * vFogDist) * (uFog.z * vFogDist));\n";
        else s += "  float fogf = exp(-uFog.z * vFogDist);\n";
        s += "  prev.rgb = mix(uFogColor.rgb, prev.rgb, clamp(fogf, 0.0, 1.0));\n";
    }
    if (e.alphaTest) {
        const char* cmp = nullptr;
        switch (e.alphaFunc) {
            case kNever: s += "  discard;\n"; break;
            case 0x0201: cmp = "<"; break;    // LESS
            case 0x0202: cmp = "=="; break;   // EQUAL
            case 0x0203: cmp = "<="; break;   // LEQUAL
            case 0x0204: cmp = ">"; break;    // GREATER
            case 0x0205: cmp = "!="; break;   // NOTEQUAL
            case 0x0206: cmp = ">="; break;   // GEQUAL
            default: break;                   // ALWAYS
        }
        if (cmp) s += std::string("  if (!(prev.a ") + cmp + " uAlphaRef)) discard;\n";
    }
    s += "  gl_FragColor = prev;\n}\n";
    return s;
}

uint32_t compile(Fns& r, uint32_t type, const std::string& src) {
    const uint32_t sh = r.glCreateShader(type);
    const char* p = src.c_str();
    r.glShaderSource(sh, 1, &p, nullptr);
    r.glCompileShader(sh);
    int32_t ok = 0;
    r.glGetShaderiv(sh, kCompileStatus, &ok);
    if (!ok) {
        char log[2048] = {};
        r.glGetShaderInfoLog(sh, sizeof log - 1, nullptr, log);
        std::fprintf(stderr, "[hvm:warn] GLES1 emulation: shader failed to compile: %s\n%s\n", log, src.c_str());
    }
    return sh;
}

ProgramInfo& program(Gles1Emu& e, bool points) {
    const std::string key = stateKey(e, points);
    auto it = e.programs.find(key);
    if (it != e.programs.end()) return it->second;
    Fns& r = e.real;
    ProgramInfo pi;
    pi.id = r.glCreateProgram();
    const uint32_t vs = compile(r, kVertexShader, vertexShader(e, points));
    const uint32_t fs = compile(r, kFragmentShader, fragmentShader(e, points));
    r.glAttachShader(pi.id, vs);
    r.glAttachShader(pi.id, fs);
    r.glBindAttribLocation(pi.id, LPos, "aPos");
    r.glBindAttribLocation(pi.id, LColor, "aColor");
    r.glBindAttribLocation(pi.id, LNormal, "aNormal");
    r.glBindAttribLocation(pi.id, LTex0, "aTex0");
    r.glBindAttribLocation(pi.id, LTex1, "aTex1");
    r.glBindAttribLocation(pi.id, LPointSize, "aPointSize");
    r.glLinkProgram(pi.id);
    int32_t ok = 0;
    r.glGetProgramiv(pi.id, kLinkStatus, &ok);
    if (!ok) {
        char log[2048] = {};
        r.glGetProgramInfoLog(pi.id, sizeof log - 1, nullptr, log);
        std::fprintf(stderr, "[hvm:warn] GLES1 emulation: program failed to link: %s\n", log);
    }
    r.glDeleteShader(vs);
    r.glDeleteShader(fs);
    auto U = [&](const char* n) { return int(r.glGetUniformLocation(pi.id, n)); };
    pi.uMvp = U("uMvp"); pi.uMv = U("uMv"); pi.uNormal = U("uNormal");
    pi.uTexMat[0] = U("uTexMat0"); pi.uTexMat[1] = U("uTexMat1");
    pi.uSampler[0] = U("uSampler0"); pi.uSampler[1] = U("uSampler1");
    pi.uMatAmbient = U("uMatAmbient"); pi.uMatDiffuse = U("uMatDiffuse"); pi.uMatSpecular = U("uMatSpecular");
    pi.uMatEmission = U("uMatEmission"); pi.uMatShininess = U("uMatShininess"); pi.uModelAmbient = U("uModelAmbient");
    pi.uLAmb = U("uLAmb"); pi.uLDif = U("uLDif"); pi.uLSpec = U("uLSpec"); pi.uLPos = U("uLPos");
    pi.uLSpotDir = U("uLSpotDir"); pi.uLSpot = U("uLSpot"); pi.uLAtt = U("uLAtt");
    pi.uFog = U("uFog"); pi.uFogColor = U("uFogColor"); pi.uAlphaRef = U("uAlphaRef");
    pi.uEnvColor[0] = U("uEnvColor0"); pi.uEnvColor[1] = U("uEnvColor1");
    pi.uEnvScale[0] = U("uEnvScale0"); pi.uEnvScale[1] = U("uEnvScale1");
    pi.uPoint = U("uPoint"); pi.uPointAtt = U("uPointAtt"); pi.uPlanes = U("uPlanes");
    return e.programs.emplace(key, pi).first->second;
}

void prepareDraw(Gles1Emu& e, bool points) {
    Fns& r = e.real;
    ProgramInfo& pi = program(e, points);
    r.glUseProgram(pi.id);
    const Mat4& mvm = e.mv.back();
    const Mat4 mvp = e.proj.back() * mvm;
    r.glUniformMatrix4fv(pi.uMvp, 1, GL_FALSE_, mvp.m);
    r.glUniformMatrix4fv(pi.uMv, 1, GL_FALSE_, mvm.m);
    for (int u = 0; u < kUnits; ++u) {
        if (pi.uTexMat[u] >= 0) r.glUniformMatrix4fv(pi.uTexMat[u], 1, GL_FALSE_, e.tex[u].back().m);
        if (pi.uSampler[u] >= 0) r.glUniform1i(pi.uSampler[u], u);
        if (pi.uEnvColor[u] >= 0) r.glUniform4fv(pi.uEnvColor[u], 1, e.env[u].color);
        if (pi.uEnvScale[u] >= 0) r.glUniform2f(pi.uEnvScale[u], e.env[u].rgbScale, e.env[u].alphaScale);
    }
    if (e.lighting) {
        float nm[9];
        normalMatrix(mvm, nm);
        if (e.rescale && !e.normalize) {
            // GL_RESCALE_NORMAL with a uniform scale: the shader normalizes anyway.
        }
        r.glUniformMatrix3fv(pi.uNormal, 1, GL_FALSE_, nm);
        r.glUniform4fv(pi.uMatAmbient, 1, e.matAmbient);
        r.glUniform4fv(pi.uMatDiffuse, 1, e.matDiffuse);
        r.glUniform4fv(pi.uMatSpecular, 1, e.matSpecular);
        r.glUniform4fv(pi.uMatEmission, 1, e.matEmission);
        r.glUniform1f(pi.uMatShininess, e.matShininess);
        r.glUniform4fv(pi.uModelAmbient, 1, e.modelAmbient);
        float amb[32], dif[32], spec[32], pos[32], dir[24], spot[16], att[24];
        for (int i = 0; i < kLights; ++i) {
            const Light& l = e.light[i];
            std::memcpy(amb + i * 4, l.ambient, 16); std::memcpy(dif + i * 4, l.diffuse, 16);
            std::memcpy(spec + i * 4, l.specular, 16); std::memcpy(pos + i * 4, l.position, 16);
            std::memcpy(dir + i * 3, l.spotDir, 12); std::memcpy(att + i * 3, l.att, 12);
            spot[i * 2] = l.spotExp;
            spot[i * 2 + 1] = l.spotCutoff >= 180 ? -1.0f : std::cos(l.spotCutoff * float(M_PI) / 180.0f);
        }
        r.glUniform4fv(pi.uLAmb, kLights, amb); r.glUniform4fv(pi.uLDif, kLights, dif);
        r.glUniform4fv(pi.uLSpec, kLights, spec); r.glUniform4fv(pi.uLPos, kLights, pos);
        r.glUniform3fv(pi.uLSpotDir, kLights, dir); r.glUniform2fv(pi.uLSpot, kLights, spot);
        r.glUniform3fv(pi.uLAtt, kLights, att);
    }
    if (e.fog) {
        r.glUniform4f(pi.uFog, e.fogStart, e.fogEnd, e.fogDensity, 0);
        r.glUniform4fv(pi.uFogColor, 1, e.fogColor);
    }
    if (e.alphaTest) r.glUniform1f(pi.uAlphaRef, e.alphaRef);
    if (points) {
        r.glUniform3f(pi.uPoint, e.pointSize, e.pointMin, e.pointMax);
        r.glUniform3f(pi.uPointAtt, e.pointAtt[0], e.pointAtt[1], e.pointAtt[2]);
    }
    if (pi.uPlanes >= 0) r.glUniform4fv(pi.uPlanes, kPlanes, &e.planes[0][0]);
}

// ------------------------------------------------------------ overrides ---
// Same signatures as the host GL functions (see HostGLTable.inc).

int capIndex(uint32_t cap) { return cap >= kLight0 && cap < kLight0 + kLights ? int(cap - kLight0) : -1; }

// true if `cap` is fixed-function state held here.
bool setCap(Gles1Emu& e, uint32_t cap, bool on) {
    if (cap == kCapTexture2D) { e.tex2d[e.activeUnit] = on; return true; }
    if (const int i = capIndex(cap); i >= 0) { e.lightOn[i] = on; return true; }
    if (cap >= kClipPlane0 && cap < kClipPlane0 + kPlanes) { e.clipOn[cap - kClipPlane0] = on; return true; }
    switch (cap) {
        case kLighting: e.lighting = on; return true;
        case kFog: e.fog = on; return true;
        case kAlphaTest: e.alphaTest = on; return true;
        case kNormalize: e.normalize = on; return true;
        case kRescaleNormal: e.rescale = on; return true;
        case kColorMaterial: e.colorMaterial = on; return true;
        case kPointSprite: e.pointSprite = on; return true;
        case kPointSmooth: e.pointSmooth = on; return true;
        case kLineSmooth: e.lineSmooth = on; return true;
        case kMultisample: e.multisample = on; return true;
        case kSampleAlphaToOne: e.alphaToOne = on; return true;
        case kColorLogicOp: e.logicOp = on; return true;
        default: return false;
    }
}
bool getCap(Gles1Emu& e, uint32_t cap, bool& on) {
    if (cap == kCapTexture2D) { on = e.tex2d[e.activeUnit]; return true; }
    if (const int i = capIndex(cap); i >= 0) { on = e.lightOn[i]; return true; }
    if (cap >= kClipPlane0 && cap < kClipPlane0 + kPlanes) { on = e.clipOn[cap - kClipPlane0]; return true; }
    switch (cap) {
        case kLighting: on = e.lighting; return true;
        case kFog: on = e.fog; return true;
        case kAlphaTest: on = e.alphaTest; return true;
        case kNormalize: on = e.normalize; return true;
        case kRescaleNormal: on = e.rescale; return true;
        case kColorMaterial: on = e.colorMaterial; return true;
        case kPointSprite: on = e.pointSprite; return true;
        case kPointSmooth: on = e.pointSmooth; return true;
        case kLineSmooth: on = e.lineSmooth; return true;
        case kMultisample: on = e.multisample; return true;
        case kSampleAlphaToOne: on = e.alphaToOne; return true;
        case kColorLogicOp: on = e.logicOp; return true;
        case kVertexArray: on = e.arrayOn[LPos]; return true;
        case kColorArray: on = e.arrayOn[LColor]; return true;
        case kNormalArray: on = e.arrayOn[LNormal]; return true;
        case kTexCoordArray: on = e.arrayOn[LTex0 + e.clientUnit]; return true;
        case kPointSizeArrayOES: on = e.arrayOn[LPointSize]; return true;
        default: return false;
    }
}

void eEnable(uint32_t cap) { if (!setCap(E(), cap, true)) E().real.glEnable(cap); }
void eDisable(uint32_t cap) { if (!setCap(E(), cap, false)) E().real.glDisable(cap); }
uint8_t eIsEnabled(uint32_t cap) { bool on; return getCap(E(), cap, on) ? on : E().real.glIsEnabled(cap); }

int arrayLoc(Gles1Emu& e, uint32_t array) {
    switch (array) {
        case kVertexArray: return LPos;
        case kColorArray: return LColor;
        case kNormalArray: return LNormal;
        case kTexCoordArray: return int(LTex0 + e.clientUnit);
        case kPointSizeArrayOES: return LPointSize;
        default: return -1;
    }
}
void syncCurrent(Gles1Emu& e, int loc) {
    Fns& r = e.real;
    switch (loc) {
        case LColor: r.glVertexAttrib4f(LColor, e.color[0], e.color[1], e.color[2], e.color[3]); break;
        case LNormal: r.glVertexAttrib3f(LNormal, e.normal[0], e.normal[1], e.normal[2]); break;
        case LTex0: case LTex1: {
            const float* t = e.texcoord[loc - LTex0];
            r.glVertexAttrib4f(loc, t[0], t[1], t[2], t[3]);
            break;
        }
        case LPointSize: r.glVertexAttrib1f(LPointSize, e.pointSize); break;
        default: break;
    }
}
void eEnableClientState(uint32_t array) {
    Gles1Emu& e = E();
    const int loc = arrayLoc(e, array);
    if (loc < 0) return;
    e.arrayOn[loc] = true;
    e.real.glEnableVertexAttribArray(uint32_t(loc));
}
void eDisableClientState(uint32_t array) {
    Gles1Emu& e = E();
    const int loc = arrayLoc(e, array);
    if (loc < 0) return;
    e.arrayOn[loc] = false;
    e.real.glDisableVertexAttribArray(uint32_t(loc));
    syncCurrent(e, loc);
}
void setPointer(int loc, int32_t size, uint32_t type, int32_t stride, const void* ptr, bool normalized) {
    Gles1Emu& e = E();
    e.arrayPtr[loc] = ptr;
    e.arraySize[loc] = size;
    e.arrayType[loc] = type;
    e.arrayStride[loc] = stride;
    // Fixed point is never normalized; integer colors and normals are.
    e.real.glVertexAttribPointer(uint32_t(loc), size, type, normalized && type != kFloat && type != kFixed ? 1 : 0, stride, ptr);
}
void eVertexPointer(int32_t size, uint32_t type, int32_t stride, const void* ptr) { setPointer(LPos, size, type, stride, ptr, false); }
void eColorPointer(int32_t size, uint32_t type, int32_t stride, const void* ptr) { setPointer(LColor, size, type, stride, ptr, true); }
void eNormalPointer(uint32_t type, int32_t stride, const void* ptr) { setPointer(LNormal, 3, type, stride, ptr, true); }
void eTexCoordPointer(int32_t size, uint32_t type, int32_t stride, const void* ptr) {
    setPointer(int(LTex0 + E().clientUnit), size, type, stride, ptr, false);
}
void ePointSizePointerOES(uint32_t type, int32_t stride, const void* ptr) { setPointer(LPointSize, 1, type, stride, ptr, false); }
void eClientActiveTexture(uint32_t t) { const uint32_t u = t - kTexture0; if (u < kUnits) E().clientUnit = u; }
void eActiveTexture(uint32_t t) {
    Gles1Emu& e = E();
    const uint32_t u = t - kTexture0;
    if (u < kUnits) e.activeUnit = u;
    e.real.glActiveTexture(t);
}
void eBindTexture(uint32_t target, uint32_t tex) {
    Gles1Emu& e = E();
    if (target == 0x0DE1) e.boundTex[e.activeUnit] = tex;
    e.real.glBindTexture(target, tex);
}
void eDeleteTextures(int32_t n, const void* ids) {
    Gles1Emu& e = E();
    const auto* p = static_cast<const uint32_t*>(ids);
    for (int32_t i = 0; i < n && p; ++i) { e.texFormat.erase(p[i]); e.texGenMip.erase(p[i]); e.crop.erase(p[i]); }
    e.real.glDeleteTextures(n, ids);
}

uint32_t baseOf(uint32_t internal) {
    switch (internal) {
        case kAlpha: case 0x803C /*ALPHA8*/: return kAlpha;
        case kLuminance: case 0x8040 /*LUMINANCE8*/: return kLuminance;
        case kLuminanceAlpha: case 0x8045 /*LUMINANCE8_ALPHA8*/: return kLuminanceAlpha;
        case kRgb: case 0x8051 /*RGB8*/: case 0x8D62 /*RGB565*/: case 0x8C00: case 0x8C01: case 0x8D64 /*ETC1*/: return kRgb;
        default: return kRgba;
    }
}
void noteUpload(Gles1Emu& e, int32_t level, uint32_t internal) {
    if (level == 0) e.texFormat[e.boundTex[e.activeUnit]] = baseOf(internal);
}
void maybeMipmap(Gles1Emu& e, int32_t level) {
    if (level != 0) return;
    auto it = e.texGenMip.find(e.boundTex[e.activeUnit]);
    if (it != e.texGenMip.end() && it->second && e.real.glGenerateMipmap) e.real.glGenerateMipmap(0x0DE1);
}
void eTexImage2D(uint32_t target, int32_t level, int32_t internal, int32_t w, int32_t h, int32_t border, uint32_t format, uint32_t type, const void* px) {
    Gles1Emu& e = E();
    noteUpload(e, level, format == kBgra ? kRgba : uint32_t(internal));
    e.real.glTexImage2D(target, level, internal, w, h, border, format, type, px);
    maybeMipmap(e, level);
}
void eTexSubImage2D(uint32_t target, int32_t level, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t format, uint32_t type, const void* px) {
    Gles1Emu& e = E();
    e.real.glTexSubImage2D(target, level, x, y, w, h, format, type, px);
    maybeMipmap(e, level);
}
void eCompressedTexImage2D(uint32_t target, int32_t level, uint32_t internal, int32_t w, int32_t h, int32_t border, int32_t size, const void* data) {
    Gles1Emu& e = E();
    noteUpload(e, level, internal);
    e.real.glCompressedTexImage2D(target, level, internal, w, h, border, size, data);
}
void eCopyTexImage2D(uint32_t target, int32_t level, uint32_t internal, int32_t x, int32_t y, int32_t w, int32_t h, int32_t border) {
    Gles1Emu& e = E();
    noteUpload(e, level, internal);
    e.real.glCopyTexImage2D(target, level, internal, x, y, w, h, border);
    maybeMipmap(e, level);
}
void eTexParameteri(uint32_t target, uint32_t pname, int32_t v) {
    Gles1Emu& e = E();
    if (pname == kGenerateMipmap) { e.texGenMip[e.boundTex[e.activeUnit]] = v != 0; return; }
    e.real.glTexParameteri(target, pname, v);
}
void eTexParameterf(uint32_t target, uint32_t pname, float v) {
    if (pname == kGenerateMipmap) { eTexParameteri(target, pname, int32_t(v)); return; }
    E().real.glTexParameterf(target, pname, v);
}
void eTexParameteriv(uint32_t target, uint32_t pname, const void* v) {
    Gles1Emu& e = E();
    const auto* p = static_cast<const int32_t*>(v);
    if (pname == kTextureCropRect) { if (p) e.crop[e.boundTex[e.activeUnit]] = {p[0], p[1], p[2], p[3]}; return; }
    if (pname == kGenerateMipmap) { if (p) eTexParameteri(target, pname, p[0]); return; }
    e.real.glTexParameteriv(target, pname, v);
}
void eTexParameterfv(uint32_t target, uint32_t pname, const void* v) {
    const auto* p = static_cast<const float*>(v);
    if (pname == kTextureCropRect) {
        if (!p) return;
        const int32_t i[4] = {int32_t(p[0]), int32_t(p[1]), int32_t(p[2]), int32_t(p[3])};
        eTexParameteriv(target, pname, i);
        return;
    }
    if (pname == kGenerateMipmap) { if (p) eTexParameteri(target, pname, int32_t(p[0])); return; }
    E().real.glTexParameterfv(target, pname, v);
}

// --- texture environment
void texEnv(uint32_t target, uint32_t pname, const float* v, bool isInt) {
    Gles1Emu& e = E();
    Env& env = e.env[e.activeUnit];
    auto asEnum = [&](float f) { return uint32_t(f); };
    if (target == kPointSpriteTarget) { if (pname == kCoordReplace) env.coordReplace = v[0] != 0; return; }
    if (target != kTextureEnv) return;
    switch (pname) {
        case kTextureEnvMode: env.mode = asEnum(v[0]); break;
        case kCombineRgb: env.combineRgb = asEnum(v[0]); break;
        case kCombineAlpha: env.combineA = asEnum(v[0]); break;
        case kRgbScale: env.rgbScale = v[0]; break;
        case kAlphaScale: env.alphaScale = v[0]; break;
        case kTextureEnvColor:
            for (int i = 0; i < 4; ++i) env.color[i] = isInt ? std::clamp(v[i] / 2147483647.0f, 0.0f, 1.0f) : v[i];
            break;
        default:
            if (pname >= kSrc0Rgb && pname < kSrc0Rgb + 3) env.srcRgb[pname - kSrc0Rgb] = asEnum(v[0]);
            else if (pname >= kSrc0Alpha && pname < kSrc0Alpha + 3) env.srcA[pname - kSrc0Alpha] = asEnum(v[0]);
            else if (pname >= kOperand0Rgb && pname < kOperand0Rgb + 3) env.opRgb[pname - kOperand0Rgb] = asEnum(v[0]);
            else if (pname >= kOperand0Alpha && pname < kOperand0Alpha + 3) env.opA[pname - kOperand0Alpha] = asEnum(v[0]);
            break;
    }
}
void eTexEnvf(uint32_t t, uint32_t p, float v) { const float a[4] = {v, 0, 0, 0}; texEnv(t, p, a, false); }
void eTexEnvi(uint32_t t, uint32_t p, int32_t v) { const float a[4] = {float(v), 0, 0, 0}; texEnv(t, p, a, false); }
void eTexEnvfv(uint32_t t, uint32_t p, const void* v) {
    const auto* f = static_cast<const float*>(v);
    float a[4] = {};
    for (int i = 0; i < (p == kTextureEnvColor ? 4 : 1); ++i) a[i] = f[i];
    texEnv(t, p, a, false);
}
void eTexEnviv(uint32_t t, uint32_t p, const void* v) {
    const auto* iv = static_cast<const int32_t*>(v);
    float a[4] = {};
    for (int i = 0; i < (p == kTextureEnvColor ? 4 : 1); ++i) a[i] = float(iv[i]);
    texEnv(t, p, a, p == kTextureEnvColor);
}
void eGetTexEnvfv(uint32_t t, uint32_t p, void* out) {
    Gles1Emu& e = E();
    const Env& env = e.env[e.activeUnit];
    auto* f = static_cast<float*>(out);
    if (!f) return;
    if (t == kPointSpriteTarget) { f[0] = env.coordReplace; return; }
    switch (p) {
        case kTextureEnvMode: f[0] = float(env.mode); break;
        case kTextureEnvColor: std::memcpy(f, env.color, 16); break;
        case kRgbScale: f[0] = env.rgbScale; break;
        case kAlphaScale: f[0] = env.alphaScale; break;
        case kCombineRgb: f[0] = float(env.combineRgb); break;
        case kCombineAlpha: f[0] = float(env.combineA); break;
        default: f[0] = 0; break;
    }
}
void eGetTexEnviv(uint32_t t, uint32_t p, void* out) {
    float f[4] = {};
    eGetTexEnvfv(t, p, f);
    auto* iv = static_cast<int32_t*>(out);
    if (!iv) return;
    if (p == kTextureEnvColor) for (int i = 0; i < 4; ++i) iv[i] = int32_t(f[i] * 2147483647.0f);
    else iv[0] = int32_t(f[0]);
}

// --- matrices
void eMatrixMode(uint32_t m) { E().matrixMode = m; }
void eLoadIdentity() { E().top() = Mat4::identity(); }
void eLoadMatrixf(const void* m) { E().top() = fromArray(static_cast<const float*>(m)); }
void eMultMatrixf(const void* m) { Gles1Emu& e = E(); e.top() = e.top() * fromArray(static_cast<const float*>(m)); }
void ePushMatrix() { auto& s = E().stack(); if (s.size() < 32) s.push_back(s.back()); }
void ePopMatrix() { auto& s = E().stack(); if (s.size() > 1) s.pop_back(); }
void eOrthof(float l, float r, float b, float t, float n, float f) { Gles1Emu& e = E(); e.top() = e.top() * ortho(l, r, b, t, n, f); }
void eFrustumf(float l, float r, float b, float t, float n, float f) { Gles1Emu& e = E(); e.top() = e.top() * frustum(l, r, b, t, n, f); }
void eTranslatef(float x, float y, float z) { Gles1Emu& e = E(); e.top() = e.top() * translate(x, y, z); }
void eScalef(float x, float y, float z) { Gles1Emu& e = E(); e.top() = e.top() * scale(x, y, z); }
void eRotatef(float a, float x, float y, float z) { Gles1Emu& e = E(); e.top() = e.top() * rotate(a, x, y, z); }

// --- current values
void eColor4f(float r, float g, float b, float a) { Gles1Emu& e = E(); e.color[0] = r; e.color[1] = g; e.color[2] = b; e.color[3] = a; syncCurrent(e, LColor); }
void eColor4ub(uint8_t r, uint8_t g, uint8_t b, uint8_t a) { eColor4f(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f); }
void eNormal3f(float x, float y, float z) { Gles1Emu& e = E(); e.normal[0] = x; e.normal[1] = y; e.normal[2] = z; syncCurrent(e, LNormal); }
void eMultiTexCoord4f(uint32_t t, float s, float tt, float r, float q) {
    Gles1Emu& e = E();
    const uint32_t u = t - kTexture0;
    if (u >= kUnits) return;
    e.texcoord[u][0] = s; e.texcoord[u][1] = tt; e.texcoord[u][2] = r; e.texcoord[u][3] = q;
    syncCurrent(e, int(LTex0 + u));
}

// --- lighting / material / fog
void lightv(uint32_t li, uint32_t p, const float* v) {
    Gles1Emu& e = E();
    const int i = int(li - kLight0);
    if (i < 0 || i >= kLights) return;
    Light& l = e.light[i];
    switch (p) {
        case kAmbient: std::memcpy(l.ambient, v, 16); break;
        case kDiffuse: std::memcpy(l.diffuse, v, 16); break;
        case kSpecular: std::memcpy(l.specular, v, 16); break;
        case kPosition: e.mv.back().apply(v, l.position); break;
        case kSpotDirection: {
            const float d[4] = {v[0], v[1], v[2], 0};
            float o[4];
            e.mv.back().apply(d, o);
            std::memcpy(l.spotDir, o, 12);
            break;
        }
        case kSpotExponent: l.spotExp = v[0]; break;
        case kSpotCutoff: l.spotCutoff = v[0]; break;
        case kConstantAtt: l.att[0] = v[0]; break;
        case kLinearAtt: l.att[1] = v[0]; break;
        case kQuadraticAtt: l.att[2] = v[0]; break;
        default: break;
    }
}
void eLightf(uint32_t l, uint32_t p, float v) { const float a[4] = {v, 0, 0, 0}; lightv(l, p, a); }
void eLightfv(uint32_t l, uint32_t p, const void* v) { lightv(l, p, static_cast<const float*>(v)); }
void eGetLightfv(uint32_t li, uint32_t p, void* out) {
    Gles1Emu& e = E();
    const int i = int(li - kLight0);
    auto* f = static_cast<float*>(out);
    if (i < 0 || i >= kLights || !f) return;
    const Light& l = e.light[i];
    switch (p) {
        case kAmbient: std::memcpy(f, l.ambient, 16); break;
        case kDiffuse: std::memcpy(f, l.diffuse, 16); break;
        case kSpecular: std::memcpy(f, l.specular, 16); break;
        case kPosition: std::memcpy(f, l.position, 16); break;
        case kSpotDirection: std::memcpy(f, l.spotDir, 12); break;
        case kSpotExponent: f[0] = l.spotExp; break;
        case kSpotCutoff: f[0] = l.spotCutoff; break;
        case kConstantAtt: f[0] = l.att[0]; break;
        case kLinearAtt: f[0] = l.att[1]; break;
        case kQuadraticAtt: f[0] = l.att[2]; break;
        default: break;
    }
}
void materialv(uint32_t, uint32_t p, const float* v) {
    Gles1Emu& e = E();
    switch (p) {
        case kAmbient: std::memcpy(e.matAmbient, v, 16); break;
        case kDiffuse: std::memcpy(e.matDiffuse, v, 16); break;
        case kAmbientAndDiffuse: std::memcpy(e.matAmbient, v, 16); std::memcpy(e.matDiffuse, v, 16); break;
        case kSpecular: std::memcpy(e.matSpecular, v, 16); break;
        case kEmission: std::memcpy(e.matEmission, v, 16); break;
        case kShininess: e.matShininess = v[0]; break;
        default: break;
    }
}
void eMaterialf(uint32_t f, uint32_t p, float v) { const float a[4] = {v, 0, 0, 0}; materialv(f, p, a); }
void eMaterialfv(uint32_t f, uint32_t p, const void* v) { materialv(f, p, static_cast<const float*>(v)); }
void eGetMaterialfv(uint32_t, uint32_t p, void* out) {
    Gles1Emu& e = E();
    auto* f = static_cast<float*>(out);
    if (!f) return;
    switch (p) {
        case kAmbient: std::memcpy(f, e.matAmbient, 16); break;
        case kDiffuse: std::memcpy(f, e.matDiffuse, 16); break;
        case kSpecular: std::memcpy(f, e.matSpecular, 16); break;
        case kEmission: std::memcpy(f, e.matEmission, 16); break;
        case kShininess: f[0] = e.matShininess; break;
        default: break;
    }
}
void eLightModelf(uint32_t p, float v) { if (p == kLightModelTwoSide) E().twoSide = v != 0; }
void eLightModelfv(uint32_t p, const void* v) {
    const auto* f = static_cast<const float*>(v);
    if (p == kLightModelAmbient) std::memcpy(E().modelAmbient, f, 16);
    else if (p == kLightModelTwoSide) E().twoSide = f[0] != 0;
}
void fogv(uint32_t p, const float* v) {
    Gles1Emu& e = E();
    switch (p) {
        case kFogMode: e.fogMode = uint32_t(v[0]); break;
        case kFogDensity: e.fogDensity = v[0]; break;
        case kFogStart: e.fogStart = v[0]; break;
        case kFogEnd: e.fogEnd = v[0]; break;
        case kFogColor: std::memcpy(e.fogColor, v, 16); break;
        default: break;
    }
}
void eFogf(uint32_t p, float v) { const float a[4] = {v, 0, 0, 0}; fogv(p, a); }
void eFogfv(uint32_t p, const void* v) { fogv(p, static_cast<const float*>(v)); }
void eAlphaFunc(uint32_t f, float ref) { Gles1Emu& e = E(); e.alphaFunc = f; e.alphaRef = std::clamp(ref, 0.0f, 1.0f); }
void eShadeModel(uint32_t m) { E().shadeModel = m; }
void eLogicOp(uint32_t) {}
void ePointSize(float s) { Gles1Emu& e = E(); e.pointSize = s; syncCurrent(e, LPointSize); }
void ePointParameterf(uint32_t p, float v) {
    Gles1Emu& e = E();
    if (p == kPointSizeMin) e.pointMin = v;
    else if (p == kPointSizeMax) e.pointMax = v;
    else if (p == kPointFadeThreshold) e.pointFade = v;
}
void ePointParameterfv(uint32_t p, const void* v) {
    const auto* f = static_cast<const float*>(v);
    if (p == kPointDistanceAttenuation) std::memcpy(E().pointAtt, f, 12);
    else ePointParameterf(p, f[0]);
}
void eClipPlanef(uint32_t plane, const void* eq) {
    Gles1Emu& e = E();
    const uint32_t i = plane - kClipPlane0;
    if (i >= kPlanes) return;
    // Stored in eye space: p_eye = p * inverse(modelview).
    Mat4 inv;
    if (!invert(e.mv.back(), inv)) return;
    const float* p = static_cast<const float*>(eq);
    for (int c = 0; c < 4; ++c) e.planes[i][c] = p[0] * inv.m[c * 4 + 0] + p[1] * inv.m[c * 4 + 1] + p[2] * inv.m[c * 4 + 2] + p[3] * inv.m[c * 4 + 3];
}
void eGetClipPlanef(uint32_t plane, void* out) {
    const uint32_t i = plane - kClipPlane0;
    if (i < kPlanes && out) std::memcpy(out, E().planes[i], 16);
}
void eHint(uint32_t target, uint32_t mode) {
    if (target == kPerspectiveCorrectionHint || target == kPointSmoothHint || target == kLineSmoothHint || target == kFogHint) return;
    E().real.glHint(target, mode);
}

// --- queries
bool getFloats(Gles1Emu& e, uint32_t p, float* out) {
    switch (p) {
        case kModelviewMatrix: std::memcpy(out, e.mv.back().m, 64); return true;
        case kProjectionMatrix: std::memcpy(out, e.proj.back().m, 64); return true;
        case kTextureMatrix: std::memcpy(out, e.tex[e.activeUnit].back().m, 64); return true;
        case kCurrentColor: std::memcpy(out, e.color, 16); return true;
        case kCurrentNormal: std::memcpy(out, e.normal, 12); return true;
        case kCurrentTextureCoords: std::memcpy(out, e.texcoord[e.clientUnit], 16); return true;
        case kAlphaTestRef: out[0] = e.alphaRef; return true;
        case kPointSize: out[0] = e.pointSize; return true;
        case kPointSizeMin: out[0] = e.pointMin; return true;
        case kPointSizeMax: out[0] = e.pointMax; return true;
        case kPointFadeThreshold: out[0] = e.pointFade; return true;
        case kPointDistanceAttenuation: std::memcpy(out, e.pointAtt, 12); return true;
        case kSmoothPointSizeRange: case kAliasedPointSizeRange: out[0] = 1; out[1] = 64; return true;
        case kFogDensity: out[0] = e.fogDensity; return true;
        case kFogStart: out[0] = e.fogStart; return true;
        case kFogEnd: out[0] = e.fogEnd; return true;
        case kFogColor: std::memcpy(out, e.fogColor, 16); return true;
        case kLightModelAmbient: std::memcpy(out, e.modelAmbient, 16); return true;
        default: return false;
    }
}
bool getInts(Gles1Emu& e, uint32_t p, int32_t* out) {
    switch (p) {
        case kMatrixMode: out[0] = int32_t(e.matrixMode); return true;
        case kMaxTextureUnits: out[0] = kUnits; return true;
        case kMaxLights: out[0] = kLights; return true;
        case kMaxClipPlanes: out[0] = kPlanes; return true;
        case kMaxModelviewStackDepth: out[0] = 32; return true;
        case kMaxProjectionStackDepth: case kMaxTextureStackDepth: out[0] = 32; return true;
        case kModelviewStackDepth: out[0] = int32_t(e.mv.size()); return true;
        case kProjectionStackDepth: out[0] = int32_t(e.proj.size()); return true;
        case kTextureStackDepth: out[0] = int32_t(e.tex[e.activeUnit].size()); return true;
        case kClientActiveTexture: out[0] = int32_t(kTexture0 + e.clientUnit); return true;
        case kAlphaTestFunc: out[0] = int32_t(e.alphaFunc); return true;
        case kShadeModel: out[0] = int32_t(e.shadeModel); return true;
        case kFogMode: out[0] = int32_t(e.fogMode); return true;
        case kVertexArraySize: out[0] = e.arraySize[LPos]; return true;
        case kVertexArrayType: out[0] = int32_t(e.arrayType[LPos]); return true;
        case kVertexArrayStride: out[0] = e.arrayStride[LPos]; return true;
        case kColorArraySize: out[0] = e.arraySize[LColor]; return true;
        case kTexCoordArraySize: out[0] = e.arraySize[LTex0 + e.clientUnit]; return true;
        default: {
            float f[16];
            if (!getFloats(e, p, f)) return false;
            const int n = (p == kModelviewMatrix || p == kProjectionMatrix || p == kTextureMatrix) ? 16 : 4;
            for (int i = 0; i < n; ++i) out[i] = int32_t(std::lround(f[i]));
            return true;
        }
    }
}
void eGetIntegerv(uint32_t p, void* out) {
    Gles1Emu& e = E();
    bool on;
    if (getInts(e, p, static_cast<int32_t*>(out))) return;
    if (getCap(e, p, on)) { static_cast<int32_t*>(out)[0] = on; return; }
    e.real.glGetIntegerv(p, out);
}
void eGetFloatv(uint32_t p, void* out) {
    Gles1Emu& e = E();
    if (getFloats(e, p, static_cast<float*>(out))) return;
    int32_t iv[4] = {};
    if (getInts(e, p, iv)) { static_cast<float*>(out)[0] = float(iv[0]); return; }
    e.real.glGetFloatv(p, out);
}
void eGetBooleanv(uint32_t p, void* out) {
    Gles1Emu& e = E();
    bool on;
    if (getCap(e, p, on)) { static_cast<uint8_t*>(out)[0] = on; return; }
    e.real.glGetBooleanv(p, out);
}
void eGetPointerv(uint32_t p, void* out) {
    Gles1Emu& e = E();
    auto** o = static_cast<const void**>(out);
    switch (p) {
        case kVertexArrayPointer: *o = e.arrayPtr[LPos]; break;
        case kColorArrayPointer: *o = e.arrayPtr[LColor]; break;
        case kNormalArrayPointer: *o = e.arrayPtr[LNormal]; break;
        case kTexCoordArrayPointer: *o = e.arrayPtr[LTex0 + e.clientUnit]; break;
        case kPointSizeArrayPointer: *o = e.arrayPtr[LPointSize]; break;
        default: *o = nullptr; break;
    }
}

// --- draws
void eDrawArrays(uint32_t mode, int32_t first, int32_t count) {
    Gles1Emu& e = E();
    if (!e.arrayOn[LPos]) return;   // ES 1.1 draws nothing without a vertex array
    prepareDraw(e, mode == kPoints);
    e.real.glDrawArrays(mode, first, count);
}
void eDrawElements(uint32_t mode, int32_t count, uint32_t type, const void* idx) {
    Gles1Emu& e = E();
    if (!e.arrayOn[LPos]) return;
    prepareDraw(e, mode == kPoints);
    e.real.glDrawElements(mode, count, type, idx);
}

// glDrawTex{f,i,s,x}OES: a screen-aligned textured quad at window coordinates,
// using each enabled unit's crop rectangle.
void drawTex(float x, float y, float z, float w, float h) {
    Gles1Emu& e = E();
    Fns& r = e.real;
    int32_t vp[4] = {0, 0, 1, 1};
    r.glGetIntegerv(kViewport, vp);
    if (vp[2] <= 0 || vp[3] <= 0 || w <= 0 || h <= 0) return;
    const float zc = std::clamp(z, 0.0f, 1.0f) * 2 - 1;
    const float x0 = 2 * (x - vp[0]) / vp[2] - 1, y0 = 2 * (y - vp[1]) / vp[3] - 1;
    const float x1 = 2 * (x + w - vp[0]) / vp[2] - 1, y1 = 2 * (y + h - vp[1]) / vp[3] - 1;
    float verts[4 * 3] = {x0, y0, zc, x1, y0, zc, x0, y1, zc, x1, y1, zc};
    float tc[kUnits][4 * 2] = {};
    for (int u = 0; u < kUnits; ++u) {
        auto it = e.crop.find(e.boundTex[u]);
        float s0 = 0, t0 = 0, s1 = 1, t1 = 1;
        if (it != e.crop.end()) {
            // Crop rect is in texels; the texture size isn't tracked, so use the
            // GL query on that unit.
            int32_t tw = 1, th = 1;
            r.glActiveTexture(kTexture0 + uint32_t(u));
            if (r.glGetTexParameteriv) { /* ES has no level size query; fall back below */ }
            (void)tw; (void)th;
            const auto& c = it->second;
            // Best effort without the texture size: treat the crop as already normalized
            // when it fits in [0,1], otherwise assume power-of-two max of the crop extents.
            int32_t size = 1;
            while (size < std::max(std::abs(c[0]) + std::abs(c[2]), std::abs(c[1]) + std::abs(c[3]))) size <<= 1;
            s0 = float(c[0]) / size; t0 = float(c[1]) / size;
            s1 = float(c[0] + c[2]) / size; t1 = float(c[1] + c[3]) / size;
        }
        const float t[8] = {s0, t0, s1, t0, s0, t1, s1, t1};
        std::memcpy(tc[u], t, sizeof t);
    }
    r.glActiveTexture(kTexture0 + e.activeUnit);
    // Draw with identity matrices from client memory, then restore the arrays.
    const int32_t prevBuf = [&] { int32_t b = 0; r.glGetIntegerv(kArrayBufferBinding, &b); return b; }();
    r.glBindBuffer(kArrayBuffer, 0);
    const std::vector<Mat4> mv = e.mv, proj = e.proj, t0 = e.tex[0], t1 = e.tex[1];
    e.mv = {Mat4::identity()}; e.proj = {Mat4::identity()}; e.tex[0] = {Mat4::identity()}; e.tex[1] = {Mat4::identity()};
    const bool saveLight = e.lighting, saveFog = e.fog;
    bool saveClip[kPlanes];
    std::memcpy(saveClip, e.clipOn, sizeof saveClip);
    e.lighting = false; e.fog = false;
    std::memset(e.clipOn, 0, sizeof e.clipOn);
    r.glVertexAttribPointer(LPos, 3, kFloat, 0, 0, verts);
    r.glEnableVertexAttribArray(LPos);
    for (int u = 0; u < kUnits; ++u) {
        r.glVertexAttribPointer(LTex0 + uint32_t(u), 2, kFloat, 0, 0, tc[u]);
        r.glEnableVertexAttribArray(LTex0 + uint32_t(u));
    }
    const bool colorOn = e.arrayOn[LColor];
    if (colorOn) r.glDisableVertexAttribArray(LColor);
    syncCurrent(e, LColor);
    prepareDraw(e, false);
    r.glDrawArrays(kTriangleStrip, 0, 4);
    // restore
    e.mv = mv; e.proj = proj; e.tex[0] = t0; e.tex[1] = t1;
    e.lighting = saveLight; e.fog = saveFog;
    std::memcpy(e.clipOn, saveClip, sizeof saveClip);
    for (int loc = 0; loc < 6; ++loc) {
        if (e.arrayOn[loc]) {
            r.glBindBuffer(kArrayBuffer, 0);
            r.glVertexAttribPointer(uint32_t(loc), e.arraySize[loc], e.arrayType[loc],
                                    (loc == LColor || loc == LNormal) && e.arrayType[loc] != kFloat && e.arrayType[loc] != kFixed,
                                    e.arrayStride[loc], e.arrayPtr[loc]);
            r.glEnableVertexAttribArray(uint32_t(loc));
        } else if (loc == LPos || loc == LTex0 || loc == LTex1) {
            r.glDisableVertexAttribArray(uint32_t(loc));
        }
    }
    r.glBindBuffer(kArrayBuffer, uint32_t(prevBuf));
}
void eDrawTexfOES(float x, float y, float z, float w, float h) { drawTex(x, y, z, w, h); }
void eDrawTexiOES(int32_t x, int32_t y, int32_t z, int32_t w, int32_t h) { drawTex(float(x), float(y), float(z), float(w), float(h)); }
void eDrawTexxOES(int32_t x, int32_t y, int32_t z, int32_t w, int32_t h) {
    drawTex(x / 65536.0f, y / 65536.0f, z / 65536.0f, w / 65536.0f, h / 65536.0f);
}

} // namespace

void installGles1Emulation(HostContext& hc) {
    auto e = std::make_shared<Gles1Emu>();
    e->real = hc.fn;
    Fns& f = hc.fn;
    // ES 1.1 extension spellings -> the ES 2 core functions.
    f.glBindFramebufferOES = f.glBindFramebuffer; f.glBindRenderbufferOES = f.glBindRenderbuffer;
    f.glCheckFramebufferStatusOES = f.glCheckFramebufferStatus; f.glDeleteFramebuffersOES = f.glDeleteFramebuffers;
    f.glDeleteRenderbuffersOES = f.glDeleteRenderbuffers; f.glFramebufferRenderbufferOES = f.glFramebufferRenderbuffer;
    f.glFramebufferTexture2DOES = f.glFramebufferTexture2D; f.glGenFramebuffersOES = f.glGenFramebuffers;
    f.glGenRenderbuffersOES = f.glGenRenderbuffers; f.glGenerateMipmapOES = f.glGenerateMipmap;
    f.glGetFramebufferAttachmentParameterivOES = f.glGetFramebufferAttachmentParameteriv;
    f.glGetRenderbufferParameterivOES = f.glGetRenderbufferParameteriv; f.glIsFramebufferOES = f.glIsFramebuffer;
    f.glIsRenderbufferOES = f.glIsRenderbuffer; f.glRenderbufferStorageOES = f.glRenderbufferStorage;
    f.glBlendEquationOES = f.glBlendEquation; f.glBlendEquationSeparateOES = f.glBlendEquationSeparate;
    f.glBlendFuncSeparateOES = f.glBlendFuncSeparate;
    // Shared entry points with fixed-function behaviour.
    f.glEnable = eEnable; f.glDisable = eDisable; f.glIsEnabled = eIsEnabled;
    f.glActiveTexture = eActiveTexture; f.glBindTexture = eBindTexture; f.glDeleteTextures = eDeleteTextures;
    f.glTexImage2D = eTexImage2D; f.glTexSubImage2D = eTexSubImage2D; f.glCompressedTexImage2D = eCompressedTexImage2D;
    f.glCopyTexImage2D = eCopyTexImage2D;
    f.glTexParameteri = eTexParameteri; f.glTexParameterf = eTexParameterf;
    f.glTexParameteriv = eTexParameteriv; f.glTexParameterfv = eTexParameterfv;
    f.glGetIntegerv = eGetIntegerv; f.glGetFloatv = eGetFloatv; f.glGetBooleanv = eGetBooleanv; f.glGetPointerv = eGetPointerv;
    f.glDrawArrays = eDrawArrays; f.glDrawElements = eDrawElements; f.glHint = eHint;
    // ES 1.1 only.
    f.glAlphaFunc = eAlphaFunc; f.glClientActiveTexture = eClientActiveTexture;
    f.glColor4f = eColor4f; f.glColor4ub = eColor4ub; f.glColorPointer = eColorPointer;
    f.glDisableClientState = eDisableClientState; f.glEnableClientState = eEnableClientState;
    f.glFogf = eFogf; f.glFogfv = eFogfv; f.glFrustumf = eFrustumf; f.glOrthof = eOrthof;
    f.glClipPlanef = eClipPlanef; f.glGetClipPlanef = eGetClipPlanef;
    f.glGetLightfv = eGetLightfv; f.glGetMaterialfv = eGetMaterialfv;
    f.glGetTexEnvfv = eGetTexEnvfv; f.glGetTexEnviv = eGetTexEnviv;
    f.glLightModelf = eLightModelf; f.glLightModelfv = eLightModelfv; f.glLightf = eLightf; f.glLightfv = eLightfv;
    f.glLoadIdentity = eLoadIdentity; f.glLoadMatrixf = eLoadMatrixf; f.glLogicOp = eLogicOp;
    f.glMaterialf = eMaterialf; f.glMaterialfv = eMaterialfv; f.glMatrixMode = eMatrixMode; f.glMultMatrixf = eMultMatrixf;
    f.glMultiTexCoord4f = eMultiTexCoord4f; f.glNormal3f = eNormal3f; f.glNormalPointer = eNormalPointer;
    f.glPointParameterf = ePointParameterf; f.glPointParameterfv = ePointParameterfv; f.glPointSize = ePointSize;
    f.glPopMatrix = ePopMatrix; f.glPushMatrix = ePushMatrix; f.glRotatef = eRotatef; f.glScalef = eScalef;
    f.glShadeModel = eShadeModel; f.glTexCoordPointer = eTexCoordPointer;
    f.glTexEnvf = eTexEnvf; f.glTexEnvfv = eTexEnvfv; f.glTexEnvi = eTexEnvi; f.glTexEnviv = eTexEnviv;
    f.glTranslatef = eTranslatef; f.glVertexPointer = eVertexPointer; f.glPointSizePointerOES = ePointSizePointerOES;
    f.glDrawTexfOES = eDrawTexfOES; f.glDrawTexiOES = eDrawTexiOES; f.glDrawTexsOES = eDrawTexiOES; f.glDrawTexxOES = eDrawTexxOES;
    // Not the desktop path: the ES 1.1 spellings above are the ones to use.
    f.glOrtho = nullptr; f.glFrustum = nullptr; f.glClipPlane = nullptr; f.glGetClipPlane = nullptr;
    hc.gles1 = e;
}

// The context's initial generic attribute values match ES 1.1 defaults once it is
// current; called on first draw would also work, but the attribute defaults are
// per context, so they're set when the emulation is installed and first bound.
} // namespace Hydrogen::Runtime::HostGL
